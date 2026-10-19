"""recomp-check rules on a synthetic generated folder, plus the real Hobbit output."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from tools import refgame
from tools.checks.recomp_check import CheckResult, run_static_checks

TABLE_VA = 0x1000
SITES = (
    'RECOMP_UNIMPL("add al, byte ptr [eax]", 0x00001004u);\n'  # inside the table
    'RECOMP_UNIMPL("ud2", 0x00002000u);\n'  # compiler trap
    'RECOMP_UNIMPL("cvtdq2pd xmm0, xmm0", 0x00003000u);\n'  # real gap
)


def _build(tmp_path: Path, *, extra_c: str = "", other: list[dict[str, int]] | None = None,
           sites: str = SITES) -> list[CheckResult]:
    gen = tmp_path / "generated"
    gen.mkdir()
    (gen / "chunk_0.c").write_text(sites + extra_c)
    (gen / "sample.c").write_text('RECOMP_UNIMPL("cvtdq2pd xmm1, xmm1", 0x00009000u);\n')
    (gen / "lift_report.json").write_text(json.dumps({"functions_failed": 0}))
    switch = tmp_path / "switch_tables.json"
    switch.write_text(json.dumps({"switch": [{"table_addr": TABLE_VA, "count": 4}], "other": other or []}))
    stats = tmp_path / "disasm_stats.json"
    stats.write_text(json.dumps({"decode_failures_outside_tables": 0,
                                 "decode_failures_outside_tables_addrs": []}))
    return run_static_checks(gen, switch, stats, gen / "lift_report.json")


def _failed(results: list[CheckResult]) -> dict[str, CheckResult]:
    return {r.name: r for r in results if not r.ok}


def test_only_the_real_gap_is_listed(tmp_path: Path) -> None:
    failed = _failed(_build(tmp_path))
    assert list(failed) == ["unhandled instructions"]
    assert failed["unhandled instructions"].lines == ["  0x00003000 cvtdq2pd xmm0, xmm0"]


def _flag_read_failures(tmp_path: Path, body: str) -> dict[str, CheckResult]:
    clean = SITES.replace("cvtdq2pd xmm0, xmm0", "ud2")
    return _failed(_build(tmp_path, sites=clean, extra_c="void sub_00001000(void)\n{\n" + body + "}\n"))


def test_unset_flag_read_fails(tmp_path: Path) -> None:
    failed = _flag_read_failures(
        tmp_path, "    if (_flags /* jne: not equal / not zero */) goto loc_00001010;\n"
                  "    return;\nloc_00001010: ;\n    return;\n")
    assert list(failed) == ["unset-flag reads"]
    assert failed["unset-flag reads"].lines == ["  chunk_0.c:6"]


def test_unreachable_unset_flag_read_passes(tmp_path: Path) -> None:
    assert not _flag_read_failures(
        tmp_path, "    goto loc_00001010;\n"
                  "    if (_flags /* jne: not equal / not zero */) goto loc_00001020;\n"
                  "loc_00001010: ;\n    return;\nloc_00001020: ;\n    return;\n")


def test_unset_flag_read_without_effect_passes(tmp_path: Path) -> None:
    assert not _flag_read_failures(
        tmp_path, "    if (_flags /* jo: overflow */) (void)0; "
                  "/* goto loc_00009999 - dead code, label not in function */\n    return;\n")


def test_unexplained_indirect_jump_fails(tmp_path: Path) -> None:
    clean = SITES.replace("cvtdq2pd xmm0, xmm0", "ud2")
    failed = _failed(_build(tmp_path, sites=clean, other=[{"addr": 0x4000}]))
    assert list(failed) == ["unexplained indirect jumps"]
    assert failed["unexplained indirect jumps"].lines == ["  0x00004000"]


def test_clean_variant_passes_everything(tmp_path: Path) -> None:
    clean = SITES.replace("cvtdq2pd xmm0, xmm0", "hlt")
    assert not _failed(_build(tmp_path, sites=clean))


def test_real_hobbit_output_passes() -> None:
    report = refgame.GENERATED / "lift_report.json"
    if not report.is_file():
        pytest.skip("generated output not available")
    results = run_static_checks(refgame.GENERATED, refgame.RECON / "switch_tables.json",
                                refgame.RECON / "disasm_stats.json", report)
    assert not _failed(results)
