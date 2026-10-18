from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

from tools import refgame
from tools.hobbit import analyze, disasm


@pytest.fixture(scope="session")
def binary_path() -> Path:
    path = refgame.binary()
    if not path.is_file():
        pytest.fail(f"game binary not found: {path}")
    return path


@pytest.fixture(scope="session")
def binary_data(binary_path: Path) -> bytes:
    return binary_path.read_bytes()


@pytest.fixture(scope="session")
def analysis(binary_data: bytes, tmp_path_factory: pytest.TempPathFactory) -> dict[str, Any]:
    out = tmp_path_factory.mktemp("analysis") / "analysis.json"
    out.write_text(json.dumps(analyze.analyze(binary_data)))
    return json.loads(out.read_text())


@pytest.fixture(scope="session")
def sweep(
    analysis: dict[str, Any], binary_data: bytes
) -> tuple[dict[str, Any], dict[str, Any]]:
    return disasm.sweep(analysis, binary_data)
