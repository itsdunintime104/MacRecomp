"""Build recon/analysis.json from a 32-bit PE32 executable.

Usage: python -I analyze_pe.py <exe> --ghidra-functions <json> \
           --ghidra-data <json> [--drop-starts <txt>] [--add-starts <txt>] \
           [--init-tables <txt>] -o <analysis.json>

The engine's sweeper, switch-table finder and lifter all read the Mach-O
analyzer's schema, so a PE is described in that same schema:

  * one segment per PE section, because each PE section has its own raw file
    offset and a single linear segment would map the wrong bytes;
  * Mach-O section names (__text, __const, __data, __bss), because the
    sweeper selects code by the name "__text" and the absolute switch-table
    finder matches "__DATA,__const" and "__DATA,__data";
  * the zero-filled tail of a section whose virtual size exceeds its raw size
    becomes a separate __bss section with no file bytes.

A stripped PE has no function-starts table. Ghidra's analysis
(tools/ghidra/ExportPE.java) supplies candidate starts, and the data items that
sit inside the code section (MSVC emits switch jump tables there). The kept
starts and their extents come from a CFG walk of the code (find_functions).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import bisect
import re
import struct
from collections.abc import Container, Mapping, Sequence, Set
from enum import StrEnum
from pathlib import Path
from typing import Any, NamedTuple

IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_MEM_WRITE = 0x80000000
MACHO_S_ZEROFILL = 0x01

# PE section name -> (Mach-O segment, Mach-O section). Anything not listed
# keeps its own name under __DATA.
SECTION_MAP = {
    ".text": ("__TEXT", "__text"),
    ".rdata": ("__DATA", "__const"),
    ".data": ("__DATA", "__data"),
    ".idata": ("__DATA", "__idata"),
    ".rsrc": ("__RSRC", "__rsrc"),
}


class PE:
    def __init__(self, data: bytes) -> None:
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("not an MZ image")
        nt = struct.unpack_from("<I", data, 0x3C)[0]
        if data[nt:nt + 4] != b"PE\0\0":
            raise ValueError("no PE signature")
        coff = nt + 4
        (self.machine, nsec, self.timestamp, _, _, optsz,
         self.characteristics) = struct.unpack_from("<HHIIIHH", data, coff)
        if self.machine != 0x014C:
            raise ValueError(f"not i386: machine=0x{self.machine:04x}")
        opt = coff + 20
        if struct.unpack_from("<H", data, opt)[0] != 0x10B:
            raise ValueError("not PE32 (optional header magic)")
        self.entry_rva = struct.unpack_from("<I", data, opt + 16)[0]
        self.image_base = struct.unpack_from("<I", data, opt + 28)[0]
        ndd = struct.unpack_from("<I", data, opt + 92)[0]
        self.dirs = [struct.unpack_from("<II", data, opt + 96 + 8 * i)
                     for i in range(ndd)]
        self.sections = []
        off = opt + optsz
        for i in range(nsec):
            name, vsize, va, rsize, roff, _, _, _, _, flags = \
                struct.unpack_from("<8sIIIIIIHHI", data, off + 40 * i)
            self.sections.append({
                "name": name.rstrip(b"\0").decode("latin1"),
                "vsize": vsize, "va": va, "rsize": rsize,
                "roff": roff, "flags": flags,
            })

    @property
    def entry(self) -> int:
        return self.image_base + self.entry_rva

    def section(self, name: str) -> dict[str, Any]:
        return next(s for s in self.sections if s["name"] == name)

    def exports(self) -> list[int]:
        """VAs of the exported functions (forwarders aside)."""
        rva, _ = self.dirs[0] if self.dirs else (0, 0)
        off = self.rva_to_off(rva) if rva else None
        if off is None:
            return []
        n, table = struct.unpack_from("<I8xI", self.data, off + 20)
        toff = self.rva_to_off(table)
        assert toff is not None
        return [self.image_base + r
                for r in struct.unpack_from(f"<{n}I", self.data, toff) if r]

    def rva_to_off(self, rva: int) -> int | None:
        for s in self.sections:
            span = max(s["vsize"], s["rsize"])
            if s["va"] <= rva < s["va"] + span:
                off = s["roff"] + (rva - s["va"])
                return off if off < len(self.data) else None
        return None

    def cstr(self, rva: int) -> str:
        off = self.rva_to_off(rva)
        if off is None:
            raise ValueError(f"string rva 0x{rva:x} outside the image")
        end = self.data.index(b"\0", off)
        return self.data[off:end].decode("latin1")

    def imports(self) -> list[dict[str, Any]]:
        """One row per imported function, in IAT slot order."""
        rva, _ = self.dirs[1]
        if not rva:
            return []
        out: list[dict[str, Any]] = []
        off = self.rva_to_off(rva)
        assert off is not None
        while True:
            oft, _, _, name_rva, first_thunk = struct.unpack_from(
                "<IIIII", self.data, off)
            if name_rva == 0:
                break
            dll = self.cstr(name_rva)
            lookup = self.rva_to_off(oft or first_thunk)
            assert lookup is not None
            slot_rva = first_thunk
            while True:
                value = struct.unpack_from("<I", self.data, lookup)[0]
                if value == 0:
                    break
                if value & 0x80000000:
                    out.append({"dll": dll, "name": None,
                                "ordinal": value & 0xFFFF,
                                "slot": self.image_base + slot_rva})
                else:
                    hint_off = self.rva_to_off(value & 0x7FFFFFFF)
                    assert hint_off is not None
                    end = self.data.index(b"\0", hint_off + 2)
                    out.append({
                        "dll": dll,
                        "name": self.data[hint_off + 2:end].decode("latin1"),
                        "ordinal": None,
                        "slot": self.image_base + slot_rva,
                    })
                lookup += 4
                slot_rva += 4
            off += 20
        return out


def _macho_name(pe_name: str) -> tuple[str, str]:
    if pe_name in SECTION_MAP:
        return SECTION_MAP[pe_name]
    return ("__DATA", "__" + pe_name.lstrip(".").lower())


def _segments(pe: PE) -> list[dict[str, Any]]:
    """One segment per PE section, splitting off any zero-filled tail."""
    segs: list[dict[str, Any]] = []
    for s in pe.sections:
        seg_name, sec_name = _macho_name(s["name"])
        va = pe.image_base + s["va"]
        file_bytes = min(s["rsize"], s["vsize"])
        is_code = bool(s["flags"] & IMAGE_SCN_MEM_EXECUTE)
        segs.append({
            "name": seg_name, "vmaddr": va, "vmsize": s["vsize"],
            "fileoff": s["roff"], "filesize": file_bytes,
            "initprot": 5 if is_code else 3, "maxprot": 7,
            "sections": [{
                "name": sec_name, "addr": va, "size": file_bytes,
                "offset": s["roff"], "flags": 0,
                "reserved1": 0, "reserved2": 0,
            }],
        })
        tail = s["vsize"] - file_bytes
        if tail > 0:
            segs.append({
                "name": seg_name, "vmaddr": va + file_bytes, "vmsize": tail,
                "fileoff": 0, "filesize": 0, "initprot": 3, "maxprot": 7,
                "sections": [{
                    "name": "__bss" if sec_name == "__data" else sec_name + "_bss",
                    "addr": va + file_bytes, "size": tail, "offset": 0,
                    "flags": MACHO_S_ZEROFILL, "reserved1": 0, "reserved2": 0,
                }],
            })
    return segs


class Kind(StrEnum):
    SEQ = "seq"
    RET = "ret"
    JMP = "jmp"        # direct jmp
    JCC = "jcc"        # conditional branch, loop, jecxz
    CALL = "call"      # call; target is None when indirect
    TABLE = "table"    # jmp [reg*4 + table]
    IJMP = "ijmp"      # any other indirect jmp
    STOP = "stop"      # int3, hlt, ud2, or zero bytes (data)


class Evidence(StrEnum):
    LISTED = "listed"  # PE entry, export, or recon/add_function_starts.txt
    THUNK = "thunk"    # target of a 5-byte `jmp rel32` thunk (ILT)
    DATA = "data"      # aligned dword in a data section
    CALL = "call"      # direct call from a kept body
    IMM = "imm"        # immediate operand in a kept body
    JUMP = "jump"      # jump target outside the jumping function's span
    ORPHAN = "orphan"  # candidate that no kept span covers


class Insn(NamedTuple):
    size: int
    kind: Kind
    target: int | None       # branch target, or the jump table's VA
    imms: tuple[int, ...]    # immediate operands
    pad: bool                # no-op fill (nop, lea r,[r], mov r,r)


_JCC = frozenset({"jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js",
                  "jns", "jp", "jnp", "jl", "jge", "jle", "jg", "jcxz",
                  "jecxz", "loop", "loope", "loopne"})
_HEX = re.compile(r"0x[0-9a-f]+")
_MEM = re.compile(r"\[[^\]]*\]")
_TABLE = re.compile(r"dword ptr \[e[a-z]{2}\*4 \+ (0x[0-9a-f]+)\]")
_PAD = re.compile(r"nop\b.*|lea (\w+), \[\1\]|mov (\w+), \2|xchg (\w+), \3")
MAX_TABLE = 512


def _classify(mnem: str, ops: str, head: bytes) -> tuple[Kind, int | None]:
    direct = int(ops, 16) if _HEX.fullmatch(ops) else None
    if mnem in ("int3", "hlt", "ud2") or head == b"\0\0":
        return Kind.STOP, None
    if mnem.startswith("ret") or mnem == "iretd":
        return Kind.RET, None
    if mnem == "call":
        return Kind.CALL, direct
    if mnem == "jmp":
        if direct is not None:
            return Kind.JMP, direct
        m = _TABLE.fullmatch(ops)
        return (Kind.TABLE, int(m.group(1), 16)) if m else (Kind.IJMP, None)
    if mnem in _JCC and direct is not None:
        return Kind.JCC, direct
    return Kind.SEQ, None


class Code:
    """Decoder over image bytes mapped at `base`; code is [lo, hi)."""

    def __init__(self, image: bytes, base: int, lo: int, hi: int) -> None:
        import capstone  # only the function-list pass needs it
        self.image, self.base, self.lo, self.hi = image, base, lo, hi
        self._cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self._insns: dict[int, Insn | None] = {}
        self._tables: dict[int, list[int]] = {}

    def at(self, va: int) -> Insn | None:
        if va in self._insns:
            return self._insns[va]
        insn = None
        if self.lo <= va < self.hi:
            off = va - self.base
            raw = self.image[off:off + 16]
            for _, size, mnem, ops in self._cs.disasm_lite(raw, va, 1):
                kind, target = _classify(mnem, ops, raw[:2])
                imms = (tuple(int(h, 16) for h in _HEX.findall(
                    _MEM.sub("", ops))) if kind is Kind.SEQ else ())
                insn = Insn(size, kind, target, imms,
                            bool(_PAD.fullmatch(f"{mnem} {ops}".strip())))
        self._insns[va] = insn
        return insn

    def kind(self, va: int) -> Kind | None:
        insn = self.at(va)
        return insn.kind if insn is not None else None

    def starts_code(self, va: int) -> bool:
        insn = self.at(va)
        return insn is not None and insn.kind is not Kind.STOP

    def dword(self, va: int) -> int | None:
        off = va - self.base
        if 0 <= off <= len(self.image) - 4:
            return struct.unpack_from("<I", self.image, off)[0]
        return None

    def table(self, va: int) -> list[int]:
        """Arms of the switch table at va: the dwords from va up to the
        first one that is not a code address. An index byte map or int3 fill
        after the table holds no code address, so it ends the run."""
        if va not in self._tables:
            arms: list[int] = []
            while len(arms) < MAX_TABLE:
                value = self.dword(va + 4 * len(arms))
                if value is None or not self.starts_code(value):
                    break
                arms.append(value)
            self._tables[va] = arms
        return self._tables[va]

    def is_padding(self, start: int, end: int) -> bool:
        """[start, end) is no-op fill, and short jumps across it."""
        va = start
        while va < end:
            insn = self.at(va)
            if insn is None or not (insn.pad or insn.kind is Kind.JMP
                                    and insn.target is not None
                                    and start < insn.target <= end):
                return False
            va += insn.size
        return True


class Body(NamedTuple):
    insns: dict[int, int]     # VA -> size of each instruction reached
    calls: frozenset[int]     # direct call targets
    jumps: frozenset[tuple[int, int]]   # (site, target) of jmp, jcc, arm
    arms: frozenset[int]      # switch-arm targets
    imms: frozenset[int]      # immediate operands
    end: int                  # end of the highest instruction reached


def walk(code: Code, start: int, stops: Container[int],
         noreturn: Set[int]) -> Body:
    """Recursive descent from start over fall-through, jcc, direct jmp and
    switch arms. Calls are not followed. A path ends at ret, int3, an
    indirect jmp that is not a switch, a call to a no-return function, or
    another kept start (`stops`), which the lifter makes a tail call."""
    insns: dict[int, int] = {}
    calls: set[int] = set()
    jumps: set[tuple[int, int]] = set()
    arms: set[int] = set()
    imms: set[int] = set()
    work = [start]
    while work:
        va = work.pop()
        while va not in insns and (va == start or va not in stops):
            insn = code.at(va)
            if insn is None or insn.kind is Kind.STOP:
                break
            insns[va] = insn.size
            imms.update(insn.imms)
            kind, target = insn.kind, insn.target
            if kind is Kind.RET or kind is Kind.IJMP:
                break
            if kind is Kind.JMP or kind is Kind.JCC:
                assert target is not None
                jumps.add((va, target))
                work.append(target)
                if kind is Kind.JMP:
                    break
            elif kind is Kind.TABLE:
                assert target is not None
                arms.update(code.table(target))
                jumps.update((va, arm) for arm in code.table(target))
                work.extend(code.table(target))
                break
            elif kind is Kind.CALL and target is not None:
                calls.add(target)
                if target in noreturn:
                    break
            va += insn.size
    end = max((va + n for va, n in insns.items()), default=start)
    return Body(insns, frozenset(calls), frozenset(jumps), frozenset(arms),
                frozenset(imms), end)


_POINTER = frozenset({Evidence.DATA, Evidence.IMM})


class Function(NamedTuple):
    end: int
    evidence: Evidence


def find_functions(code: Code, candidates: Set[int],
                   evidence: Mapping[int, Evidence], noreturn: Set[int],
                   drop: Set[int], data: Set[int] = frozenset()
                   ) -> dict[int, Function]:
    """Kept starts and their [start, end) spans.

    Candidates (Ghidra's starts) are kept only with evidence. Besides the
    given `evidence`, a kept body's direct call targets and code-address
    immediates are kept, and so is each target of a jump inside a span that
    falls outside that span: the lifter takes one [start, end) per function,
    so such a jump can only be a tail call. A candidate that no span covers
    is kept as an orphan (dead code, or entered by a pointer not seen),
    unless it is no-op fill. A span ends at the end of the highest
    instruction its descent reaches, or at the next kept start. A start
    inside an instruction of a lower kept span is dropped, unless listed,
    and so is a pointer-evidenced start that straight-line code of another
    body runs into; the search then restarts without them. An orphan whose
    straight-line code runs into data is not kept. `data` holds the VAs of the data
    bytes inside the code (jump tables, index maps).
    """
    drop = set(drop)
    while True:
        cands = {c for c in candidates if code.starts_code(c)} - drop
        why = {va: e for va, e in evidence.items()
               if va not in drop and code.starts_code(va)}
        bodies: dict[int, Body] = {}
        _converge(code, cands, why, bodies, noreturn, drop, data)
        starts = sorted(why)
        ends = {k: min(bodies[k].end, starts[i + 1])
                if i + 1 < len(starts) else bodies[k].end
                for i, k in enumerate(starts)}
        interior = bytearray(code.hi - code.lo)
        split: set[int] = set()
        for k in starts:
            if interior[k - code.lo] and why[k] is not Evidence.LISTED:
                split.add(k)
                continue
            for va, n in bodies[k].insns.items():
                if va < ends[k]:
                    interior[va + 1 - code.lo:va + n - code.lo] = (
                        b"\1" * (n - 1))
                # Straight-line code runs into a pointer-evidenced start:
                # the dword matched by chance (strings such as "ket\0" are
                # code addresses; a function can run from `mov ecx,[eax+0x340]`
                # into the next function). Junk orphans that would run into a
                # real start are dropped first (_runs_into_data).
                t = va + n
                if (why.get(t) in _POINTER
                        and code.kind(va) in (Kind.SEQ, Kind.JCC)):
                    split.add(t)
        if not split:
            return {k: Function(ends[k], why[k]) for k in starts}
        drop |= split


def _converge(code: Code, cands: set[int], why: dict[int, Evidence],
              bodies: dict[int, Body], noreturn: Set[int],
              drop: Set[int], data: Set[int]) -> None:
    fresh: set[int] = set(why)
    while fresh:
        # A new start changes only the bodies that reached it.
        order = sorted(fresh)
        for k, b in list(bodies.items()):
            i = bisect.bisect_right(order, k)
            while i < len(order) and order[i] < b.end:
                if order[i] in b.insns:
                    del bodies[k]
                    break
                i += 1
        for k in why:
            if k not in bodies:
                bodies[k] = walk(code, k, why, noreturn)
        starts = sorted(why)
        arms = {a for b in bodies.values() for a in b.arms}
        imm_targets = cands - arms
        ends = [min(bodies[k].end, starts[i + 1]) if i + 1 < len(starts)
                else bodies[k].end for i, k in enumerate(starts)]
        new: dict[int, Evidence] = {}
        for i, k in enumerate(starts):
            b = bodies[k]
            end = ends[i]
            for t in b.calls:
                new.setdefault(t, Evidence.CALL)
            # A code address right after int3 fill is a function start
            # even when Ghidra missed it: `push <function>`
            # registers a callback that boot calls.
            for t in b.imms:
                if t in imm_targets or (code.lo < t < code.hi and t not in arms
                                        and code.image[t - 1 - code.base]
                                        == 0xCC):
                    new.setdefault(t, Evidence.IMM)
            # A jump from past the span is code of another function; the
            # jump that led there is the one to keep. This holds for
            # orphans too: an SEH4 join (entered from the main body and
            # from its handler) and an x87 entry two bytes into another
            # function's are shared code that the lifter only reaches as a
            # tail call to a start.
            for site, t in b.jumps:
                if k <= site < end and not k <= t < end:
                    new.setdefault(t, Evidence.JUMP)
        # An orphan that kept code calls or jumps to is no longer one.
        promoted = {t for t in new if why.get(t) is Evidence.ORPHAN}
        why.update((t, new[t]) for t in promoted)
        fresh = {t for t in new if t not in why and t not in drop
                 and code.starts_code(t)}
        if fresh:
            why.update((t, new[t]) for t in fresh)
        elif promoted:
            fresh = promoted
        else:
            fresh = _orphans(code, cands, why, bodies, noreturn, data)


def _orphans(code: Code, cands: set[int], why: dict[int, Evidence],
             bodies: dict[int, Body], noreturn: Set[int],
             data: Set[int]) -> set[int]:
    """Keep, lowest first, each candidate no span covers.

    When the code from the end of the span before it (past int3 fill)
    reaches the candidate, that address is the start instead: Ghidra lists
    switch arms of some functions without the function itself."""
    starts = sorted(why)
    ordered = sorted(cands | why.keys())
    added: set[int] = set()
    reach = 0
    j = 0
    for i, c in enumerate(ordered):
        while j < len(starts) and starts[j] <= c:
            k = starts[j]
            nxt = starts[j + 1] if j + 1 < len(starts) else bodies[k].end
            reach = max(reach, min(bodies[k].end, nxt))
            j += 1
        if c in why or c < reach:
            continue
        nxt = ordered[i + 1] if i + 1 < len(ordered) else code.hi
        if code.is_padding(c, nxt):
            continue
        gap = reach
        while gap < c and code.image[gap - code.base] == 0xCC:
            gap += 1
        start = c
        if code.lo <= gap < c and gap not in data and code.starts_code(gap):
            body = walk(code, gap, why, noreturn)
            if c in body.insns:
                start = gap
        if _runs_into_data(code, start, data):
            continue
        why[start] = Evidence.ORPHAN
        bodies[start] = walk(code, start, why, noreturn)
        nxt = starts[j] if j < len(starts) else bodies[start].end
        reach = max(reach, min(bodies[start].end, nxt))
        added.add(start)
    return added


def _runs_into_data(code: Code, start: int, data: Set[int]) -> bool:
    """The straight-line code from start reaches a byte Ghidra typed as data:
    start is a decode of a jump table, not code (two such decodes in HP run
    from `8b ff` into tables of code pointers). Only fall-through counts:
    a real switch reaches its table's arms, and over-read arms may land in
    data."""
    va = start
    while (insn := code.at(va)) is not None and insn.kind in (
            Kind.SEQ, Kind.JCC, Kind.CALL):
        if any(b in data for b in range(va, va + insn.size)):
            return True
        va += insn.size
    return insn is not None and va in data


def data_pointers(sections: Sequence[tuple[bytes, int, bool]],
                  targets: Set[int]) -> set[int]:
    """Members of targets held by an aligned dword of a (bytes, VA,
    writable) section. In a writable section the dword counts only next to
    another member (a table of function pointers): alone, or among other
    values, it is as likely a bit mask or a counter: a mask or a run of
    16-bit counts in .data can read as an address inside a function."""
    found: set[int] = set()
    for raw, va, writable in sections:
        skew = -va % 4
        words = struct.unpack_from(f"<{(len(raw) - skew) // 4}I", raw, skew)
        for i, v in enumerate(words):
            if v in targets and (not writable or any(
                    0 <= j < len(words) and words[j] in targets
                    for j in (i - 1, i + 1))):
                found.add(v)
    return found


def code_end(pe: PE) -> int:
    """VA just past the last byte of .text that is not 0xCC fill."""
    text = pe.section(".text")
    raw = pe.data[text["roff"]:text["roff"] + min(text["rsize"], text["vsize"])]
    return pe.image_base + text["va"] + len(raw.rstrip(b"\xcc"))


def _functions(pe: PE, ghidra: list[dict[str, Any]],
               ghidra_data: list[dict[str, Any]], drop: frozenset[int],
               add: frozenset[int], init_tables: Sequence[tuple[int, int]]
               ) -> tuple[list[dict[str, int]], dict[str, int]]:
    """Function entries with their [start, end) span, from the code itself.

    Ghidra's starts are candidates only. Loop-alignment padding, switch arms
    and mid-instruction decodes are among them, and each one cut its owner's
    body short: every jump past the cut became a call to a no-op stub
    (notes/stub_triage.md). find_functions keeps a start on evidence and
    sets its span from a recursive descent.
    """
    text = pe.section(".text")
    lo = pe.image_base + text["va"]
    image = bytearray()
    for s in pe.sections:
        if s["name"] == ".rsrc":
            continue
        va = s["va"]
        raw = pe.data[s["roff"]:s["roff"] + min(s["rsize"], s["vsize"])]
        image[len(image):] = bytes(va - len(image)) if va > len(image) else b""
        image[va:va + len(raw)] = raw
    code = Code(bytes(image), pe.image_base, lo, code_end(pe))
    rows = [f for f in ghidra if code.lo <= f["addr"] < code.hi]
    candidates = {f["addr"] for f in rows} | add
    # Ghidra marks some returning routines no-return (__fload_withFB at
    # a CRT stub ends in ret), which would cut the caller after the call.
    noreturn = {f["addr"] for f in rows if f["no_return"]
                and not any(code.kind(va) is Kind.RET for va in
                            walk(code, f["addr"], frozenset(),
                                 frozenset()).insns)}
    evidence: dict[int, Evidence] = {}
    data = [(pe.data[s["roff"]:s["roff"] + min(s["rsize"], s["vsize"])],
             pe.image_base + s["va"], bool(s["flags"] & IMAGE_SCN_MEM_WRITE))
            for s in pe.sections
            if not s["flags"] & IMAGE_SCN_MEM_EXECUTE and s["name"] != ".rsrc"]
    # Ghidra names switch arms caseD_* and __break. Their only real
    # references are jump tables, so a data dword that matches one is a
    # coincidence (a string such as "dat\0" is 0x00746164).
    arms = {f["addr"] for f in rows
            if f["name"].startswith("caseD_") or f["name"] == "__break"}
    for va in data_pointers(data, candidates - arms):
        evidence[va] = Evidence.DATA
    for c in candidates:
        insn = code.at(c)
        if (insn is not None and insn.kind is Kind.JMP and insn.size == 5
                and insn.target is not None):
            evidence.setdefault(insn.target, Evidence.THUNK)
    for va in (pe.entry, *pe.exports(), *add,
               *_table_entries(pe, init_tables)):
        evidence[va] = Evidence.LISTED
    data_bytes = {d["addr"] + i for d in ghidra_data
                  for i in range(d["length"])}
    found = find_functions(code, candidates, evidence, noreturn, drop,
                           data_bytes)
    stats = {"candidates": len(candidates), "kept": len(found)}
    for f in found.values():
        stats[f.evidence.value] = stats.get(f.evidence.value, 0) + 1
    return [{"addr": k, "size": f.end - k}
            for k, f in sorted(found.items())], stats


def read_ranges(path: Path) -> list[tuple[int, int]]:
    """Two hex VAs per line, [first, last); '#' starts a comment."""
    out = []
    for line in path.read_text().splitlines():
        text = line.split("#", 1)[0].split()
        if text:
            out.append((int(text[0], 16), int(text[1], 16)))
    return out


def read_starts(path: Path) -> frozenset[int]:
    """Hex VAs, one per line; '#' starts a comment."""
    vas = set()
    for line in path.read_text().splitlines():
        text = line.split("#", 1)[0].strip()
        if text:
            vas.add(int(text, 16))
    return frozenset(vas)


def _table_entries(pe: PE, tables: Sequence[tuple[int, int]]) -> list[int]:
    """Non-null dwords of the [first, last) VA ranges (initialiser tables)."""
    out = []
    for lo, hi in tables:
        off = pe.rva_to_off(lo - pe.image_base)
        assert off is not None, f"table 0x{lo:x} outside the image"
        out += [v for v in struct.unpack_from(f"<{(hi - lo) // 4}I", pe.data,
                                              off) if v]
    return out


def analyze(data: bytes, ghidra_funcs: list[dict[str, Any]],
            ghidra_data: list[dict[str, Any]],
            drop: frozenset[int] = frozenset(),
            add: frozenset[int] = frozenset(),
            init_tables: Sequence[tuple[int, int]] = ()) -> dict[str, Any]:
    pe = PE(data)
    functions, stats = _functions(pe, ghidra_funcs, ghidra_data, drop, add,
                                  init_tables)
    text = pe.section(".text")
    text_lo = pe.image_base + text["va"]
    text_hi = text_lo + text["vsize"]
    imports = pe.imports()
    return {
        "binary": {"sha256": hashlib.sha256(data).hexdigest(),
                   "entry": pe.entry},
        "dylibs": sorted({i["dll"] for i in imports}),
        "segments": _segments(pe),
        "functions": functions,
        "data_in_code": [
            {"addr": d["addr"], "length": d["length"], "kind": d["type"]}
            for d in ghidra_data if text_lo <= d["addr"] < text_hi
        ],
        "imports": [
            {"name": i["name"] or f'{i["dll"]}#{i["ordinal"]}',
             "dylib": i["dll"]}
            for i in imports
        ],
        "indirect_slots": [
            {"section": "__idata", "addr": i["slot"], "kind": "nonlazy_ptr",
             "symbol": i["name"] or f'{i["dll"]}#{i["ordinal"]}'}
            for i in imports
        ],
        "mod_init_funcs": [],
        # PE-only: the run-time loader binds these IAT slots. The static CRT
        # runs its own initialiser table, so mod_init_funcs stays empty.
        "pe": {
            "image_base": pe.image_base,
            "entry_rva": pe.entry_rva,
            "timestamp": pe.timestamp,
            "sections": pe.sections,
            "iat": imports,
            "function_evidence": stats,
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--ghidra-functions", type=Path, required=True)
    parser.add_argument("--ghidra-data", type=Path, required=True)
    parser.add_argument("--drop-starts", type=Path, default=None,
                        help="hex VAs that are not function entries")
    parser.add_argument("--add-starts", type=Path, default=None,
                        help="hex VAs that are function entries")
    parser.add_argument("--init-tables", type=Path, default=None,
                        help="hex VA ranges of CRT initialiser tables")
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args()
    result = analyze(
        args.binary.read_bytes(),
        json.loads(args.ghidra_functions.read_text()),
        json.loads(args.ghidra_data.read_text()),
        read_starts(args.drop_starts) if args.drop_starts else frozenset(),
        read_starts(args.add_starts) if args.add_starts else frozenset(),
        read_ranges(args.init_tables) if args.init_tables else (),
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=1) + "\n")
    text = next(s for s in result["segments"] if s["name"] == "__TEXT")
    print(f'entry 0x{result["binary"]["entry"]:x}  '
          f'functions {len(result["functions"])}  '
          f'imports {len(result["imports"])}  '
          f'data_in_code {len(result["data_in_code"])}  '
          f'text 0x{text["vmaddr"]:x}+0x{text["vmsize"]:x}')
    print("function evidence", result["pe"]["function_evidence"])


if __name__ == "__main__":
    main()
