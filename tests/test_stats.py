"""Compile and run the C check for runtime/src/stats.c."""

from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [ROOT / "runtime" / "src" / "stats.c", ROOT / "runtime" / "tests" / "test_stats.c"]


def test_stats(tmp_path: Path) -> None:
    exe = tmp_path / "test_stats"
    subprocess.run(
        [
            "clang", "-std=gnu11", "-O1", "-w", "-arch", "arm64",
            "-I", str(ROOT / "runtime" / "include"),
            *map(str, SOURCES),
            "-framework", "OpenGL",
            "-o", str(exe),
        ],
        check=True,
    )
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    assert "stats ok" in result.stdout
