"""A byte-map switch with one arm becomes a goto, not a tail call.

`cmp ecx, 6; ja; movzx ecx, byte [ecx + MAP]; jmp [ecx*4 + TABLE]` with a
one-entry table and a map of zeros: LEGO Harry Potter 0x0096D7E4. Unicorn is
the oracle. The switch keeps its RECOMP_ITAIL fallback, so the dispatch
lookups are stubbed to fail loudly.
"""

from __future__ import annotations

from pathlib import Path

from tests.conformance.harness import (
    Case, assemble, compare, compile_native, initial_state, lift, native_source,
    run_native, run_unicorn)

# Layout: arm at 0x10013, table at 0x1001C, map (7 zero bytes) at 0x10020.
ASM = ("cmp ecx, 6\nja out\nmovzx ecx, byte ptr [ecx + 0x10020]\n"
       "jmp dword ptr [ecx*4 + 0x1001C]\nmov edx, 7\nout:\nret\n"
       ".byte 0xCC, 0xCC, 0xCC\n.long 0x10013\n.byte 0, 0, 0, 0, 0, 0, 0")

_STUBS = r"""
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va; return NULL; }
void recomp_icall_fail_log(uint32_t va) { fprintf(stderr, "tail call 0x%08X\n", va); abort(); }
"""


def test_one_arm_byte_map_switch_is_a_goto(tmp_path: Path) -> None:
    case = Case("byte_map_one_arm", ASM, regs={"ecx": 3, "edx": 0})
    code = assemble(case)
    c = lift(code, tmp_path)
    assert "goto loc_00010013" in c
    binary = tmp_path / "byte_map"
    # The native side maps only the data page; the image's table goes in by
    # hand (the map is zero, as calloc leaves it).
    setup = "    *(uint32_t *)(host + 0x1001C) = 0x10013u;"
    compile_native(native_source(c, includes=_STUBS, setup=setup), tmp_path, binary)
    state = initial_state(case, 1)
    assert compare(run_unicorn(code, state), run_native(binary, state, tmp_path)) == []


def test_map_byte_past_the_table_keeps_the_tail_call(tmp_path: Path) -> None:
    asm = ASM.replace(".byte 0, 0, 0, 0, 0, 0, 0", ".byte 0, 0, 2, 0, 0, 0, 0")
    c = lift(assemble(Case("byte_map_past_table", asm)), tmp_path)
    assert "goto loc_00010013" not in c
