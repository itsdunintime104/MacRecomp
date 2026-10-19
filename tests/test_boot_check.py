"""boot-check: verdicts on synthetic logs (no game run)."""
from __future__ import annotations

import subprocess
import types
from pathlib import Path

import pytest

from tools.checks import boot_check
from tools.checks.recomp_check import CheckResult

GOOD_STATS = ("stats: t=60 swaps=9307 draws_per_swap=154.8 tex_uploads=17934 audio_renders=24726 "
              "cg_programs=0 cg_ms=0 gl_errors=0")
GOOD_LOG = "starting\n" + GOOD_STATS + "\n"


def _failed(results: list[CheckResult]) -> list[str]:
    return [r.name for r in results if not r.ok]


def test_good_log_passes() -> None:
    assert _failed(boot_check.judge(GOOD_LOG, False, 60)) == []


def test_last_stats_line_wins() -> None:
    log = "stats: t=10 swaps=1 draws_per_swap=1.0 tex_uploads=0 audio_renders=0 cg_programs=0 cg_ms=0 gl_errors=0\n" + GOOD_LOG
    assert _failed(boot_check.judge(log, False, 60)) == []


def test_early_exit_fails_alive() -> None:
    results = boot_check.judge(GOOD_LOG, True, 60, exit_code=139)
    assert _failed(results) == ["alive"]
    assert "139" in results[0].detail


@pytest.mark.parametrize("field, bad, check", [
    ("draws_per_swap=154.8", "draws_per_swap=10.0", "rendering"),
    ("tex_uploads=17934", "tex_uploads=0", "textures"),
    ("audio_renders=24726", "audio_renders=0", "audio"),
    ("gl_errors=0", "gl_errors=3", "GL errors"),
])
def test_bad_stat_fails_one_check(field: str, bad: str, check: str) -> None:
    assert _failed(boot_check.judge(GOOD_LOG.replace(field, bad), False, 60)) == [check]


def test_no_stats_line_fails_the_three_counters() -> None:
    assert _failed(boot_check.judge("starting\n", False, 60)) == ["stats fresh", "rendering", "textures", "audio"]


def test_stale_stats_fail_stats_fresh() -> None:
    stale = GOOD_LOG.replace("t=60", "t=20")
    assert _failed(boot_check.judge(stale, False, 60)) == ["stats fresh"]


@pytest.mark.parametrize("line, check", [
    ("gl: error 0x0502 first at swap 15", "GL errors"),
    ("RECOMP: unimplemented cvtdq2pd at 0x00401234", "translation gaps"),
    ("UNSUPPORTED call: CGLFoo", "unsupported calls"),
    ("guest_heap_guard: overflow block 0x00123456 size 16 alloc site 0x00401000", "heap"),
    ("guest_heap: heap check failed (dlmalloc.c:99)", "heap"),
    ("Assertion failed: x > 0", "assertions"),
])
def test_error_line_fails_its_check(line: str, check: str) -> None:
    results = boot_check.judge(GOOD_LOG + line + "\n", False, 60)
    assert _failed(results) == [check]
    failing = next(r for r in results if r.name == check)
    assert any(line in listed for listed in failing.lines)


def test_listing_is_capped() -> None:
    log = GOOD_LOG + "".join(f"RECOMP: gap {i}\n" for i in range(30))
    failing = next(r for r in boot_check.judge(log, False, 60) if r.name == "translation gaps")
    assert len(failing.lines) == boot_check.MAX_LISTED


def test_guard_disabled_notice_fails_only_with_heap_guard() -> None:
    log = GOOD_LOG + "guest_heap_guard: out of memory, guard disabled\n"
    assert _failed(boot_check.judge(log, False, 60)) == []
    assert _failed(boot_check.judge(log, False, 60, heap_guard=True)) == ["heap"]


def test_run_game_stops_the_child_when_interrupted(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    started: list[subprocess.Popen[bytes]] = []
    real_popen = subprocess.Popen

    def recording_popen(*args: object, **kwargs: object) -> subprocess.Popen[bytes]:
        proc: subprocess.Popen[bytes] = real_popen(*args, **kwargs)  # type: ignore[call-overload]
        started.append(proc)
        return proc

    def interrupt(_seconds: float) -> None:
        raise KeyboardInterrupt

    monkeypatch.setattr(subprocess, "Popen", recording_popen)
    monkeypatch.setattr(boot_check, "time", types.SimpleNamespace(monotonic=boot_check.time.monotonic,
                                                                  sleep=interrupt))
    with pytest.raises(KeyboardInterrupt):
        boot_check.run_game(["sleep", "30"], {}, tmp_path / "boot.log", 60)
    assert started[0].poll() is not None


def test_info_line_reports_cg_and_log_path() -> None:
    line = boot_check.info_line(GOOD_LOG, Path("/x/boot.log"))
    assert "swaps=9307" in line and "cg_programs=0" in line and "/x/boot.log" in line


def _write_profile(root: Path) -> Path:
    path = root / "game.toml"
    path.write_text(
        'id = "toy"\ntitle = "Toy"\nwindow_title = "Toy"\nsteam_app_id = 1\nsteam_build = "1"\n'
        'steam_app = "Toy"\ngame_exe = "toy.exe"\nbinary_sha256 = "' + "0" * 64 + '"\n'
        'binary = "toy.exe"\nanalysis = "a.json"\nswitch_tables = "s.json"\nimport_sigs = "i.json"\n'
        'env_prefix = "TOY"\n[app]\nname = "Toy"\nexe = "toy"\nbundle_id = "com.example.toy"\n'
        'support_dir = "Toy"\n')
    return path


def test_refuses_when_game_is_running(tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
                                      capsys: pytest.CaptureFixture[str]) -> None:
    monkeypatch.setattr(boot_check, "running_pids", lambda game_id: [4242])
    monkeypatch.setattr(boot_check, "run_game", lambda *a, **k: pytest.fail("game must not start"))
    assert boot_check.main(["--profile", str(_write_profile(tmp_path))]) == boot_check.EXIT_REFUSED
    assert "already running" in capsys.readouterr().err


def test_missing_executable_is_refused(tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
                                       capsys: pytest.CaptureFixture[str]) -> None:
    monkeypatch.setattr(boot_check, "running_pids", lambda game_id: [])
    assert boot_check.main(["--profile", str(_write_profile(tmp_path))]) == boot_check.EXIT_REFUSED
    assert "missing executable" in capsys.readouterr().err


def test_saves_are_copied_not_shared(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    home = tmp_path / "home"
    real = home / "Library" / "Application Support" / "Toy" / "remote"
    real.mkdir(parents=True)
    (real / "slot1").write_text("save")
    monkeypatch.setattr(Path, "home", lambda: home)
    scratch = tmp_path / "scratch"
    scratch.mkdir()
    saves = boot_check.prepare_saves("Toy", scratch)
    assert (saves / "slot1").read_text() == "save"
    assert real not in saves.parents and saves != real
    empty = tmp_path / "scratch2"
    empty.mkdir()
    assert list(boot_check.prepare_saves("Missing", empty).iterdir()) == []
