"""`jmp [IAT slot]` reaches the import the loader bound into the slot.

The PE loader writes host_func_va(thunk), an address in 0xFE000000.., into
each slot, and an indirect tail jmp resolves it through recomp_lookup_kernel
-- the same lookup chain `call [IAT]` uses.
"""

from __future__ import annotations

import json
import struct
import subprocess
from pathlib import Path

from tests.conformance.harness import (
    DATA_BASE, GPRS, Case, assemble, compile_native, initial_state, lift, native_source)

SLOT_OFF = 0x20
THUNK_VA = 0xFE000010

_HOST = r"""
static void import_thunk(void) {  /* stdcall, one argument */
    g_eax = 0x1000u + MEM32(g_esp + 4u);
    g_esp += 8u;
}
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_kernel(uint32_t va) {
    return va == %#xu ? import_thunk : NULL;
}
void recomp_icall_fail_log(uint32_t va) { fprintf(stderr, "unresolved 0x%%08X\n", va); }
""" % THUNK_VA


def test_jmp_iat_slot_tail_calls_the_bound_import(tmp_path: Path) -> None:
    case = Case("jmp_iat", f"jmp dword ptr [{DATA_BASE + SLOT_OFF:#x}]")
    c = lift(assemble(case), tmp_path)
    binary = tmp_path / "jmp_iat"
    compile_native(native_source(c, includes=_HOST), tmp_path, binary)

    state = initial_state(case, 1)
    data = bytearray(state.data)
    struct.pack_into("<I", data, SLOT_OFF, THUNK_VA)
    # The harness pushes a return address at esp - 4, so the argument is at esp.
    blob = tmp_path / "state.bin"
    blob.write_bytes(struct.pack("<8I", *(state.gpr[r] for r in GPRS))
                     + b"\0" * (16 * 8) + bytes(data))
    run = subprocess.run([str(binary), str(blob)], capture_output=True, text=True, check=True)
    out = json.loads(run.stdout)
    assert "unresolved" not in run.stderr
    # The import popped the return address and its one argument.
    assert out["esp_delta"] == 4
    assert out["eax"] == 0x1000  # argument slot is zeroed stack
