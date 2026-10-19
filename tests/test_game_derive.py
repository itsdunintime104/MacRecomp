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
AFTER_RULE = {"fe_make_current", "fe_swap_buffers", "fe_backing_size", "pad_release", "view_mouse_button"}
NEXT_RULE = {"fe_screen_size"}
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
    assert {n for n, f in doc["functions"].items() if f["rule"] == "next"} == NEXT_RULE
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


def test_mask_is_instruction_aware() -> None:
    code = bytes.fromhex(
        "8b8800c95c01"          # mov ecx, [eax+0x15cc900]: the whole disp32 is masked, including c9
        "e801020304"            # call rel32
        "c7442404" "00104000"   # mov dword [esp+4], 0x401000: immediate inside the image
        "8b4dcc"                # mov ecx, [ebp-0x34]: small disp stays
        "c785b4fcffff" "05000000"  # mov dword [ebp-0x34c], 5: small disp and small imm stay
    )
    pat = feral._mask(code, 0x2000, 0x1000, 0x2000000)
    assert pat == [
        0x8B, 0x88, None, None, None, None,
        0xE8, None, None, None, None,
        0xC7, 0x44, 0x24, 0x04, None, None, None, None,
        0x8B, 0x4D, 0xCC,
        0xC7, 0x85, 0xB4, 0xFC, 0xFF, 0xFF, 0x05, 0x00, 0x00, 0x00,
    ]
    long = b"\x90" * (feral.SIG_BYTES - 1) + bytes.fromhex("8b8800100000")  # insn crosses SIG_BYTES
    pat = feral._mask(long, 0x2000, 0x1000, 0x2000000)
    assert len(pat) == feral.SIG_BYTES + 5 and pat[-4:] == [None] * 4


def with_functions(frontend: feral.Frontend, **changes: dict[str, Any]) -> feral.Frontend:
    funcs = tuple(
        dataclasses.replace(f, **changes[f.name]) if f.name in changes else f for f in frontend.functions
    )
    return dataclasses.replace(frontend, functions=funcs)


def test_next_rule(image: feral.Image, frontend: feral.Frontend) -> None:
    unique = feral.resolve(image, frontend, {})["fe_set_view_frame"]
    assert (unique.va, unique.rule) == (0x3DCDF0, feral.Rule.UNIQUE)
    lost = with_functions(frontend, fe_set_view_frame={"sig": "00 00 00 00 00 00 00 00 00 00 00 00"})
    nxt = feral.resolve(image, lost, {})["fe_set_view_frame"]
    assert (nxt.va, nxt.rule, nxt.matches) == (0x3DCDF0, feral.Rule.NEXT, 0)
    both = with_functions(frontend, fe_make_current={"next": "fe_alert"})  # `after` outranks `next`
    after = feral.resolve(image, both, {})["fe_make_current"]
    assert (after.va, after.rule) == (0x3DE7F0, feral.Rule.AFTER)
    cycle = with_functions(
        lost, fe_view_frame={"sig": "00 00 00 00 00 00 00 00 00 00 00 00", "next": "fe_set_view_frame"}
    )
    with pytest.raises(feral.FeralError, match="cannot be resolved"):
        feral.resolve(image, cycle, {})


def test_absent_entries_are_skipped_and_flagged(image: feral.Image, frontend: feral.Frontend) -> None:
    absent = frozenset({"fe_backing_size", "G_INIT_ONCE"})
    prof = dataclasses.replace(profile.load(GAME_TOML), absent=tuple(sorted(absent)))
    derived = derive.compute(prof, frontend, image)
    assert "fe_backing_size" not in derived.functions and "G_INIT_ONCE" not in derived.data
    text = derive.render_feral_symbols(prof, frontend, derived)
    assert "#define FE_HAS_FE_BACKING_SIZE 0\n" in text and "#define FE_HAS_G_INIT_ONCE 0\n" in text
    assert "#define FE_HAS_G_RETINA 1\n" in text and "#define FE_HAS_FE_ALERT 1\n" in text
    assert "fe_backing_size" not in text and "G_INIT_ONCE 0x" not in text
    assert "003DFF70" not in text and "003DFF70" not in derive.render_frontend_rename(prof, frontend, derived)
    with pytest.raises(feral.FeralError, match="unknown entry nope"):
        feral.validate_absent(frontend, frozenset({"nope"}))


def test_profile_reads_absent(tmp_path: Path) -> None:
    path = tmp_path / "game.toml"
    path.write_text(GAME_TOML.read_text().replace('env_prefix = "HOBBIT"', 'env_prefix = "HOBBIT"\nabsent = ["a", "b"]'))
    assert profile.load(path).absent == ("a", "b")
    assert profile.load(GAME_TOML).absent == ()


def test_data_nth_operand(image: feral.Image, frontend: feral.Frontend) -> None:
    resolved = feral.resolve(image, frontend, {})
    entry = next(d for d in frontend.data if d.name == "G_WINDOWS")
    assert entry.func == "fe_open_window" and entry.insn is not None and entry.nth is not None
    beyond = dataclasses.replace(entry, nth=entry.nth + 1000)
    with pytest.raises(feral.FeralError, match=r"G_WINDOWS: fe_open_window has no operand #"):
        feral.derive_data(image, dataclasses.replace(frontend, data=(beyond,)), resolved)
    elsewhere = dataclasses.replace(entry, func="fe_pool_create")  # typed anchor without such an operand
    with pytest.raises(feral.FeralError, match="G_WINDOWS: no PIC-based operand in fe_pool_create"):
        feral.refresh(dataclasses.replace(frontend, data=(elsewhere,)), image)


def test_numain_absent(image: feral.Image, frontend: feral.Frontend) -> None:
    base = profile.load(GAME_TOML)
    present = derive.compute(base, frontend, image)
    assert present.cfstr_numain == NUMAIN_CFSTRING
    prof = dataclasses.replace(base, absent=("NuMain",))
    derived = derive.compute(prof, frontend, image)
    config = derive.render_game_config(prof, derived)
    assert "#define GAME_CFSTR_NUMAIN 0x00000000u\n" in config
    assert json.loads(derive.render_derived_json(prof, derived))["cfstr_numain"] == 0


def test_next_rule_pick_must_be_a_signature_match(image: feral.Image, frontend: feral.Frontend) -> None:
    make_current = next(f for f in frontend.functions if f.name == "fe_make_current")  # 2 matches
    wrong = with_functions(frontend, fe_set_view_frame={"sig": make_current.sig})
    with pytest.raises(feral.FeralError, match=r"fe_set_view_frame: rule 'next fe_view_frame' picks 0x003DCDF0, .* 2 signature"):
        feral.resolve(image, wrong, {})


def pic_operands(code: str) -> list[tuple[int, int]]:
    insns = list(feral._disassembler().disasm(bytes.fromhex(code.replace(" ", "")), 0x1000))
    return [(o.disp, o.base) for o in feral._track_pic(insns)]


def test_each_register_keeps_its_own_pic_base() -> None:
    assert pic_operands(
        "e800000000 5b"          # call $+5; pop ebx: base 0x1005
        "e800000000 59"          # call $+5; pop ecx: base 0x100b
        "8b8300200000"           # mov eax, [ebx+0x2000]
        "8b9100200000"           # mov edx, [ecx+0x2000]
    ) == [(0x2000, 0x1005), (0x2000, 0x100B)]


def test_spill_slot_loses_its_pic_base_when_written() -> None:
    head = "e800000000 58 8945fc"  # call $+5; pop eax; mov [ebp-4], eax
    use = "8b4dfc 8b9100200000"    # mov ecx, [ebp-4]; mov edx, [ecx+0x2000]
    assert pic_operands(head + use) == [(0x2000, 0x1005)]
    for write in ("c745fc00000000", "8355fc01", "8955fc"):  # mov imm, adc imm, mov from a register without a base
        assert pic_operands(head + write + use) == [], write
