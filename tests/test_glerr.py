"""Build glerr.dylib and check it names the GL call that raises an error."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
GAME_ROOT = ROOT.parent
NO_RENDERER = 77


def test_glerr_names_the_failing_call(tmp_path: Path) -> None:
    subprocess.run(["make", "-C", str(ROOT / "build"), "glerr"], check=True)
    dylib = GAME_ROOT / "build.nosync" / "glerr.dylib"
    exe = tmp_path / "glerr_test"
    subprocess.run(
        ["clang", "-std=gnu11", "-O1", "-w", "-arch", "arm64",
         str(ROOT / "tools" / "glcount" / "glerr_test.c"), "-framework", "OpenGL", "-o", str(exe)],
        check=True,
    )
    plain = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    if plain.returncode == NO_RENDERER:
        pytest.skip("no GL renderer available")
    assert plain.returncode == 0, plain.stderr
    result = subprocess.run(
        [str(exe)], capture_output=True, text=True, timeout=60,
        env={**os.environ, "DYLD_INSERT_LIBRARIES": str(dylib)},
    )
    assert result.returncode == 0, result.stderr
    lines = result.stderr.splitlines()
    assert lines.count("glerr: glEnd -> 0x0502") == 1, result.stderr
    assert not any(line.startswith("glerr: glBindTexture") for line in lines), result.stderr
