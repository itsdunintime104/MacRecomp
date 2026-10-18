"""Feral front-end functions and data in a game binary.

The engine file `feral_frontend.toml` names the functions and data the runtime replaces, calls or
reads. Functions are found in a target binary by a masked byte signature (taken from the reference
build); data addresses are found as PIC base + displacement inside those functions.

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
_CALL_OPCODES = (0xE8, 0xE9)
_ADDR_MASK = 0xFFFFFFFF
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


@dataclass(frozen=True)
class FunctionEntry:
    name: str
    role: Role
    ref: int
    after: str | None = None
    sig: str | None = None


@dataclass(frozen=True)
class DataEntry:
    name: str
    ref: int
    func: str | None = None
    offset: int | None = None
    base: int | None = None
    insn: str | None = None
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


def _mask(code: bytes, va: int, lo: int, hi: int) -> list[int | None]:
    pat: list[int | None] = list(code)
    i = 0
    while i + 4 <= len(code):
        v = int.from_bytes(code[i:i + 4], "little")
        rel = (va + i + 4 + v) & _ADDR_MASK  # rel32 target if this were a call/jmp operand
        if lo <= v < hi or (i > 0 and code[i - 1] in _CALL_OPCODES and lo <= rel < hi):
            pat[i:i + 4] = [None] * 4
            i += 4
        else:
            i += 1
    return pat


def signature(img: Image, va: int) -> str:
    code = function_bytes(img, va)[:SIG_BYTES]
    return " ".join("??" if b is None else f"{b:02x}" for b in _mask(code, va, img.lo, img.hi))


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


def resolve(img: Image, fe: Frontend, pins: dict[str, int]) -> dict[str, Resolved]:
    """Find every function of `fe` in `img`: pin, else unique signature match, else `after` rule."""
    known = {f.name for f in fe.functions}
    for name in pins:
        if name not in known:
            raise FeralError(f"pin for unknown function {name}")
    resolved: dict[str, Resolved] = {}
    candidates: dict[str, list[int]] = {}
    pending: list[FunctionEntry] = []
    for f in fe.functions:
        if f.name in pins:
            resolved[f.name] = Resolved(pins[f.name], Rule.PINNED, 0)
            continue
        if f.sig is None:
            raise FeralError(f"{f.name}: no signature in the toml (run refresh)")
        hits = find_matches(img, f.sig)
        if len(hits) == 1:
            resolved[f.name] = Resolved(hits[0], Rule.UNIQUE, 1)
        elif len(hits) >= 2 and f.after is not None:
            if f.after not in known:
                raise FeralError(f"{f.name}: 'after' names unknown function {f.after}")
            candidates[f.name] = hits
            pending.append(f)
        else:
            raise FeralError(
                f"{f.name}: signature matches {len(hits)} functions and no 'after' rule resolves it"
            )
    while pending:
        progress = False
        for f in list(pending):
            anchor = resolved.get(cast(str, f.after))
            if anchor is None:
                continue
            hits = candidates[f.name]
            pick = next((c for c in hits if c > anchor.va), None)
            if pick is None:
                raise FeralError(
                    f"{f.name}: rule 'after {f.after}' found none of {len(hits)} matches above 0x{anchor.va:08X}"
                )
            resolved[f.name] = Resolved(pick, Rule.AFTER, len(hits))
            pending.remove(f)
            progress = True
        if not progress:
            names = ", ".join(f.name for f in pending)
            raise FeralError(f"'after' rules cannot be resolved (cycle or unresolved anchor): {names}")
    owner: dict[int, str] = {}
    for f in fe.functions:
        va = resolved[f.name].va
        if va in owner:
            raise FeralError(f"{owner[va]} and {f.name} both resolve to 0x{va:08X}")
        owner[va] = f.name
    return {f.name: resolved[f.name] for f in fe.functions}


@dataclass(frozen=True)
class Decoded:
    insns: list[Any]  # capstone CsInsn
    pic_bases: list[int]


def _decode(img: Image, va: int) -> Decoded:
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    insns = list(md.disasm(function_bytes(img, va), va))
    bases = [
        i.address + 5
        for i, j in zip(insns, insns[1:])
        if bytes(i.bytes) == CALL_NEXT and j.mnemonic == "pop"
    ]
    return Decoded(insns, bases)


def _mem_operands(insn: Any) -> list[Any]:
    return [
        op for op in insn.operands if op.type == x86.X86_OP_MEM and op.mem.base != 0
    ]


def _find_anchor(
    cache: dict[str, Decoded], fe: Frontend, ref: int
) -> tuple[FunctionEntry, int, int, str] | None:
    """Smallest-offset (function, offset, base, mnemonic) with PIC base + disp == ref."""
    best: tuple[FunctionEntry, int, int, str] | None = None
    for f in fe.functions:
        dec = cache[f.name]
        for insn in dec.insns:
            offset = insn.address - f.ref
            if best is not None and offset >= best[1]:
                break
            hit = next(
                (
                    base
                    for op in _mem_operands(insn)
                    for base in dec.pic_bases
                    if base <= insn.address and (base + op.mem.disp) & _ADDR_MASK == ref
                ),
                None,
            )
            if hit is not None:
                best = (f, offset, hit - f.ref, str(insn.mnemonic))
                break
    return best


def refresh(fe: Frontend, img: Image) -> Frontend:
    """Fill sig (functions) and func/offset/base/insn/section (data) from the reference binary."""
    functions = tuple(replace(f, sig=signature(img, f.ref)) for f in fe.functions)
    cache = {f.name: _decode(img, f.ref) for f in fe.functions}
    data: list[DataEntry] = []
    for d in fe.data:
        found = _find_anchor(cache, fe, d.ref)
        if found is None:
            raise FeralError(f"{d.name}: no PIC base + displacement equals 0x{d.ref:08X}")
        section = section_of(img, d.ref)
        if section is None:
            raise FeralError(f"{d.name}: 0x{d.ref:08X} is in no section")
        func, offset, base, mnemonic = found
        data.append(DataEntry(d.name, d.ref, func.name, offset, base, mnemonic, section))
    return Frontend(fe.header, functions, tuple(data))


def derive_data(img: Image, fe: Frontend, resolved: dict[str, Resolved]) -> dict[str, int]:
    """Find every data address of `fe` in `img` using the anchors written by refresh."""
    cache: dict[str, Decoded] = {}
    values: dict[str, int] = {}
    for d in fe.data:
        if d.func is None or d.offset is None or d.base is None or d.insn is None or d.section is None:
            raise FeralError(f"{d.name}: no anchor in the toml (run refresh)")
        if d.func not in resolved:
            raise FeralError(f"{d.name}: anchor function {d.func} is not in the toml")
        va = resolved[d.func].va
        if d.func not in cache:
            cache[d.func] = _decode(img, va)
        dec = cache[d.func]
        if va + d.base not in dec.pic_bases:
            raise FeralError(f"{d.name}: no PIC base (call $+5; pop) at {d.func}+0x{d.base:X}")
        insn = next((i for i in dec.insns if i.address == va + d.offset), None)
        if insn is None or insn.mnemonic != d.insn:
            raise FeralError(f"{d.name}: no '{d.insn}' instruction at {d.func}+0x{d.offset:X}")
        mems = _mem_operands(insn)
        if len(mems) != 1:
            raise FeralError(f"{d.name}: instruction at {d.func}+0x{d.offset:X} has {len(mems)} based memory operands")
        value = (va + d.base + mems[0].mem.disp) & _ADDR_MASK
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
            FunctionEntry(item["name"], Role(item["role"]), item["ref"], item.get("after"), item.get("sig"))
        )
    data: list[DataEntry] = []
    for item in cast(list[dict[str, Any]], raw.get("data", [])):
        data.append(
            DataEntry(
                item["name"], item["ref"], item.get("func"), item.get("offset"), item.get("base"),
                item.get("insn"), item.get("section"),
            )
        )
    return Frontend(header, tuple(functions), tuple(data))


def render(fe: Frontend) -> str:
    blocks: list[str] = []
    for f in fe.functions:
        lines = ["[[function]]", f'name = "{f.name}"', f'role = "{f.role}"', f"ref = 0x{f.ref:08X}"]
        if f.after is not None:
            lines.append(f'after = "{f.after}"')
        if f.sig is not None:
            lines.append(f'sig = "{f.sig}"')
        blocks.append("\n".join(lines))
    for d in fe.data:
        lines = ["[[data]]", f'name = "{d.name}"', f"ref = 0x{d.ref:08X}"]
        if d.func is not None:
            lines.append(f'func = "{d.func}"')
        if d.offset is not None:
            lines.append(f"offset = 0x{d.offset:X}")
        if d.base is not None:
            lines.append(f"base = 0x{d.base:X}")
        if d.insn is not None:
            lines.append(f'insn = "{d.insn}"')
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
