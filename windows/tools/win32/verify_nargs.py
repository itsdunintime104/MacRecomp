"""Check the inventory's stdcall pop counts against the call sites in .text.

Usage: python -I tools/win32/verify_nargs.py orig.nosync/<game exe> \
           recon/analysis.json recon/ghidra_functions.json notes/win32_imports.md \
           -o notes/nargs_check.md

For every IAT slot (analysis.json "pe"."iat"), each `call dword ptr [slot]`
and `jmp dword ptr [slot]` in .text is found and disassembled with capstone,
function by function (Ghidra's function ranges, so the sweep never runs into
data). Per call site:

- the next instruction: `add esp, N` means the caller pops (cdecl);
- the pushes before the call in the same basic block: walking back from the
  call until a block boundary (function start, branch target, branch, ret,
  an earlier call, or another write to esp).

- a later `add esp, N` in the same block (MSVC defers a caller's cleanup).

A stdcall row with N > 0 argument slots DISAGREES when a call is followed
directly by `add esp, 4*N` (the caller pops: cdecl). Otherwise it AGREES when
at least one site has exactly N pushes. A cdecl row AGREES when a site has an
`add esp` after the call (direct or deferred), and DISAGREES when it has
pushes but no `add esp` anywhere in the block. Anything else is NO-EVIDENCE:
push counts alone never make a DISAGREES, because MSVC pushes arguments
across block boundaries and before earlier calls. Read only; the inventory is
not changed.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import cast

import capstone

BRANCH = {"jmp", "ret", "retn", "iretd", "int3", "hlt", "ud2"}
ROW_RE = re.compile(r"^\|(?P<cells>.*)\|\s*$")
ADD_ESP_RE = re.compile(r"^esp, (0x[0-9a-f]+|\d+)$")


@dataclass
class Row:
    dll: str
    symbol: str
    cdecl: bool
    nbytes: int  # bytes popped by the callee (stdcall) or by the caller (cdecl)


@dataclass
class Site:
    va: int
    kind: str  # "call" or "jmp"
    pushes: int | None  # None: block not clean (esp written otherwise)
    add_esp: int | None  # N of an `add esp, N` directly after the call
    later_add_esp: int | None  # N of the first later `add esp, N` in the block


@dataclass
class SlotEvidence:
    sites: list[Site] = field(default_factory=lambda: list[Site]())
    reg_loads: int = 0  # `mov reg, dword ptr [slot]` (called through a register)


def parse_inventory(path: Path) -> dict[int, Row]:
    """Inventory rows keyed by IAT slot VA."""
    rows: dict[int, Row] = {}
    in_table = False
    for line in path.read_text().splitlines():
        if line.startswith("## Imports"):
            in_table = True
            continue
        m = ROW_RE.match(line) if in_table else None
        if m is None:
            continue
        cells = [c.strip() for c in m.group("cells").split("|")]
        if len(cells) != 7 or cells[0] == "DLL" or cells[0].startswith("---"):
            continue
        args = cells[3]
        cdecl = args.startswith("cdecl")
        nbytes = int(re.sub(r"\D", "", args)) if cdecl else int(args)
        rows[int(cells[6], 16)] = Row(cells[0], cells[1], cdecl, nbytes)
    return rows


def parse_imm(s: str) -> int:
    return int(s, 16) if s.startswith("0x") else int(s)


def text_section(exe: bytes, image_base: int, sections: list[dict[str, int | str]]) -> tuple[int, bytes]:
    for s in sections:
        if s["name"] == ".text":
            va, roff, rsize = int(s["va"]), int(s["roff"]), int(s["rsize"])
            return image_base + va, exe[roff:roff + rsize]
    raise SystemExit("no .text section")


def writes_esp(mn: str, ops: str) -> bool:
    if mn in ("pop", "popal", "popfd", "leave", "enter"):
        return True
    return ops == "esp" or ops.startswith("esp,")


def analyse(text_va: int, text: bytes, funcs: list[dict[str, object]], slots: set[int]
            ) -> tuple[dict[int, SlotEvidence], int]:
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    ev: dict[int, SlotEvidence] = {s: SlotEvidence() for s in slots}
    mem_re = re.compile(r"dword ptr \[(0x[0-9a-f]+)\]$")
    seen_sites: set[int] = set()
    text_end = text_va + len(text)
    for f in funcs:
        ranges = cast(list[list[int]], f.get("ranges") or [])
        for lo, hi in ranges:
            if lo < text_va or hi >= text_end:
                continue
            insns = list(md.disasm_lite(text[lo - text_va:hi + 1 - text_va], lo))
            targets = {lo}
            for _a, _sz, mn, ops in insns:
                if (mn.startswith("j") or mn == "loop") and ops.startswith("0x"):
                    targets.add(int(ops, 16))
            for i, (a, _sz, mn, ops) in enumerate(insns):
                m = mem_re.search(ops)
                if m is None:
                    continue
                slot = int(m.group(1), 16)
                if slot not in ev:
                    continue
                if mn == "mov" and not ops.startswith("dword ptr"):
                    ev[slot].reg_loads += 1
                    continue
                if mn not in ("call", "jmp") or a in seen_sites:
                    continue
                seen_sites.add(a)
                add_esp = later = None
                if mn == "call":
                    for k in range(i + 1, min(i + 16, len(insns))):
                        kmn, kops = insns[k][2], insns[k][3]
                        if insns[k][0] in targets or kmn in BRANCH or kmn.startswith("j") \
                                or kmn == "call":
                            break
                        am = ADD_ESP_RE.match(kops) if kmn == "add" else None
                        if am:
                            if k == i + 1:
                                add_esp = parse_imm(am.group(1))
                            else:
                                later = parse_imm(am.group(1))
                            break
                        if writes_esp(kmn, kops) or kmn == "push":
                            break
                pushes: int | None = None
                if mn == "call":
                    pushes = 0
                    j = i
                    while insns[j][0] not in targets and j > 0:
                        j -= 1
                        pmn, pops = insns[j][2], insns[j][3]
                        if pmn in BRANCH or pmn.startswith("j") or pmn == "call":
                            break
                        if pmn == "push":
                            pushes += 1
                        elif writes_esp(pmn, pops):
                            pushes = None
                            break
                ev[slot].sites.append(Site(a, mn, pushes, add_esp, later))
    # Sites outside every Ghidra function: found by bytes, no push evidence.
    stray = 0
    for op, kind in ((b"\xff\x15", "call"), (b"\xff\x25", "jmp")):
        for slot in slots:
            pat = op + struct.pack("<I", slot)
            pos = text.find(pat)
            while pos >= 0:
                va = text_va + pos
                if va not in seen_sites:
                    stray += 1
                    nxt = list(md.disasm_lite(text[pos + 6:pos + 16], va + 6, 1))
                    add_esp = None
                    if kind == "call" and nxt and nxt[0][2] == "add":
                        am = ADD_ESP_RE.match(nxt[0][3])
                        if am:
                            add_esp = parse_imm(am.group(1))
                    ev[slot].sites.append(Site(va, kind + "?", None, add_esp, None))
                    seen_sites.add(va)
                pos = text.find(pat, pos + 1)
    return ev, stray


def verdict(row: Row, e: SlotEvidence) -> tuple[str, str]:
    calls = [s for s in e.sites if s.kind.startswith("call")]
    jmps = len(e.sites) - len(calls)
    adds = Counter(s.add_esp for s in calls if s.add_esp is not None)
    later = Counter(s.later_add_esp for s in calls if s.later_add_esp is not None)
    pushes = Counter(s.pushes for s in calls if s.pushes is not None)
    n = row.nbytes // 4
    parts = [f"{len(calls)} call", f"{jmps} jmp", f"{e.reg_loads} reg-load"]
    if pushes:
        parts.append("pushes " + ", ".join(f"{k}x{v}" for k, v in sorted(pushes.items())))
    if adds:
        parts.append("add esp " + ", ".join(f"{k}x{v}" for k, v in sorted(adds.items())))
    if later:
        parts.append("later add esp " + ", ".join(f"{k}x{v}" for k, v in sorted(later.items())))
    evidence = "; ".join(parts)
    if row.cdecl:
        if adds or later:
            return "AGREES", evidence
        if pushes:
            return "DISAGREES", evidence
        return "NO-EVIDENCE", evidence
    if n > 0 and adds.get(row.nbytes, 0) > 0:
        return "DISAGREES", evidence
    if pushes.get(n, 0) > 0:
        return "AGREES", evidence
    return "NO-EVIDENCE", evidence


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("exe", type=Path)
    ap.add_argument("analysis", type=Path)
    ap.add_argument("functions", type=Path)
    ap.add_argument("inventory", type=Path)
    ap.add_argument("-o", "--output", type=Path, required=True)
    args = ap.parse_args()

    pe = json.loads(args.analysis.read_text())["pe"]
    iat: list[dict[str, object]] = pe["iat"]
    rows = parse_inventory(args.inventory)
    text_va, text = text_section(args.exe.read_bytes(), int(pe["image_base"]), pe["sections"])
    funcs: list[dict[str, object]] = json.loads(args.functions.read_text())
    slots = {int(str(s["slot"])) for s in iat}
    ev, stray = analyse(text_va, text, funcs, slots)

    results: list[tuple[str, Row, int, str]] = []
    for s in iat:
        slot = int(str(s["slot"]))
        row = rows.get(slot)
        if row is None or row.dll.lower() != str(s["dll"]).lower():
            raise SystemExit(f"IAT slot {s} has no matching inventory row")
        v, evidence = verdict(row, ev[slot])
        results.append((v, row, slot, evidence))

    counts = Counter(r[0] for r in results)
    out = [
        "# Import pop-count check",
        "",
        "Generated by `tools/win32/verify_nargs.py` (capstone, Ghidra function ranges)",
        "from the game exe. Read only: the inventory is not changed.",
        "",
        "Evidence per import: direct `call [slot]` and `jmp [slot]` sites; `reg-load` counts",
        "`mov reg, [slot]` (the import is then called through a register, not checked here);",
        "`pushes KxV` means V call sites have K pushes in the same basic block before the call;",
        "`add esp NxV` means V call sites are followed directly by `add esp, N`;",
        "`later add esp` is a deferred cleanup later in the same block.",
        "Push counts are an upper or lower bound only when the block also saves registers",
        "or builds arguments with `mov [esp+..]`; read DISAGREES rows at the call sites.",
        "",
        "| Verdict | Count |",
        "|---|---|",
    ]
    for k in ("AGREES", "DISAGREES", "NO-EVIDENCE"):
        out.append(f"| {k} | {counts.get(k, 0)} |")
    out += [f"| total | {len(results)} |", "",
            f"Direct sites outside every Ghidra function (import `jmp [slot]` thunks; no push",
            f"evidence): {stray}.", ""]
    dis = [r for r in results if r[0] == "DISAGREES"]
    out += ["## Disagreements", ""]
    if dis:
        out += ["| DLL | Symbol | Inventory | Evidence |", "|---|---|---|---|"]
        for _v, row, _slot, evidence in dis:
            inv = f"cdecl {row.nbytes}" if row.cdecl else str(row.nbytes)
            out.append(f"| {row.dll} | {row.symbol} | {inv} | {evidence} |")
    else:
        out.append("None.")
    out += ["", "## All imports", "",
            "| DLL | Symbol | IAT VA | Inventory bytes | Evidence | Verdict |",
            "|---|---|---|---|---|---|"]
    for v, row, slot, evidence in results:
        inv = f"cdecl {row.nbytes}" if row.cdecl else str(row.nbytes)
        out.append(f"| {row.dll} | {row.symbol} | 0x{slot:x} | {inv} | {evidence} | {v} |")
    args.output.write_text("\n".join(out) + "\n")
    print(" ".join(f"{k}={counts.get(k, 0)}" for k in ("AGREES", "DISAGREES", "NO-EVIDENCE")),
          f"total={len(results)} stray_sites={stray} -> {args.output}")


if __name__ == "__main__":
    main()
