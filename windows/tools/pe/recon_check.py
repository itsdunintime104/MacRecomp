"""Report recon misses of a PE32 game: code the lift would not reach.

Usage: python -I recon_check.py <exe> --recon <dir>

Reads <dir>/analysis.json and <dir>/entry_zf.json (optional) and prints one
line per finding, `<class> 0x<VA> <evidence>`, then a count. Exit status 1
when there is a finding. Read-only; it fixes nothing.

  data-pointer  an aligned dword of .rdata/.data holds a code address that is
                no start: a slot of the incremental-link table, or code that
                no function covers, at a boundary (after 0xCC, 0x00, ret or
                jmp) and with code up to a ret or jmp. Strings and MSVC
                exception data (SEH scopes, C++ unwind, catch and throw info)
                are left out.
  entry-zf      a function called right after the CRT x87 classifier
                (analyze_pe.entry_zf) is missing from entry_zf.json, or a
                listed one is not derived.
  false-start   a start that the function before it reaches by falling
                through from cmp or test: the start cuts the compare from the
                jcc that reads its flags.
"""

from __future__ import annotations

import argparse
import bisect
import importlib.util
import json
import struct
import sys
from collections.abc import Iterator, Mapping, Sequence, Set
from enum import StrEnum
from pathlib import Path
from typing import Any, NamedTuple

import capstone

_SPEC = importlib.util.spec_from_file_location(
    "analyze_pe", Path(__file__).resolve().with_name("analyze_pe.py"))
assert _SPEC is not None and _SPEC.loader is not None
A = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(A)

# MSVC C++ FuncInfo magic numbers (VC6, VC7, VC8 and later).
_EH_MAGIC = frozenset({0x19930520, 0x19930521, 0x19930522})
_TOP_LEVEL = frozenset({0xFFFFFFFE, 0xFFFFFFFF})   # SEH4 and SEH3 top scope
MAX_DECODE = 0x1000


class Check(StrEnum):
    DATA_POINTER = "data-pointer"
    ENTRY_ZF = "entry-zf"
    FALSE_START = "false-start"


class Finding(NamedTuple):
    check: Check
    va: int
    evidence: str


class Image(NamedTuple):
    pe: Any                  # analyze_pe.PE
    code: Any                # analyze_pe.Code
    data: list[tuple[int, bytes]]   # (VA, bytes) of .rdata, .data and the like


def load(exe: bytes) -> Image:
    pe = A.PE(exe)
    data = [(pe.image_base + s["va"],
             pe.data[s["roff"]:s["roff"] + min(s["rsize"], s["vsize"])])
            for s in pe.sections
            if not s["flags"] & A.IMAGE_SCN_MEM_EXECUTE and s["name"] != ".rsrc"]
    return Image(pe, A.image_code(pe), data)


def dwords(img: Image) -> Iterator[tuple[int, int]]:
    """(VA, value) of every aligned dword of the data sections."""
    for va, raw in img.data:
        skew = -va % 4
        for i, v in enumerate(struct.unpack_from(
                f"<{(len(raw) - skew) // 4}I", raw, skew)):
            yield va + skew + 4 * i, v


def eh_targets(img: Image) -> set[int]:
    """Code addresses that MSVC exception data holds: C++ unwind actions and
    catch handlers (FuncInfo, found by its magic), and SEH scope-table
    filters and handlers (runs of {level, filter, handler} records that
    start at a top-level record)."""
    code = img.code
    dw = img.code.dword

    def is_code(v: int) -> bool:
        return code.lo <= v < code.hi

    out: set[int] = set()
    words = list(dwords(img))
    for va, v in words:
        if v in _EH_MAGIC:
            max_state, unwind, n_try, try_map = (dw(va + 4), dw(va + 8),
                                                 dw(va + 12), dw(va + 16))
            if None in (max_state, unwind, n_try, try_map) or \
                    max_state > 0x10000 or n_try > 0x1000:
                continue
            for i in range(max_state if unwind else 0):
                out.add(dw(unwind + 8 * i + 4) or 0)
            for i in range(n_try if try_map else 0):
                n_catch, handlers = dw(try_map + 20 * i + 12), dw(
                    try_map + 20 * i + 16)
                if n_catch is None or handlers is None or n_catch > 0x100:
                    continue
                for j in range(n_catch):
                    out.add(dw(handlers + 16 * j + 12) or 0)
    for va, v in words:
        if v not in _TOP_LEVEL:
            continue
        n = 0
        while True:
            level, flt, handler = (dw(va + 12 * n), dw(va + 12 * n + 4),
                                   dw(va + 12 * n + 8))
            if level is None or flt is None or handler is None or not (
                    (level in _TOP_LEVEL if n == 0 else
                     level in _TOP_LEVEL or level < n)
                    and (flt == 0 or is_code(flt)) and is_code(handler)):
                break
            out.update((flt, handler))
            n += 1
    # C++ throw data: a CatchableType {properties, pType, mdisp, pdisp,
    # vdisp, size, copyFunction} names an RTTI TypeDescriptor (".?A..." at
    # +8); a ThrowInfo {attributes, pmfnUnwind, pForwardCompat,
    # pCatchableTypeArray} points at an array {n, CatchableType*...}.
    def is_catchable(ct: int) -> bool:
        td, pdisp = dw(ct + 4), dw(ct + 12)
        return (td is not None and td >= code.base and pdisp is not None
                and (pdisp == 0xFFFFFFFF or pdisp < 0x1000)
                and code.image[td + 8 - code.base:td + 11 - code.base]
                == b".?A")

    for va, v in words:
        if is_catchable(va - 4):
            out.add(dw(va + 20) or 0)
        n, first = dw(v), dw(v + 4)
        if n is not None and 0 < n < 0x40 and first is not None \
                and is_catchable(first):
            out.add(dw(va - 8) or 0)
    return {t for t in out if is_code(t)}


def _is_text(code: Any, h: int) -> bool:
    """The dword at h is part of a string: ASCII after a dword of printable
    bytes, or UTF-16 next to UTF-16."""
    raw = code.image[h - 4 - code.base:h + 8 - code.base]
    if len(raw) < 12 or any(b and not 0x20 <= b < 0x7F for b in raw[4:8]):
        return False

    def utf16(w: bytes) -> bool:
        return w[1] == w[3] == 0 and all(0x20 <= b < 0x7F for b in w[::2])

    return (all(0x20 <= b < 0x7F for b in raw[:4])
            or utf16(raw[4:8]) and (utf16(raw[:4]) or utf16(raw[8:])))


def _boundary(code: Any, t: int) -> bool:
    """t follows 0xCC or 0x00 fill, a ret, a ret n, or a jmp."""
    b = code.image
    o = t - code.base
    return (b[o - 1] in (0xCC, 0x00, 0xC3) or b[o - 3] == 0xC2
            or b[o - 5] == 0xE9 or b[o - 2] == 0xEB)


def _decodes(code: Any, t: int, data_bytes: Set[int], own: range) -> bool:
    """The straight-line code from t reaches a ret or jmp before a stop
    (int3, zero bytes, undecodable) or a byte Ghidra typed as data, other
    than the data item `own` that begins at t."""
    va = t
    while va < t + MAX_DECODE:
        insn = code.at(va)
        if insn is None or insn.kind is A.Kind.STOP or any(
                b in data_bytes and b not in own
                for b in range(va, va + insn.size)):
            return False
        if insn.kind in (A.Kind.RET, A.Kind.JMP, A.Kind.IJMP, A.Kind.TABLE):
            return True
        va += insn.size
    return True


class Spans:
    """The kept functions of analysis.json."""

    def __init__(self, functions: Sequence[Mapping[str, int]]) -> None:
        rows = sorted((f["addr"], f["addr"] + f["size"]) for f in functions)
        self.starts = [a for a, _ in rows]
        self.ends = [e for _, e in rows]
        self.set = frozenset(self.starts)

    def covers(self, va: int) -> bool:
        i = bisect.bisect_right(self.starts, va) - 1
        return i >= 0 and va < self.ends[i]


def data_pointers(img: Image, spans: Spans, data_bytes: Set[int],
                  items: Mapping[int, int]) -> list[Finding]:
    """A Ghidra data item that begins at the target does not count: Ghidra
    types the target of a data pointer it could not follow as data (a
    handler in a table becomes an `undefined *`). An incremental-link slot
    counts even inside a span: Ghidra starts junk functions inside the table,
    one byte or more into a slot, and their spans cover the next slots."""
    code = img.code
    eh = eh_targets(img)
    slots = ilt_slots(code)
    holders: dict[int, list[int]] = {}
    for va, v in dwords(img):
        if code.lo <= v < code.hi and not _is_text(code, va):
            holders.setdefault(v, []).append(va)
    return [Finding(Check.DATA_POINTER, t,
                    f"held by {len(hs)} dword(s), first 0x{hs[0]:X}")
            for t, hs in sorted(holders.items())
            if t not in spans.set and t not in eh and (
                t in slots or not spans.covers(t)
                and (t not in data_bytes or t in items) and _boundary(code, t)
                and _decodes(code, t, data_bytes,
                             range(t, t + items.get(t, 0))))]


def entry_flags(img: Image, spans: Spans, calls: Mapping[int, Sequence[int]],
                listed: Mapping[str, Any]) -> list[Finding]:
    derived = {f"0x{va:X}": json.loads(A.entry_zf_json({va: c}))[f"0x{va:X}"]
               for va, c in A.entry_zf(img.code, spans.set, calls).items()}
    out = [Finding(Check.ENTRY_ZF, int(k, 16),
                   f"called right after the x87 classifier: {v}")
           for k, v in derived.items() if listed.get(k) != v]
    out += [Finding(Check.ENTRY_ZF, int(k, 16),
                    "in entry_zf.json, not derived")
            for k in listed if k not in derived]
    return out


def ilt_slots(code: Any) -> range:
    """The incremental-link table: 5-byte `jmp rel32` slots from the start
    of .text, past 0xCC fill."""
    va = code.lo
    while code.image[va - code.base] == 0xCC:
        va += 1
    end = va
    while (insn := code.at(end)) is not None and insn.kind is A.Kind.JMP \
            and insn.size == 5:
        end += 5
    return range(va, end, 5)


def false_starts(img: Image, spans: Spans) -> list[Finding]:
    """Starts that the function before them reaches by falling through from
    cmp or test."""
    code = img.code
    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    out = []
    for k, end in zip(spans.starts, spans.ends):
        if end not in spans.set:
            continue
        for va, n in A.walk(code, k, spans.set, frozenset()).insns.items():
            if va + n != end:
                continue
            off = va - code.base
            mnem = next(cs.disasm_lite(code.image[off:off + n], va, 1))[2]
            if mnem in ("cmp", "test"):
                out.append(Finding(Check.FALSE_START, end,
                                   f"{mnem} at 0x{va:X} in 0x{k:X} "
                                   "falls through into it"))
    return out


def check(exe: bytes, analysis: Mapping[str, Any],
          listed_zf: Mapping[str, Any]) -> list[Finding]:
    img = load(exe)
    spans = Spans(analysis["functions"])
    items = {d["addr"]: d["length"] for d in analysis["data_in_code"]}
    data_bytes = {va + i for va, n in items.items() for i in range(n)}
    calls = A.direct_calls(img.code)
    return (data_pointers(img, spans, data_bytes, items)
            + entry_flags(img, spans, calls, listed_zf)
            + false_starts(img, spans))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--recon", type=Path, required=True)
    args = parser.parse_args()
    zf = args.recon / "entry_zf.json"
    findings = check(args.binary.read_bytes(),
                     json.loads((args.recon / "analysis.json").read_text()),
                     json.loads(zf.read_text()) if zf.exists() else {})
    for f in findings:
        print(f"{f.check} 0x{f.va:X} {f.evidence}")
    print(f"findings {len(findings)}")
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()
