"""Feral front-end functions and data in a game binary.

The engine file `feral_frontend.toml` names the functions and data the runtime replaces, calls or
reads. Functions are found in a target binary by a masked byte signature (taken from the reference
build); data addresses are found as PIC base + displacement of the nth matching instruction inside
a named anchor function.

    python -m tools.game.feral refresh <reference binary> [toml]
"""
from __future__ import annotations

import re
import sys
import tomllib
from bisect import bisect_right
from dataclasses import dataclass, replace
from enum import StrEnum
from pathlib import Path
from typing import Any, cast

import capstone
from capstone import x86

from tools.hobbit import macho

DEFAULT_TOML = Path(__file__).resolve().parent / "feral_frontend.toml"
SIG_BYTES = 96
CALL_NEXT = b"\xe8\x00\x00\x00\x00"  # call $+5, the PIC base idiom
_ADDR_MASK = 0xFFFFFFFF
NUMAIN_ENTRY = "NuMain"  # a name `absent` may hold besides the toml entries
_DISP_MASK_MIN = 0x1000  # a 4-byte displacement at least this large is an address, not a frame offset
_CALLER_SAVED = ("eax", "ecx", "edx")
_REG_FAMILY = {
    "eax": {"eax", "ax", "al", "ah"}, "ecx": {"ecx", "cx", "cl", "ch"},
    "edx": {"edx", "dx", "dl", "dh"}, "ebx": {"ebx", "bx", "bl", "bh"},
    "esi": {"esi", "si"}, "edi": {"edi", "di"}, "ebp": {"ebp", "bp"},
}
_ENTRY_START = re.compile(r"^\[\[", re.MULTILINE)


class FeralError(Exception):
    pass


class Role(StrEnum):
    FRONTEND = "frontend"
    PAD = "pad"
    CALL = "call"
    ANCHOR = "anchor"


class Rule(StrEnum):
    PINNED = "pinned"
    UNIQUE = "unique"
    AFTER = "after"
    NEXT = "next"


@dataclass(frozen=True)
class FunctionEntry:
    name: str
    role: Role
    ref: int
    after: str | None = None
    next: str | None = None
    sig: str | None = None


@dataclass(frozen=True)
class DataEntry:
    name: str
    ref: int
    func: str
    insn: str | None = None
    nth: int | None = None
    section: str | None = None


@dataclass(frozen=True)
class Frontend:
    header: str
    functions: tuple[FunctionEntry, ...]
    data: tuple[DataEntry, ...]


@dataclass(frozen=True)
class Resolved:
    va: int
    rule: Rule
    matches: int


@dataclass(frozen=True)
class Image:
    data: bytes
    parsed: macho.MachO
    text: macho.Section
    starts: list[int]
    lo: int
    hi: int
    sections: list[tuple[str, macho.Section]]  # ("SEG.sect", section)


def load_image(path: str | Path) -> Image:
    data = Path(path).read_bytes()
    parsed = macho.parse(data)
    sections = [(f"{seg.name}.{sec.name}", sec) for seg in parsed.segments for sec in seg.sections]
    text = next((sec for full, sec in sections if full.endswith(".__text")), None)
    if text is None:
        raise FeralError(f"{path}: no __text section")
    return Image(
        data=data,
        parsed=parsed,
        text=text,
        starts=sorted(parsed.function_starts),
        lo=min(s.vmaddr for s in parsed.segments if s.vmsize),
        hi=max(s.vmaddr + s.vmsize for s in parsed.segments),
        sections=sections,
    )


def section_of(img: Image, va: int) -> str | None:
    for full, sec in img.sections:
        if sec.addr <= va < sec.addr + sec.size:
            return full
    return None


def _in_text(img: Image, va: int) -> bool:
    return img.text.addr <= va < img.text.addr + img.text.size


def function_bytes(img: Image, va: int) -> bytes:
    """Bytes from `va` to the next function start (or the end of __text)."""
    if not _in_text(img, va):
        raise FeralError(f"0x{va:08X} is not inside __text")
    k = bisect_right(img.starts, va)
    end = img.starts[k] if k < len(img.starts) else img.text.addr + img.text.size
    end = min(end, img.text.addr + img.text.size)
    off = img.text.offset + (va - img.text.addr)
    return img.data[off:off + (end - va)]


def _disassembler() -> Any:
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    return md


def _variable_fields(insn: Any, lo: int, hi: int) -> list[tuple[int, int]]:
    """(offset, size) of the bytes of `insn` that differ between builds: rel32 of call/jmp/jcc,
    large memory displacements and 4-byte immediates that point into the image."""
    fields: list[tuple[int, int]] = []
    if insn.imm_size == 4:
        imm = next(op.imm for op in insn.operands if op.type == x86.X86_OP_IMM)
        if insn.group(capstone.CS_GRP_JUMP) or insn.group(capstone.CS_GRP_CALL) or lo <= imm & _ADDR_MASK < hi:
            fields.append((insn.imm_offset, 4))
    if insn.disp_size == 4:
        disp = next(op.mem.disp for op in insn.operands if op.type == x86.X86_OP_MEM)
        if abs(disp) >= _DISP_MASK_MIN:
            fields.append((insn.disp_offset, 4))
    return fields


def _mask(code: bytes, va: int, lo: int, hi: int) -> list[int | None]:
    """Whole instructions up to SIG_BYTES (the instruction that crosses it is kept whole);
    bytes after an undecodable point stay literal."""
    pat: list[int | None] = []
    for insn in _disassembler().disasm(code, va):
        if len(pat) >= SIG_BYTES:
            break
        raw: list[int | None] = list(insn.bytes)
        for off, size in _variable_fields(insn, lo, hi):
            raw[off:off + size] = [None] * size
        pat += raw
    if len(pat) < min(SIG_BYTES, len(code)):
        pat += list(code[len(pat):SIG_BYTES])
    return pat


def signature(img: Image, va: int) -> str:
    pat = _mask(function_bytes(img, va), va, img.lo, img.hi)
    return " ".join("??" if b is None else f"{b:02x}" for b in pat)


def find_matches(img: Image, sig: str) -> list[int]:
    parts = sig.split()
    rx = re.compile(
        b"".join(b"." if p == "??" else re.escape(bytes.fromhex(p)) for p in parts), re.DOTALL
    )
    text = img.text
    hits: list[int] = []
    for s in img.starts:
        if text.addr <= s < text.addr + text.size and rx.match(img.data, text.offset + (s - text.addr)):
            hits.append(s)
    return hits


def validate_absent(fe: Frontend, absent: frozenset[str]) -> None:
    known = {f.name for f in fe.functions} | {d.name for d in fe.data} | {NUMAIN_ENTRY}
    for name in sorted(absent - known):
        raise FeralError(f"absent names unknown entry {name}")


def resolve(
    img: Image, fe: Frontend, pins: dict[str, int], absent: frozenset[str] = frozenset()
) -> dict[str, Resolved]:
    """Find every function of `fe` in `img` that is not absent: pin, else unique signature match,
    else `after` rule (2+ matches), else `next` rule."""
    known = {f.name for f in fe.functions}
    for name in pins:
        if name not in known:
            raise FeralError(f"pin for unknown function {name}")
    resolved: dict[str, Resolved] = {}
    pending: list[tuple[FunctionEntry, Rule, str, list[int]]] = []
    for f in fe.functions:
        if f.name in absent:
            continue
        if f.name in pins:
            resolved[f.name] = Resolved(pins[f.name], Rule.PINNED, 0)
            continue
        if f.sig is None:
            raise FeralError(f"{f.name}: no signature in the toml (run refresh)")
        hits = find_matches(img, f.sig)
        if len(hits) == 1:
            resolved[f.name] = Resolved(hits[0], Rule.UNIQUE, 1)
        elif len(hits) >= 2 and f.after is not None:
            pending.append((f, Rule.AFTER, f.after, hits))
        elif f.next is not None:
            pending.append((f, Rule.NEXT, f.next, hits))
        else:
            raise FeralError(
                f"{f.name}: signature matches {len(hits)} functions and no 'after' or 'next' rule resolves it"
            )
    for f, rule, anchor_name, _hits in pending:
        if anchor_name not in known:
            raise FeralError(f"{f.name}: '{rule}' names unknown function {anchor_name}")
    while pending:
        progress = False
        for item in list(pending):
            f, rule, anchor_name, hits = item
            anchor = resolved.get(anchor_name)
            if anchor is None:
                continue
            if rule is Rule.AFTER:
                pick = next((c for c in hits if c > anchor.va), None)
            else:
                k = bisect_right(img.starts, anchor.va)
                pick = img.starts[k] if k < len(img.starts) else None
                if pick is not None and hits and pick not in hits:
                    raise FeralError(
                        f"{f.name}: rule 'next {anchor_name}' picks 0x{pick:08X}, "
                        f"which is not one of the {len(hits)} signature matches"
                    )
            if pick is None:
                raise FeralError(
                    f"{f.name}: rule '{rule} {anchor_name}' found no function above 0x{anchor.va:08X}"
                )
            resolved[f.name] = Resolved(pick, rule, len(hits))
            pending.remove(item)
            progress = True
        if not progress:
            names = ", ".join(item[0].name for item in pending)
            raise FeralError(f"'after'/'next' rules cannot be resolved (cycle or unresolved anchor): {names}")
    owner: dict[int, str] = {}
    for f in fe.functions:
        if f.name in absent:
            continue
        va = resolved[f.name].va
        if va in owner:
            raise FeralError(f"{owner[va]} and {f.name} both resolve to 0x{va:08X}")
        owner[va] = f.name
    return {f.name: resolved[f.name] for f in fe.functions if f.name not in absent}


@dataclass(frozen=True)
class PicOperand:
    """An instruction with one based memory operand whose base register holds a PIC base."""
    insn: Any  # capstone CsInsn
    disp: int
    base: int  # the PIC base (address after `call $+5`) that the base register holds


def _written(insn: Any) -> set[str]:
    names = {cast(str, insn.reg_name(r)) for r in insn.regs_access()[1]}
    if insn.group(capstone.CS_GRP_CALL):
        names |= set(_CALLER_SAVED)
    return names


def _pic_operands(img: Image, va: int) -> list[PicOperand]:
    return _track_pic(list(_disassembler().disasm(function_bytes(img, va), va)))


def _track_pic(insns: list[Any]) -> list[PicOperand]:
    """Instructions with exactly one based memory operand whose base register holds a PIC base, in
    address order. A register holds a PIC base from `call $+5; pop reg` until it is written, and
    through `mov` copies and `[ebp+disp]` spills; a spill slot holds it until it is written."""
    held: dict[str, int] = {}  # register -> PIC base it holds
    slots: dict[int, int] = {}  # [ebp+disp] -> PIC base it holds
    found: list[PicOperand] = []
    for k, insn in enumerate(insns):
        name = insn.reg_name
        based = [op for op in insn.operands if op.type == x86.X86_OP_MEM and op.mem.base != 0]
        pic = [op for op in based if name(op.mem.base) in held]
        if len(pic) == 1:
            found.append(PicOperand(insn, pic[0].mem.disp, held[name(pic[0].mem.base)]))
        if bytes(insn.bytes) == CALL_NEXT:
            continue  # the PIC base idiom clobbers nothing
        if insn.mnemonic == "pop" and k > 0 and bytes(insns[k - 1].bytes) == CALL_NEXT:
            held[name(insn.operands[0].reg)] = insns[k - 1].address + 5
            continue
        received: int | None = None  # PIC base that the destination register of a `mov` receives
        stored: tuple[int, int] | None = None  # (slot, PIC base) written by `mov [ebp+disp], reg`
        if insn.mnemonic == "mov" and len(insn.operands) == 2:
            dst, src = insn.operands
            if dst.type == x86.X86_OP_REG and src.type == x86.X86_OP_REG:
                received = held.get(name(src.reg))
            elif dst.type == x86.X86_OP_REG and src.type == x86.X86_OP_MEM and name(src.mem.base) == "ebp":
                received = slots.get(src.mem.disp)
            elif dst.type == x86.X86_OP_MEM and src.type == x86.X86_OP_REG and name(dst.mem.base) == "ebp":
                if name(src.reg) in held:
                    stored = (dst.mem.disp, held[name(src.reg)])
        for op in insn.operands:
            if op.type == x86.X86_OP_MEM and name(op.mem.base) == "ebp" and op.access & capstone.CS_AC_WRITE:
                slots.pop(op.mem.disp, None)
        if stored is not None:
            slots[stored[0]] = stored[1]
        written = _written(insn)
        for r in [r for r in held if _REG_FAMILY.get(r, {r}) & written]:
            del held[r]
        if received is not None:
            held[name(insn.operands[0].reg)] = received
    return found


def refresh(fe: Frontend, img: Image) -> Frontend:
    """Fill sig (functions) and insn/nth/section (data) from the reference binary."""
    functions = tuple(replace(f, sig=signature(img, f.ref)) for f in fe.functions)
    refs = {f.name: f.ref for f in fe.functions}
    cache: dict[str, list[PicOperand]] = {}
    data: list[DataEntry] = []
    for d in fe.data:
        if d.func not in refs:
            raise FeralError(f"{d.name}: anchor function {d.func} is not in the toml")
        if d.func not in cache:
            cache[d.func] = _pic_operands(img, refs[d.func])
        ops = cache[d.func]
        hit = next((o for o in ops if (o.base + o.disp) & _ADDR_MASK == d.ref), None)
        if hit is None:
            raise FeralError(f"{d.name}: no PIC-based operand in {d.func} equals 0x{d.ref:08X}")
        section = section_of(img, d.ref)
        if section is None:
            raise FeralError(f"{d.name}: 0x{d.ref:08X} is in no section")
        mnemonic = str(hit.insn.mnemonic)
        nth = [o for o in ops if o.insn.mnemonic == mnemonic].index(hit)
        data.append(DataEntry(d.name, d.ref, d.func, mnemonic, nth, section))
    return Frontend(fe.header, functions, tuple(data))


def derive_data(
    img: Image, fe: Frontend, resolved: dict[str, Resolved], absent: frozenset[str] = frozenset()
) -> dict[str, int]:
    """Find every data address of `fe` in `img` that is not absent, using the anchors written by refresh."""
    cache: dict[str, list[PicOperand]] = {}
    values: dict[str, int] = {}
    for d in fe.data:
        if d.name in absent:
            continue
        if d.insn is None or d.nth is None or d.section is None:
            raise FeralError(f"{d.name}: no anchor in the toml (run refresh)")
        if d.func not in resolved:
            raise FeralError(f"{d.name}: anchor function {d.func} is not resolved")
        if d.func not in cache:
            cache[d.func] = _pic_operands(img, resolved[d.func].va)
        same = [o for o in cache[d.func] if o.insn.mnemonic == d.insn]
        if d.nth >= len(same):
            raise FeralError(f"{d.name}: {d.func} has no operand #{d.nth} of '{d.insn}' on a PIC base")
        value = (same[d.nth].base + same[d.nth].disp) & _ADDR_MASK
        section = next((sec for full, sec in img.sections if full == d.section), None)
        if section is None or not section.addr <= value < section.addr + section.size:
            raise FeralError(f"{d.name}: 0x{value:08X} is not inside section {d.section}")
        values[d.name] = value
    return values


def load_file(path: str | Path) -> Frontend:
    text = Path(path).read_text()
    first = _ENTRY_START.search(text)
    header = text[:first.start()].rstrip("\n") if first else text.rstrip("\n")
    raw = tomllib.loads(text)
    functions: list[FunctionEntry] = []
    for item in cast(list[dict[str, Any]], raw.get("function", [])):
        functions.append(
            FunctionEntry(
                item["name"], Role(item["role"]), item["ref"], item.get("after"), item.get("next"), item.get("sig")
            )
        )
    data: list[DataEntry] = []
    for item in cast(list[dict[str, Any]], raw.get("data", [])):
        data.append(
            DataEntry(item["name"], item["ref"], item["func"], item.get("insn"), item.get("nth"), item.get("section"))
        )
    return Frontend(header, tuple(functions), tuple(data))


def render(fe: Frontend) -> str:
    blocks: list[str] = []
    for f in fe.functions:
        lines = ["[[function]]", f'name = "{f.name}"', f'role = "{f.role}"', f"ref = 0x{f.ref:08X}"]
        if f.after is not None:
            lines.append(f'after = "{f.after}"')
        if f.next is not None:
            lines.append(f'next = "{f.next}"')
        if f.sig is not None:
            lines.append(f'sig = "{f.sig}"')
        blocks.append("\n".join(lines))
    for d in fe.data:
        lines = ["[[data]]", f'name = "{d.name}"', f"ref = 0x{d.ref:08X}", f'func = "{d.func}"']
        if d.insn is not None:
            lines.append(f'insn = "{d.insn}"')
        if d.nth is not None:
            lines.append(f"nth = {d.nth}")
        if d.section is not None:
            lines.append(f'section = "{d.section}"')
        blocks.append("\n".join(lines))
    return fe.header + "\n\n" + "\n\n".join(blocks) + "\n"


def main(argv: list[str]) -> int:
    if len(argv) not in (2, 3) or argv[0] != "refresh":
        print("usage: python -m tools.game.feral refresh <reference binary> [toml]", file=sys.stderr)
        return 2
    toml_path = Path(argv[2]) if len(argv) == 3 else DEFAULT_TOML
    try:
        refreshed = refresh(load_file(toml_path), load_image(argv[1]))
    except (FeralError, OSError) as exc:
        print(f"feral: {exc}", file=sys.stderr)
        return 1
    toml_path.write_text(render(refreshed))
    print(f"wrote {toml_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
