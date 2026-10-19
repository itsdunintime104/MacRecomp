"""Checks on the Capstone sweep (S2) and jump classification (S3)."""

from __future__ import annotations

import struct
from collections import defaultdict
from typing import Any

from tools.hobbit import disasm

EXPECTED_INSTRUCTIONS = 4514750
INDIRECT_JMP_COUNT = 2166

Sweep = tuple[dict[str, Any], dict[str, Any]]


def test_instruction_count_within_one_percent(sweep: Sweep) -> None:
    stats, _ = sweep
    total = stats["total_instructions"]
    assert abs(total - EXPECTED_INSTRUCTIONS) <= EXPECTED_INSTRUCTIONS * 0.01
    assert stats["decode_failure_count"] == len(stats["decode_failures"])
    assert sum(stats["mnemonics"].values()) == total


def test_every_indirect_jmp_is_classified(sweep: Sweep) -> None:
    stats, switch = sweep
    classified = (
        len(switch["switch"])
        + len(switch["tailcall"])
        + len(switch["other"])
        + len(switch["explained"])
    )
    assert stats["indirect_jmp"] == INDIRECT_JMP_COUNT
    assert classified == INDIRECT_JMP_COUNT == 2166
    assert len(switch["other"]) == 0
    assert stats["decode_failures_outside_tables"] == 0
    jmp_addrs = (
        [s["jmp_addr"] for s in switch["switch"]]
        + switch["tailcall"]
        + [o["addr"] for o in switch["other"]]
        + [e["addr"] for e in switch["explained"]]
    )
    assert len(set(jmp_addrs)) == INDIRECT_JMP_COUNT
    for entry in switch["explained"]:
        assert entry["reason"] and entry["context"]


def test_switch_targets_inside_function_and_decoded(
    analysis: dict[str, Any], binary_data: bytes, sweep: Sweep
) -> None:
    _, switch = sweep
    sizes = {f["addr"]: f["size"] for f in analysis["functions"]}
    by_func: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for table in switch["switch"]:
        by_func[table["func_addr"]].append(table)
        assert table["count"] == len(table["targets"]) > 0
        assert table["kind"] in ("pic", "abs")
    for func_addr, tables in by_func.items():
        end = func_addr + sizes[func_addr]
        starts = disasm.instruction_starts(analysis, binary_data, func_addr, tables)
        for table in tables:
            for target in table["targets"]:
                assert func_addr <= target < end, hex(table["jmp_addr"])
                assert target in starts, (hex(table["jmp_addr"]), hex(target))


def test_switch_tables_inside_text_or_const(analysis: dict[str, Any], sweep: Sweep) -> None:
    _, switch = sweep
    allowed = [
        (sec["addr"], sec["addr"] + sec["size"])
        for seg in analysis["segments"]
        for sec in seg["sections"]
        if sec["name"] in ("__text", "__const", "__data")
    ]
    for table in switch["switch"]:
        start = table["table_addr"]
        end = start + 4 * table["count"]
        assert any(lo <= start and end <= hi for lo, hi in allowed), hex(table["jmp_addr"])
        if table["kind"] == "pic":
            assert table["pic_base"] != 0


def test_report_zero_goal_counts(sweep: Sweep, capsys: Any) -> None:
    stats, switch = sweep
    with capsys.disabled():
        print(
            f"\nswitch={len(switch['switch'])} tailcall={len(switch['tailcall'])} "
            f"other={len(switch['other'])} explained={len(switch['explained'])} "
            f"decode_failures_outside_tables={stats['decode_failures_outside_tables']}"
        )


# Synthetic PIC switches: the base comes from `call +0; pop ebx` at 0x1000.
FUNC = (0x1000, 0x1200)
BASE = 0x1005
TABLE = 0x1100
ENTRIES = [0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90]


def _resolve(lines: list[tuple[str, str]], count: int) -> dict[str, Any] | None:
    insns = [disasm.Insn(0x1000, 5, "call", "0x1005"), disasm.Insn(0x1005, 1, "pop", "ebx")]
    insns += [disasm.Insn(0x1006 + 3 * n, 3, mnem, ops) for n, (mnem, ops) in enumerate(lines)]
    data = bytearray(0x200)
    for k, entry in enumerate(ENTRIES[:count]):
        data[TABLE - 0x1000 + 4 * k : TABLE - 0x1000 + 4 * k + 4] = struct.pack("<i", entry)
    amap = disasm.AddressMap(
        {"segments": [{"vmaddr": 0x1000, "fileoff": 0, "filesize": 0x200, "sections": []}]}
    )
    states = disasm._pic_bases(insns)
    return disasm._match_switch(insns, len(insns) - 1, states, {}, FUNC, amap, bytes(data))


def test_pic_switch_with_spilled_base_slot() -> None:
    switch = _resolve(
        [
            ("mov", "dword ptr [ebp - 8], ebx"),
            ("cmp", "eax, 3"),
            ("ja", "0x1180"),
            ("mov", "ecx, dword ptr [ebp - 8]"),
            ("mov", f"edx, dword ptr [ecx + eax*4 + {TABLE - BASE:#x}]"),
            ("add", "edx, dword ptr [ebp - 8]"),
            ("jmp", "edx"),
        ],
        4,
    )
    assert switch is not None
    assert (switch["pic_base"], switch["table_addr"], switch["count"]) == (BASE, TABLE, 4)
    assert switch["targets"] == [BASE + e for e in ENTRIES[:4]]


def test_pic_switch_with_shifted_index_and_lea_table() -> None:
    switch = _resolve(
        [
            ("cmp", "dword ptr [ebp - 0x1c], 8"),
            ("ja", "0x1180"),
            ("mov", "edx, dword ptr [ebp - 0x1c]"),
            ("shl", "edx, 2"),
            ("lea", f"eax, [ebx + {TABLE - BASE:#x}]"),
            ("mov", "eax, dword ptr [edx + eax]"),
            ("add", "eax, ebx"),
            ("jmp", "eax"),
        ],
        9,
    )
    assert switch is not None
    assert (switch["pic_base"], switch["table_addr"], switch["count"]) == (BASE, TABLE, 9)
    assert switch["targets"] == [BASE + e for e in ENTRIES]

