"""Game profile and derive: the derived Hobbit values equal the values that were typed by hand."""
from __future__ import annotations

import dataclasses
import json
import re
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest

from tools import refgame
from tools.game import derive, feral, profile

ROOT = Path(__file__).resolve().parent.parent
GAME_TOML = refgame.GAME_TOML
FERAL_TOML = ROOT / "tools" / "game" / "feral_frontend.toml"
FIXTURES = ROOT / "runtime" / "tests" / "include"

ENTRY = 0x2720
STATIC_BASE = 0x019D0000
NUMAIN_CFSTRING = 0x016876D4
OVERRIDES = ["0x003D18A0", "0x003D19C0", "0x003D1A80", "0x003D1AE0", "0x003D1AF0"]
AFTER_RULE = {"fe_screen_size", "fe_make_current", "fe_swap_buffers", "fe_backing_size", "pad_release"}
SHELL_KEYS = [
    "GAME_ROOT", "GAME_ID", "GAME_TITLE", "WINDOW_TITLE", "STEAM_APP_ID", "STEAM_BUILD", "STEAM_APP",
    "GAME_EXE", "BINARY_SHA256", "BINARY", "ANALYSIS", "SWITCH_TABLES", "IMPORT_SIGS", "ENV_PREFIX", "APP_NAME",
    "APP_EXE", "BUNDLE_ID", "SUPPORT_DIR", "OLD_APP_NAME",
]


@pytest.fixture(scope="module")
def image(binary_path: Path) -> feral.Image:
    return feral.load_image(binary_path)


@pytest.fixture(scope="module")
def frontend() -> feral.Frontend:
    return feral.load_file(FERAL_TOML)


@pytest.fixture(scope="module")
def derived_dir(tmp_path_factory: pytest.TempPathFactory) -> Path:
    out = tmp_path_factory.mktemp("derived")
    assert derive.main(["--profile", str(GAME_TOML), "--out", str(out)]) == 0
    return out


def test_refresh_is_current(image: feral.Image, frontend: feral.Frontend) -> None:
    assert feral.render(feral.refresh(frontend, image)) == FERAL_TOML.read_text()


def test_hobbit_values_equal_hand_typed(
    derived_dir: Path, frontend: feral.Frontend
) -> None:
    doc: dict[str, Any] = json.loads((derived_dir / "derived.json").read_text())
    assert doc["entry"] == ENTRY
    assert doc["static_base"] == STATIC_BASE
    assert doc["cfstr_numain"] == NUMAIN_CFSTRING
    for fn in frontend.functions:
        assert doc["functions"][fn.name]["va"] == fn.ref, fn.name
    assert {n for n, f in doc["functions"].items() if f["rule"] == "after"} == AFTER_RULE
    for d in frontend.data:
        assert doc["data"][d.name] == d.ref, d.name

    config = (derived_dir / "game_config.h").read_text()
    assert "#define GAME_ENTRY_VA 0x00002720u" in config
    assert "#define GAME_STATIC_BASE 0x019D0000u" in config
    assert "#define GAME_CFSTR_NUMAIN 0x016876D4u" in config
    assert '#define GAME_ENV_SAVE_DIR "HOBBIT_SAVE_DIR"' in config

    overrides = [
        line for line in (derived_dir / "native_overrides.txt").read_text().splitlines()
        if not line.startswith("#")
    ]
    assert overrides == OVERRIDES

    def defines(path: Path) -> list[str]:
        return [ln for ln in path.read_text().splitlines() if ln.startswith("#define sub_")]

    assert len(defines(derived_dir / "frontend_rename.h")) == 19
    refs = sorted(fn.ref for fn in frontend.functions if fn.role == feral.Role.FRONTEND)
    assert defines(derived_dir / "frontend_rename.h") == [
        f"#define sub_{va:08X} lifted_sub_{va:08X}" for va in refs
    ]

    mk = (derived_dir / "game.mk").read_text()
    assert len(re.findall(r"sub_[0-9A-F]{8}", mk)) == 19


def test_fixtures_match_derived(derived_dir: Path) -> None:
    for name in ("game_config.h", "feral_symbols.h", "gen_segments.h"):
        assert (FIXTURES / name).read_bytes() == (derived_dir / name).read_bytes(), name


def test_ambiguity_fails(image: feral.Image, tmp_path: Path) -> None:
    text = FERAL_TOML.read_text()
    assert text.count('after = "fe_screen_size"\n') == 1
    copy = tmp_path / "feral.toml"
    copy.write_text(text.replace('after = "fe_screen_size"\n', ""))
    with pytest.raises(feral.FeralError, match=r"fe_make_current.*matches 2 functions"):
        feral.resolve(image, feral.load_file(copy), {})


def test_pin_wins(image: feral.Image, frontend: feral.Frontend) -> None:
    resolved = feral.resolve(image, frontend, {"fe_alert": 0x3DF8F0})
    assert resolved["fe_alert"] == feral.Resolved(0x3DF8F0, feral.Rule.PINNED, 0)


def test_profile_rejects_bad_input(tmp_path: Path) -> None:
    text = GAME_TOML.read_text()
    cases = {
        "id": text.replace('id = "hobbit"', 'id = "Hobbit"'),
        "title": text.replace('title = "LEGO The Hobbit"', 'title = "LEGO $HOME"'),
        "window_title": text.replace('window_title = "The Hobbit"', 'window_title = "100%"'),
        "extra": text.replace('env_prefix = "HOBBIT"', 'env_prefix = "HOBBIT"\nextra = "x"'),
    }
    for key, bad in cases.items():
        path = tmp_path / f"{key}.toml"
        path.write_text(bad)
        with pytest.raises(ValueError, match=f"'{key}'"):
            profile.load(path)
    assert dataclasses.is_dataclass(profile.load(GAME_TOML))


def test_profile_shell_output() -> None:
    out = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "game" / "profile.py"), "--shell", str(GAME_TOML)],
        check=True, capture_output=True, text=True,
    ).stdout.splitlines()
    assert [line.split("=", 1)[0] for line in out] == SHELL_KEYS
    assert "GAME_ID=hobbit" in out
    assert "STEAM_APP_ID=285160" in out
    assert f"BINARY={shlex.quote(str(refgame.GAME_ROOT / 'orig.nosync/hobbit_i386'))}" in out
