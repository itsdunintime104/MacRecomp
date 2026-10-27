"""recon_check rules on small hand-assembled byte strings."""

from __future__ import annotations

import importlib.util
from pathlib import Path
from typing import Any


_PATH = Path(__file__).resolve().parents[1] / "tools" / "pe" / "recon_check.py"
_SPEC = importlib.util.spec_from_file_location("recon_check", _PATH)
assert _SPEC is not None and _SPEC.loader is not None
R = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(R)

BASE = 0x1000


def code(raw: bytes) -> Any:
    return R.A.Code(raw, BASE, BASE, BASE + len(raw))


def test_ilt_slots_run_from_the_text_start_past_int3() -> None:
    raw = b"\xcc" + bytes.fromhex("e900000000") * 3 + b"\xc3"
    assert R.ilt_slots(code(raw)) == range(0x1001, 0x1010, 5)


def test_text_dwords_are_strings_not_pointers() -> None:
    ascii_end = code(b"cket" + b"rol\0" + bytes(4))
    utf16 = code("spspsp".encode("utf-16-le"))
    pointer = code(bytes(4) + (0x00401000).to_bytes(4, "little") + bytes(4))
    assert R._is_text(ascii_end, BASE + 4)
    assert R._is_text(utf16, BASE + 4)
    assert not R._is_text(pointer, BASE + 4)

