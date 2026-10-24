"""The lifter hands ebp across calls to every named SEH frame helper.

MSVC 8 builds a frame with one of several prologs (__SEH_prolog4,
__EH_prolog3*) and tears it down with one of several epilogs. ebp is a C local
in the lifted code, so a caller publishes it to g_seh_ebp before the call and
reads it back after, and the helper publishes it before its ret.
"""
from __future__ import annotations

import json
import struct
from pathlib import Path

from tools.xrecomp import config
from tools.xrecomp.translator import BatchTranslator

BASE = 0x00010000
PROLOGS = (BASE + 0x40, BASE + 0x41)
EPILOGS = (BASE + 0x60, BASE + 0x61)
PUBLISH = "g_seh_ebp = ebp; /* publish frame to SEH helper */"
READ_BACK = "ebp = g_seh_ebp; /* read back frame from SEH helper */"


def _call(at: int, target: int) -> bytes:
    return b"\xe8" + struct.pack("<i", target - (at + 5))


def _caller(at: int, prolog: int, epilog: int) -> bytes:
    """push 0x10; push 0x12345678; call prolog; mov eax,[ebp-4]; call epilog; ret"""
    code = b"\x6a\x10" + b"\x68\x78\x56\x34\x12"
    code += _call(at + len(code), prolog)
    code += b"\x8b\x45\xfc"
    code += _call(at + len(code), epilog)
    return code + b"\xc3"


def _translator(tmp_path: Path, seh_prolog: int | set[int],
                seh_epilog: int | set[int]) -> BatchTranslator:
    image = bytearray(b"\xcc" * 0x80)
    callers = (BASE, BASE + 0x20)
    for at, prolog, epilog in zip(callers, PROLOGS, EPILOGS):
        code = _caller(at, prolog, epilog)
        image[at - BASE:at - BASE + len(code)] = code
    for helper in PROLOGS + EPILOGS:
        image[helper - BASE] = 0xC3
    (tmp_path / "image.bin").write_bytes(bytes(image))
    analysis = {"segments": [{"name": "__TEXT", "sections": [
        {"name": "__text", "addr": BASE, "size": len(image), "offset": 0, "flags": 0}]}]}
    config.configure_from_macho(analysis, tmp_path / "image.bin", entry_point=BASE)
    funcs = [(a, a + 0x15) for a in callers] + [(h, h + 1) for h in PROLOGS + EPILOGS]
    (tmp_path / "functions.json").write_text(json.dumps([
        {"start": f"0x{s:08X}", "end": f"0x{e:08X}", "name": f"sub_{s:08X}"}
        for s, e in funcs]))
    (tmp_path / "labels.json").write_text("[]")
    (tmp_path / "reg_switches.json").write_text("{}")
    return BatchTranslator(
        str(tmp_path / "image.bin"), str(tmp_path / "functions.json"),
        labels_json_path=str(tmp_path / "labels.json"), output_dir=str(tmp_path),
        image_format="macho", reg_switch_json_path=str(tmp_path / "reg_switches.json"),
        import_stubs={}, seh_prolog=seh_prolog, seh_epilog=seh_epilog)


def _lift(bt: BatchTranslator, addr: int) -> str:
    c = bt.translate_single(addr)
    assert c, bt.failures
    return c


def _call_lines(c: str, target: int) -> tuple[list[str], str, str]:
    """(global stores just before, call line, line after) of the call to target."""
    lines = [ln.strip() for ln in c.splitlines()]
    i = next(k for k, ln in enumerate(lines) if f"RECOMP_ABI_CALL(0x{target:08X}u" in ln)
    j = i
    while lines[j - 1].startswith("g_"):
        j -= 1
    return lines[j:i], lines[i], lines[i + 1]


def test_every_prolog_in_the_set_is_read_back(tmp_path: Path) -> None:
    bt = _translator(tmp_path, set(PROLOGS), set(EPILOGS))
    for caller, prolog in zip((BASE, BASE + 0x20), PROLOGS):
        before, _, after = _call_lines(_lift(bt, caller), prolog)
        assert after == READ_BACK, (hex(caller), after)
        assert PUBLISH in before, (hex(caller), before)


def test_every_epilog_in_the_set_is_published(tmp_path: Path) -> None:
    bt = _translator(tmp_path, set(PROLOGS), set(EPILOGS))
    for caller, epilog in zip((BASE, BASE + 0x20), EPILOGS):
        before, _, after = _call_lines(_lift(bt, caller), epilog)
        assert PUBLISH in before, (hex(caller), before)
        assert after == READ_BACK, (hex(caller), after)


def test_helpers_publish_ebp_before_ret(tmp_path: Path) -> None:
    bt = _translator(tmp_path, set(PROLOGS), set(EPILOGS))
    for helper in PROLOGS + EPILOGS:
        assert "g_seh_ebp = ebp; esp += 4; return;" in _lift(bt, helper), hex(helper)


def test_single_epilog_address_still_works(tmp_path: Path) -> None:
    bt = _translator(tmp_path, PROLOGS[0], EPILOGS[0])
    assert bt.translator.lifter.SEH_EPILOG == EPILOGS[0]
    before, _, after = _call_lines(_lift(bt, BASE), EPILOGS[0])
    assert PUBLISH in before and after == READ_BACK
    _, _, after = _call_lines(_lift(bt, BASE + 0x20), EPILOGS[1])
    assert after != READ_BACK
