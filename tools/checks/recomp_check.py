"""recomp-check: fail when a game's translation has a real gap.

Checks (each prints one `PASS|FAIL  <name>: <detail>` line):
  failed functions, unhandled instructions in real code, unset-flag reads,
  unexplained indirect jumps, decode failures in code, and (--fuzz) leaf-fuzzer mismatches.

Usage: python -m tools.checks.recomp_check --profile <game.toml> [--generated DIR] [--switch FILE] [--fuzz]
"""
from __future__ import annotations

import argparse
import bisect
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from tools.game import profile as game_profile

ENGINE_ROOT = Path(__file__).resolve().parents[2]
UNIMPL_RE = re.compile(r'RECOMP_UNIMPL\("([^"]*)", 0x([0-9A-Fa-f]+)u\)')
UNSET_FLAG_TEXT = "if (_flags /* "
FUNC_RE = re.compile(r"^void sub_[0-9A-F]+\(void\)")
LABEL_RE = re.compile(r"^\s*(loc_[0-9A-F]+): ;")
GOTO_RE = re.compile(r"goto (loc_[0-9A-F]+);")
COMMENT_RE = re.compile(r"/\*.*?\*/")
NO_EFFECT_TEXT = "(void)0;"
TRAP_MNEMONICS = frozenset(("ud2", "hlt"))
NAN_GROUP_PREFIX = "nan:"
MAX_LISTED = 10


@dataclass
class CheckResult:
    name: str
    ok: bool
    detail: str
    lines: list[str] = field(default_factory=lambda: [])


def _json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text())


def _unimpl_sites(generated: Path) -> dict[int, str]:
    sites: dict[int, str] = {}
    for src in sorted(generated.glob("*.c")):
        if src.name == "sample.c":
            continue
        for match in UNIMPL_RE.finditer(src.read_text(errors="replace")):
            sites.setdefault(int(match.group(2), 16), match.group(1))
    return sites


def _table_ranges(switch: dict[str, Any]) -> list[tuple[int, int]]:
    return sorted((t["table_addr"], t["table_addr"] + 4 * t["count"]) for t in switch["switch"])


def _in_tables(va: int, ranges: list[tuple[int, int]]) -> bool:
    k = bisect.bisect_right([r[0] for r in ranges], va) - 1
    return k >= 0 and ranges[k][0] <= va < ranges[k][1]


def check_failed_functions(lift_report: dict[str, Any]) -> CheckResult:
    failed = int(lift_report["functions_failed"])
    return CheckResult("failed functions", failed == 0, f"{failed} failed")


def check_unhandled(generated: Path, switch: dict[str, Any]) -> CheckResult:
    sites = _unimpl_sites(generated)
    ranges = _table_ranges(switch)
    in_tables = traps = 0
    gaps: list[tuple[int, str]] = []
    for va, text in sorted(sites.items()):
        if _in_tables(va, ranges):
            in_tables += 1
        elif text.split()[0] in TRAP_MNEMONICS:
            traps += 1
        else:
            gaps.append((va, text))
    detail = f"{len(sites)} sites, {in_tables} in tables, {traps} traps, {len(gaps)} gaps"
    return CheckResult("unhandled instructions", not gaps, detail,
                       [f"  0x{va:08X} {text}" for va, text in gaps])


def _code(line: str) -> str:
    return COMMENT_RE.sub("", line)


def _ends_block(line: str) -> bool:
    code = _code(line).strip()
    return code.startswith("goto loc_") or (code.endswith("return;") and not code.startswith("if "))


def _reached_lines(body: list[str]) -> set[int]:
    """Indexes of the body lines reachable from the function's first line."""
    labels = {m.group(1): i for i, line in enumerate(body) if (m := LABEL_RE.match(line))}
    seen: set[int] = set()
    work = [0]
    while work:
        i = work.pop()
        while i < len(body) and i not in seen:
            seen.add(i)
            work.extend(labels[t] for t in GOTO_RE.findall(_code(body[i])) if t in labels)
            if _ends_block(body[i]):
                break
            i += 1
    return seen


def _read_has_effect(body: list[str], index: int) -> bool:
    """False when the read's branch cannot change control flow (no-op, or jumps to the next line)."""
    code = _code(body[index])
    if NO_EFFECT_TEXT in code:
        return False
    target = GOTO_RE.search(code)
    if target is None:
        return True
    following = next((ln for ln in body[index + 1:] if ln.strip()), "")
    label = LABEL_RE.match(following)
    return label is None or label.group(1) != target.group(1)


def check_unset_flags(generated: Path) -> CheckResult:
    """A read of the never-assigned _flags fails only when it is reachable and changes control flow."""
    hits: list[str] = []
    total = 0
    for src in sorted(generated.glob("*.c")):
        if src.name == "sample.c":
            continue
        lines = src.read_text(errors="replace").splitlines()
        starts = [i for i, line in enumerate(lines) if FUNC_RE.match(line)]
        for k, start in enumerate(starts):
            end = starts[k + 1] if k + 1 < len(starts) else len(lines)
            body = lines[start + 1:end]
            reads = [i for i, line in enumerate(body) if UNSET_FLAG_TEXT in line]
            if not reads:
                continue
            total += len(reads)
            reached = _reached_lines(body)
            hits.extend(f"  {src.name}:{start + 2 + i}" for i in reads
                        if i in reached and _read_has_effect(body, i))
    detail = (f"{len(hits)} reachable reads ({total} in total, "
              f"{total - len(hits)} unreachable or without effect)")
    return CheckResult("unset-flag reads", not hits, detail, hits[:MAX_LISTED])


def check_indirect_jumps(switch: dict[str, Any]) -> CheckResult:
    other = switch["other"]
    return CheckResult("unexplained indirect jumps", not other, f"{len(other)} jumps",
                       [f"  0x{entry['addr']:08X}" for entry in other])


def check_decode_failures(stats: dict[str, Any]) -> CheckResult:
    count = int(stats["decode_failures_outside_tables"])
    return CheckResult("decode failures outside tables", count == 0, f"{count} failures",
                       [f"  0x{addr:08X}" for addr in stats["decode_failures_outside_tables_addrs"]])


def run_static_checks(generated: Path, switch_path: Path, stats_path: Path,
                      lift_report_path: Path) -> list[CheckResult]:
    switch = _json(switch_path)
    return [
        check_failed_functions(_json(lift_report_path)),
        check_unhandled(generated, switch),
        check_unset_flags(generated),
        check_indirect_jumps(switch),
        check_decode_failures(_json(stats_path)),
    ]


def read_fuzz_report(report_path: Path) -> CheckResult:
    report = _json(report_path)
    bad_groups = [g for g in report["groups"] if not g["mnemonic"].startswith(NAN_GROUP_PREFIX)]
    nan_only = sum(g["count"] for g in report["groups"] if g["mnemonic"].startswith(NAN_GROUP_PREFIX))
    ok = report["lift_fail"] == 0 and report["compile_fail"] == 0 and not bad_groups
    detail = (f"tested {report['tested']}, passed {report['passed']}, mismatched {report['mismatched']}, "
              f"nan-only {nan_only}, lift_fail {report['lift_fail']}, compile_fail {report['compile_fail']}")
    return CheckResult("leaf fuzz", ok, detail,
                       [f"  {g['mnemonic']} x{g['count']}" for g in bad_groups])


def run_fuzz(binary: Path, analysis: Path, switch: Path, work: Path) -> CheckResult:
    cmd = [sys.executable, "-m", "tools.hobbit.fuzz_leaf", "--binary", str(binary),
           "--analysis", str(analysis), "--switch", str(switch), "--work", str(work)]
    (work / "report.json").unlink(missing_ok=True)
    proc = subprocess.run(cmd, cwd=ENGINE_ROOT, check=False)
    report_path = work / "report.json"
    if proc.returncode != 0 or not report_path.is_file():
        return CheckResult("leaf fuzz", False, f"fuzzer exited {proc.returncode}, no report")
    return read_fuzz_report(report_path)


def print_results(results: list[CheckResult]) -> int:
    failed = 0
    for res in results:
        print(f"{'PASS' if res.ok else 'FAIL'}  {res.name}: {res.detail}")
        for line in res.lines:
            print(line)
        failed += not res.ok
    print("recomp-check: PASS" if failed == 0 else f"recomp-check: FAIL ({failed} checks)")
    return 1 if failed else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").split("\n")[0])
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--generated", type=Path, default=None)
    parser.add_argument("--switch", type=Path, default=None)
    parser.add_argument("--fuzz", action="store_true")
    args = parser.parse_args(argv)
    prof = game_profile.load(args.profile)
    generated = args.generated or prof.root / "generated.nosync"
    switch = args.switch or prof.switch_tables
    results = run_static_checks(generated, switch, switch.parent / "disasm_stats.json",
                                generated / "lift_report.json")
    if args.fuzz:
        work = prof.root / "build.nosync" / "checks" / "fuzz"
        work.mkdir(parents=True, exist_ok=True)
        results.append(run_fuzz(prof.binary, prof.analysis, switch, work))
    return print_results(results)


if __name__ == "__main__":
    sys.exit(main())
