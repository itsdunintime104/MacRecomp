"""boot-check: start a game unattended with a copy of its saves, stop it, and judge the log.

Passes only when the game stayed up, rendered, uploaded textures, played audio and logged no
errors (GL errors, translation gaps, unsupported calls, heap-guard reports, assertions).
Each check prints one `PASS|FAIL  <name>: <detail>` line.

Usage: python -m tools.checks.boot_check --profile <game.toml> [--seconds 60] [--heap-guard]
"""
from __future__ import annotations

import argparse
import datetime
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from tools.checks.recomp_check import CheckResult
from tools.game import profile as game_profile

STATS_PREFIX = "stats:"
STATS_FIELD_RE = re.compile(r"(\w+)=(-?[0-9.]+)")
UNSUPPORTED_RE = re.compile(r"^UNSUPPORTED \S+: ")
GL_DRIVER_NOTICE_PREFIX = "UNSUPPORTED (log once)"
MIN_DRAWS_PER_SWAP = 50
MAX_LISTED = 10
KILL_GRACE_SECONDS = 5
STATS_INTERVAL = 10
GUARD_PREFIXES = ("guest_heap:", "guest_heap_guard:")
GUARD_DISABLED_SUFFIX = "guard disabled"
GAME_ENV = {
    "RECOMP_ASSUME_FOCUS": "1",
    "RECOMP_NO_ALERT_UI": "1",
    "RECOMP_STATS": str(STATS_INTERVAL),
    "RECOMP_GL_CHECK": "1",
}
HEAP_GUARD_ENV = {"RECOMP_HEAP_GUARD": "2"}
EXIT_REFUSED = 2


def last_stats(log_text: str) -> dict[str, float] | None:
    """Fields of the last `stats:` line, or None when the log has none."""
    found: dict[str, float] | None = None
    for line in log_text.splitlines():
        if line.startswith(STATS_PREFIX):
            found = {k: float(v) for k, v in STATS_FIELD_RE.findall(line)}
    return found


def _lines_with(log_text: str, needle: str) -> list[str]:
    return [line for line in log_text.splitlines() if needle in line]


def _listed(lines: list[str]) -> list[str]:
    return [f"  {line}" for line in lines[:MAX_LISTED]]


def _stat_check(name: str, stats: dict[str, float] | None, key: str, minimum: float) -> CheckResult:
    if stats is None:
        return CheckResult(name, False, "no stats line in the log")
    value = stats.get(key, 0.0)
    return CheckResult(name, value > minimum, f"{key}={value:g} (needs > {minimum:g})")


def _heap_lines(log_text: str, heap_guard: bool) -> list[str]:
    lines = [ln for ln in log_text.splitlines() if ln.startswith(GUARD_PREFIXES)]
    if heap_guard:
        return lines
    return [ln for ln in lines if not ln.rstrip().endswith(GUARD_DISABLED_SUFFIX)]


def _fresh_check(stats: dict[str, float] | None, seconds: int) -> CheckResult:
    """The last stats line must be recent, or the game stalled before the run ended."""
    if stats is None:
        return CheckResult("stats fresh", False, "no stats line in the log")
    newest = stats.get("t", 0.0)
    oldest_ok = seconds - 2 * STATS_INTERVAL
    return CheckResult("stats fresh", newest >= oldest_ok,
                       f"last stats t={newest:g} (needs >= {oldest_ok:g} for a {seconds} s run)")


def judge(log_text: str, exited_early: bool, seconds: int, *, exit_code: int | None = None,
          heap_guard: bool = False) -> list[CheckResult]:
    stats = last_stats(log_text)
    gl_lines = _lines_with(log_text, "gl: error")
    gl_total = int(stats.get("gl_errors", 0)) if stats else 0
    recomp_lines = _lines_with(log_text, "RECOMP:")
    unsupported = [ln for ln in log_text.splitlines() if UNSUPPORTED_RE.match(ln)]
    driver_notices = [ln for ln in log_text.splitlines() if ln.startswith(GL_DRIVER_NOTICE_PREFIX)]
    unsupported_detail = f"{len(unsupported)} UNSUPPORTED lines"
    if driver_notices:
        unsupported_detail += f" ({len(driver_notices)} GL driver notice, not counted)"
    heap_lines = _heap_lines(log_text, heap_guard)
    asserts = _lines_with(log_text, "Assertion")
    if exited_early:
        alive = CheckResult("alive", False, f"exited early with code {exit_code} (wanted {seconds} s)")
    else:
        alive = CheckResult("alive", True, f"still running after {seconds} s")
    return [
        alive,
        _fresh_check(stats, seconds),
        _stat_check("rendering", stats, "draws_per_swap", MIN_DRAWS_PER_SWAP),
        _stat_check("textures", stats, "tex_uploads", 0),
        _stat_check("audio", stats, "audio_renders", 0),
        CheckResult("GL errors", not gl_lines and gl_total == 0,
                    f"{len(gl_lines)} gl: lines, gl_errors={gl_total}", _listed(gl_lines)),
        CheckResult("translation gaps", not recomp_lines, f"{len(recomp_lines)} RECOMP: lines",
                    _listed(recomp_lines)),
        CheckResult("unsupported calls", not unsupported, unsupported_detail, _listed(unsupported)),
        CheckResult("heap", not heap_lines, f"{len(heap_lines)} guest_heap lines", _listed(heap_lines)),
        CheckResult("assertions", not asserts, f"{len(asserts)} Assertion lines", _listed(asserts)),
    ]


def info_line(log_text: str, log_path: Path) -> str:
    stats = last_stats(log_text) or {}
    return (f"info  swaps={stats.get('swaps', 0):g} cg_programs={stats.get('cg_programs', 0):g} "
            f"cg_ms={stats.get('cg_ms', 0):g} log={log_path}")


def running_pids(game_id: str) -> list[int]:
    proc = subprocess.run(["pgrep", "-x", game_id], capture_output=True, text=True, check=False)
    return [int(tok) for tok in proc.stdout.split()]


def prepare_saves(support_dir: str, scratch: Path) -> Path:
    """Copy the real saves (if any) into scratch; the game only ever sees the copy."""
    saves = scratch / "saves"
    real = Path.home() / "Library" / "Application Support" / support_dir / "remote"
    if real.is_dir():
        shutil.copytree(real, saves)
    else:
        saves.mkdir()
    return saves


def run_game(cmd: list[str], env: dict[str, str], log_path: Path, seconds: int) -> tuple[bool, int | None]:
    """Run cmd for `seconds`, then stop it. Returns (exited_early, exit code when it exited early)."""
    with log_path.open("wb") as log:
        proc = subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                code = proc.poll()
                if code is not None:
                    return True, code
                time.sleep(1)
        finally:
            if proc.poll() is None:
                _stop(proc)
    return False, None


def _stop(proc: subprocess.Popen[bytes]) -> None:
    """SIGTERM, wait the grace period, then kill."""
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(KILL_GRACE_SECONDS)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def print_results(results: list[CheckResult], info: str) -> int:
    failed = 0
    for res in results:
        print(f"{'PASS' if res.ok else 'FAIL'}  {res.name}: {res.detail}")
        for line in res.lines:
            print(line)
        failed += not res.ok
    print(info)
    print("boot-check: PASS" if failed == 0 else f"boot-check: FAIL ({failed} checks)")
    return 1 if failed else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").split("\n")[0])
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--heap-guard", action="store_true")
    args = parser.parse_args(argv)
    if args.seconds < 3 * STATS_INTERVAL:
        parser.error(f"--seconds must be at least {3 * STATS_INTERVAL}")
    prof = game_profile.load(args.profile)
    pids = running_pids(prof.id)
    if pids:
        print(f"boot-check: refused, {prof.id} is already running (pid {pids[0]}); quit it first",
              file=sys.stderr)
        return EXIT_REFUSED
    exe = prof.root / "build.nosync" / prof.id
    if not exe.is_file():
        print(f"boot-check: missing executable {exe}; build the game first", file=sys.stderr)
        return EXIT_REFUSED
    log_dir = prof.root / "build.nosync" / "checks"
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / f"boot-{datetime.datetime.now():%Y%m%d-%H%M%S}.log"
    scratch = Path(tempfile.mkdtemp(prefix="boot-check-"))
    try:
        env = {**os.environ, **GAME_ENV, **(HEAP_GUARD_ENV if args.heap_guard else {}),
               f"{prof.env_prefix}_SAVE_DIR": str(prepare_saves(prof.support_dir, scratch))}
        exited_early, code = run_game([str(exe), str(prof.binary)], env, log_path, args.seconds)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    text = log_path.read_text(errors="replace")
    results = judge(text, exited_early, args.seconds, exit_code=code, heap_guard=args.heap_guard)
    return print_results(results, info_line(text, log_path))


if __name__ == "__main__":
    sys.exit(main())
