"""A function whose entry jcc reads ZF its callers set (--entry-zf).

LEGO Harry Potter's x87 libm entries (sub_00A69438 and six more) open with
`push edx; wait; fnstcw [esp]; je`, and every caller reaches them straight
from __fload_withFB, which leaves ZF = ((eax & 0x7FF00000) == 0x7FF00000):
the double is Inf or NaN. The snippet reloads eax before the je, as
sub_00A694DD does, so the snapshot must be taken on entry.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from tests.conformance.harness import (
    Case, assemble, compile_native, initial_state, lift, native_source, run_native)
from tools.xrecomp.run import read_entry_zf
from tools.xrecomp.translator import FunctionTranslator

ASM = "push edx\nmov eax, 5\nje special\nmov edx, 7\nspecial:\npop ecx\nret"


@pytest.mark.parametrize("hi,taken", [(0x7FF80000, True), (0xFFF00000, True),
                                      (0x3FF00000, False), (0x7FE00000, False)])
def test_entry_jcc_reads_the_callers_zf(tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
                                        hi: int, taken: bool) -> None:
    spec = tmp_path / "entry_zf.json"
    spec.write_text(json.dumps({"0x10000": {"reg": "eax", "mask": "0x7FF00000",
                                            "value": "0x7FF00000"}}))
    monkeypatch.setattr(FunctionTranslator, "entry_zf", read_entry_zf(spec))
    case = Case("entry_zf", ASM, regs={"eax": hi, "edx": 0})
    c = lift(assemble(case), tmp_path)
    assert "_flags /*" not in c
    binary = tmp_path / "entry_zf"
    compile_native(native_source(c), tmp_path, binary)
    out = run_native(binary, initial_state(case, 1), tmp_path)
    assert out["edx"] == (0 if taken else 7)


def test_without_the_option_the_entry_jcc_is_unknown(tmp_path: Path) -> None:
    c = lift(assemble(Case("entry_zf_off", ASM)), tmp_path)
    assert "_flags /* je" in c
