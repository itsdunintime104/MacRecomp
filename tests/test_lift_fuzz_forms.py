"""Random differential checks of single instruction forms: Unicorn against lifted C.

Each form is one snippet, lifted and compiled once, then run on CASES random
states in one process. The double lanes are drawn from the cases that differ
between x86 and arm64 hardware -- NaNs of both kinds and signs, infinities,
signed zeros, denormals, overflow and underflow -- as well as ordinary values.
All eight GPRs (esp as a delta), all eight xmm registers and the data window
are compared.

Unicorn is the oracle except in one place it is wrong. For a two-source
double operation whose two sources are both NaN, Unicorn 2.1 (QEMU's x87
rule) returns the NaN with the larger significand; the SSE unit returns the
first source, quieted (Intel SDM vol. 1, table 4-7). Those lanes are checked
against the SDM rule instead.

cpuid has no oracle here: the guest is meant to see a fixed CPU, not
Unicorn's, so it is checked against that profile.
"""

from __future__ import annotations

import hashlib
import random
import struct
import subprocess
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

import pytest
from unicorn import UC_ARCH_X86, UC_MODE_32
from unicorn.unicorn import Uc
from unicorn.x86_const import UC_X86_REG_EFLAGS

from tests.conformance.harness import (
    _UC_GPRS, _UC_XMM, CODE_BASE, CODE_PAGE, DATA_BASE, GPRS, INCLUDE_DIR, ROOT, SENTINEL,
    STACK_PAGES, STACK_TOP, WORK_DIR, Case, _case_dir, assemble, lift,
)

CASES = 1000
WINDOW = 0x100
D = f"0x{DATA_BASE:X}"
RECORD = struct.Struct(f"<8I32I{WINDOW}s")
MIN_MEM = DATA_BASE + 0x1000

QUIET = 1 << 51
EXP = 0x7FF << 52
SIGN = 1 << 63
SPECIAL = (
    0, SIGN, EXP, EXP | SIGN,                                     # zeros, infinities
    EXP | QUIET, EXP | QUIET | SIGN, EXP | QUIET | 0x1234,        # quiet NaNs
    EXP | 1, EXP | SIGN | 0x8000, EXP | (QUIET >> 1),             # signalling NaNs
    1, SIGN | 1, 0x000FFFFFFFFFFFFF, 0x0008000000000000,          # denormals
    0x0010000000000000, 0x7FEFFFFFFFFFFFFF, SIGN | 0x7FEFFFFFFFFFFFFF,
    0x3FF0000000000000, 0xBFF0000000000000,
    0x7E37E43C8800759C, 0x01A56E1FC2F8F359,                       # 1e300, 1e-300
)


def _is_nan(q: int) -> bool:
    return (q & ~SIGN) > EXP


def double_bits(rng: random.Random) -> int:
    pick = rng.random()
    if pick < 0.35:
        return rng.choice(SPECIAL)
    sign = rng.getrandbits(1) << 63
    if pick < 0.45:
        return sign | (rng.getrandbits(52) or 1)                   # denormal
    if pick < 0.55:
        return sign | EXP | (rng.getrandbits(52) or 1)             # either NaN kind
    if pick < 0.85:
        return sign | ((1023 + rng.randint(-60, 60)) << 52) | rng.getrandbits(52)
    return rng.getrandbits(64)


def int_bits(rng: random.Random) -> int:
    return rng.getrandbits(64)


def gpr_value(rng: random.Random) -> int:
    pick = rng.random()
    if pick < 0.3:
        return rng.choice((0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x80, 0xFF))
    return rng.getrandbits(32)


@dataclass(frozen=True)
class Form:
    name: str
    asm: str
    lanes: Callable[[random.Random], int] = int_bits
    regs: Callable[[random.Random], dict[str, int]] | None = None
    # Double lanes the SDM NaN rule decides: (dst xmm, src xmm or None for
    # the data window at offset 0), per 64-bit lane.
    nan_rule: tuple[int, int | None] | None = None
    lanes_checked: int = 2


@dataclass(frozen=True)
class State:
    gpr: tuple[int, ...]
    xmm: tuple[tuple[int, int], ...]
    data: bytes


def make_state(form: Form, rng: random.Random) -> State:
    gpr = {r: gpr_value(rng) for r in GPRS}
    if form.regs:
        gpr.update(form.regs(rng))
    gpr["esp"] = STACK_TOP
    xmm = tuple((form.lanes(rng), form.lanes(rng)) for _ in range(8))
    data = b"".join(struct.pack("<Q", form.lanes(rng)) for _ in range(WINDOW // 8))
    return State(tuple(gpr[r] for r in GPRS), xmm, data)


def _xmm_words(state: State) -> list[int]:
    out: list[int] = []
    for lo, hi in state.xmm:
        out += [lo & 0xFFFFFFFF, lo >> 32, hi & 0xFFFFFFFF, hi >> 32]
    return out


def pack_state(state: State) -> bytes:
    return RECORD.pack(*state.gpr, *_xmm_words(state), state.data)


class Oracle:
    """One Unicorn instance per form, reset for every state."""

    def __init__(self, code: bytes) -> None:
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.uc.mem_map(CODE_BASE, CODE_PAGE)
        self.uc.mem_write(CODE_BASE, code)
        self.uc.mem_write(SENTINEL, b"\x90")
        self.uc.mem_map(STACK_TOP + 0x100 - STACK_PAGES, STACK_PAGES)
        self.uc.mem_map(DATA_BASE, 0x1000)

    def run(self, state: State) -> bytes:
        uc = self.uc
        uc.mem_write(DATA_BASE, state.data)
        for reg, value in zip(_UC_GPRS, state.gpr):
            uc.reg_write(reg, value)
        for reg, (lo, hi) in zip(_UC_XMM, state.xmm):
            uc.reg_write(reg, lo | (hi << 64))
        # IF set, as the lifted pushfd reports it; DF clear, as the ABI has it.
        uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
        esp = state.gpr[4] - 4
        uc.mem_write(esp, struct.pack("<I", SENTINEL))
        uc.reg_write(_UC_GPRS[4], esp)
        uc.emu_start(CODE_BASE, SENTINEL)
        gpr = [int(uc.reg_read(r)) for r in _UC_GPRS]
        words: list[int] = []
        for r in _UC_XMM:
            v = int(uc.reg_read(r))
            words += [(v >> (32 * i)) & 0xFFFFFFFF for i in range(4)]
        return RECORD.pack(*gpr, *words, bytes(uc.mem_read(DATA_BASE, WINDOW)))


_MAIN = r"""
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#ifndef WITH_RUNTIME
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS RecompThreadState g_thread_state = {.ts_mxcsr = 0x1F80};
void recomp_divide_error(uint32_t va) { fprintf(stderr, "divide error 0x%%08X\n", va); exit(4); }
void recomp_unimpl(const char *text, uint32_t va) {
    fprintf(stderr, "unimplemented at 0x%%08X: %%s\n", va, text); exit(3);
}
#else
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
#endif

%(lifted)s

int main(int argc, char **argv) {
    (void)argc;
    RecompXmm *xr[8] = {&g_xmm0, &g_xmm1, &g_xmm2, &g_xmm3, &g_xmm4, &g_xmm5, &g_xmm6, &g_xmm7};
    uint8_t *host = calloc(1, %(mem)s);
    g_xbox_mem_offset = (ptrdiff_t)host;
    FILE *f = fopen(argv[1], "rb");
    uint32_t gpr[8], xmm[32];
    while (fread(gpr, 4, 8, f) == 8) {
        if (fread(xmm, 4, 32, f) != 32 || fread(host + %(data)s, 1, %(window)s, f) != %(window)s)
            return 2;
        g_eax = gpr[0]; g_ecx = gpr[1]; g_edx = gpr[2]; g_ebx = gpr[3];
        g_ebp = g_seh_ebp = gpr[5]; g_esi = gpr[6]; g_edi = gpr[7];
        for (int i = 0; i < 8; i++) memcpy(xr[i]->u, xmm + 4 * i, 16);
        g_df = 0; g_eflags_id = 0; g_mxcsr = 0x1F80;
        g_esp = gpr[4] - 4;
        MEM32(g_esp) = %(sentinel)s;
        sub_%(code)s();
        uint32_t out[8] = {g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi};
        fwrite(out, 4, 8, stdout);
        for (int i = 0; i < 8; i++) fwrite(xr[i]->u, 4, 4, stdout);
        fwrite(host + %(data)s, 1, %(window)s, stdout);
    }
    return 0;
}
"""

RUNTIME_SOURCES = (
    "runtime/src/diag.c", "runtime/src/thunk_files.c", "runtime/src/thunk_threads.c",
    "runtime/src/handles.c", "runtime/src/guest_mem.c", "runtime/src/macho_load.c",
    "runtime/src/recomp_glue.c", "runtime/src/host_funcs.c",
)


def build(form: Form, *, with_runtime: bool = False) -> tuple[bytes, Path]:
    """Assemble and lift the form; compile it with the game build's float flags."""
    case = Case(form.name, form.asm)
    code = assemble(case)
    work = _case_dir(f"fuzz-{form.name}")
    source = _MAIN % {
        "lifted": lift(code, work), "mem": hex(MIN_MEM), "data": hex(DATA_BASE),
        "window": hex(WINDOW), "sentinel": hex(SENTINEL), "code": f"{CODE_BASE:08X}"}
    extra = [str(ROOT / s) for s in RUNTIME_SOURCES] if with_runtime else []
    # The helpers live in the headers and runtime, not in the lifted text.
    inputs = [source, *(Path(x).read_text() for x in extra),
              *(h.read_text() for h in sorted(INCLUDE_DIR.glob("*.h")))]
    digest = hashlib.sha256("\0".join(inputs).encode()).hexdigest()[:16]
    binary = WORK_DIR / f"fuzz-{form.name}-{digest}"
    if not binary.exists():
        c_file = work / "fuzz.c"
        c_file.write_text(source)
        tmp = binary.with_suffix(".tmp")
        cmd = ["clang", "-std=gnu11", "-O1", "-fwrapv", "-fno-strict-aliasing",
               "-ffp-contract=off", "-arch", "arm64", "-w", "-I", str(INCLUDE_DIR),
               "-I", str(ROOT / "runtime" / "include")]
        if with_runtime:
            cmd += ["-DWITH_RUNTIME", "-DRECOMP_GLUE_NO_MAIN",
                    "-I", str(ROOT / "runtime" / "tests" / "include")]
        subprocess.run([*cmd, str(c_file), *extra, "-o", str(tmp), "-lm"], check=True,
                       capture_output=True)
        tmp.rename(binary)
    return code, binary


def run_native(binary: Path, states: list[State], work: Path) -> list[bytes]:
    blob = work / "states.bin"
    blob.write_bytes(b"".join(pack_state(s) for s in states))
    proc = subprocess.run([str(binary), str(blob)], capture_output=True, timeout=120)
    assert proc.returncode == 0, proc.stderr.decode()[-2000:]
    out = proc.stdout
    assert len(out) == RECORD.size * len(states)
    return [out[i * RECORD.size:(i + 1) * RECORD.size] for i in range(len(states))]


def sdm_nan_fix(form: Form, state: State, expected: bytes) -> bytes:
    """Replace the oracle's lanes where both sources are NaN with the SDM result."""
    if form.nan_rule is None:
        return expected
    dst, src = form.nan_rule
    fields = list(RECORD.unpack(expected))
    for lane in range(form.lanes_checked):
        a = state.xmm[dst][lane]
        b = (state.xmm[src][lane] if src is not None
             else struct.unpack_from("<Q", state.data, 8 * lane)[0])
        if _is_nan(a) and _is_nan(b):
            q = a | QUIET
            base = 8 + 4 * dst + 2 * lane
            fields[base], fields[base + 1] = q & 0xFFFFFFFF, q >> 32
    return RECORD.pack(*fields)


def describe(want: bytes, got: bytes) -> str:
    w, g = RECORD.unpack(want), RECORD.unpack(got)
    names = [*GPRS, *(f"xmm{i // 4}.u[{i % 4}]" for i in range(32)), "data"]
    return ", ".join(f"{n}: want {a!r} got {b!r}" for n, a, b in zip(names, w, g) if a != b)


def check(form: Form) -> int:
    code, binary = build(form)
    rng = random.Random(form.name)
    states = [make_state(form, rng) for _ in range(CASES)]
    oracle = Oracle(code)
    got = run_native(binary, states, _case_dir(f"fuzz-{form.name}"))
    bad = []
    for state, actual in zip(states, got):
        want = sdm_nan_fix(form, state, oracle.run(state))
        if want != actual:
            bad.append(describe(want, actual))
    assert not bad, f"{len(bad)}/{CASES} differ; first: {bad[0]}"
    return len(states)


_CMP = ("eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord")
_PUSHFD_SETTERS = ("sub eax, ecx", "add eax, ecx", "xor eax, ecx", "and eax, ecx",
                   "or eax, ecx", "cmp eax, ecx", "sub eax, 1", "xor eax, eax")


def _same_or_random(rng: random.Random) -> dict[str, int]:
    """eax == ecx a quarter of the time, so ZF and the borrow edges come up."""
    return {"ecx": gpr_value(rng)} if rng.random() < 0.75 else {"ecx": 0x5A5A5A5A,
                                                               "eax": 0x5A5A5A5A}


def _popfd_image(rng: random.Random) -> dict[str, int]:
    # The bits popfd loads that the lift models: CF PF AF ZF SF DF OF ID; IF set.
    return {"ecx": (rng.getrandbits(32) & 0x00200CD5) | 0x202}


FORMS: list[Form] = [
    Form("mulpd", "mulpd xmm0, xmm1\nmulpd xmm2, xmm2\nret", double_bits, nan_rule=(0, 1)),
    Form("mulpd_mem", f"mulpd xmm3, xmmword ptr [{D}]\nret", double_bits, nan_rule=(3, None)),
    Form("subpd", "subpd xmm0, xmm1\nret", double_bits, nan_rule=(0, 1)),
    Form("subpd_mem", f"subpd xmm4, xmmword ptr [{D}]\nret", double_bits, nan_rule=(4, None)),
    Form("subpd_self", "subpd xmm5, xmm5\nret", double_bits),
    *(Form(f"cmp{p}pd", f"cmp{p}pd xmm0, xmm1\nret", double_bits) for p in _CMP),
    *(Form(f"cmp{p}pd_mem", f"cmp{p}pd xmm3, xmmword ptr [{D}]\nret", double_bits)
      for p in ("lt", "nle")),
    *(Form(f"cmp{p}sd", f"cmp{p}sd xmm0, xmm1\nret", double_bits) for p in _CMP),
    *(Form(f"cmp{p}sd_mem", f"cmp{p}sd xmm2, qword ptr [{D}+8]\nret", double_bits)
      for p in ("eq", "lt")),
    *(Form(m, f"{m} xmm0, xmm1\n{m} xmm2, xmmword ptr [{D}+0x10]\nret")
      for m in ("andpd", "orpd", "andnpd")),
    Form("unpcklpd", f"unpcklpd xmm0, xmm1\nunpcklpd xmm2, xmm2\n"
                     f"unpcklpd xmm3, xmmword ptr [{D}]\nret"),
    Form("unpckhpd", f"unpckhpd xmm0, xmm1\nunpckhpd xmm6, xmm6\n"
                     f"unpckhpd xmm3, xmmword ptr [{D}]\nret"),
    *(Form(f"shufpd_{imm}", f"shufpd xmm0, xmm1, {imm}\nshufpd xmm3, xmm3, {imm}\n"
                            f"shufpd xmm4, xmmword ptr [{D}], {imm}\nret") for imm in range(4)),
    Form("pmovmskb", "pmovmskb eax, xmm2\npmovmskb ecx, xmm1\nret"),
    Form("pmaxsw", f"pmaxsw xmm1, xmm3\npmaxsw xmm2, xmmword ptr [{D}]\nret"),
    *(Form(f"pushfd_{s.split()[0]}_{i}", f"{s}\npushfd\npop edx\nand edx, 0xffffffef\nret",
           regs=_same_or_random) for i, s in enumerate(_PUSHFD_SETTERS)),
    Form("popfd", "push ecx\npopfd\nsetb al\nsetbe ah\nsete bl\nsets bh\nsetp dl\nseto dh\n"
                  "setl cl\nsetle ch\npushfd\npop esi\npush 0x202\npopfd\nret", regs=_popfd_image),
    # The MSVC CRT's "is cpuid there" test, as LEGO Harry Potter has it at 0x0093CB90.
    Form("cpuid_probe", "push ebp\nmov ebp, esp\nsub esp, 8\npush ebx\npushfd\npop eax\n"
                        "mov ecx, eax\nxor eax, 0x200000\npush eax\npopfd\npushfd\npop eax\n"
                        "xor eax, ecx\npush ecx\npopfd\nand ecx, 0xffffffef\npop ebx\n"
                        "mov esp, ebp\npop ebp\nret"),
    Form("repz_ret", "test eax, eax\njz two\nmov ecx, 1\nrepz ret\ntwo:\nmov ecx, 2\nret"),
]


@pytest.mark.parametrize("form", FORMS, ids=[f.name for f in FORMS])
def test_form_matches_unicorn(form: Form) -> None:
    assert check(form) == CASES


# Leaf -> (EAX, EBX, ECX, EDX) the guest must see: a Core 2 Duo E8400.
BRAND = b"Intel(R) Core(TM)2 Duo CPU     E8400  @ 3.00GHz".ljust(48, b"\0")
PROFILE = {
    0: (1, 0x756E6547, 0x6C65746E, 0x49656E69),
    1: (0x0001067A, 0x00010800, 0, 0x07808111),
    0x80000000: (0x80000004, 0, 0, 0),
    **{0x80000002 + i: struct.unpack("<4I", BRAND[16 * i:16 * i + 16]) for i in range(3)},
}


def test_cpuid_profile() -> None:
    form = Form("cpuid", "cpuid\nret")
    _, binary = build(form, with_runtime=True)
    leaves = [*PROFILE, 2, 4, 0xA, 0x80000001, 0x80000005, 0x40000000, 0xFFFFFFFF]
    rng = random.Random("cpuid")
    states = []
    for leaf in leaves:
        state = make_state(form, rng)
        states.append(State((leaf, *state.gpr[1:]), state.xmm, state.data))
    got = run_native(binary, states, _case_dir("fuzz-cpuid"))
    for leaf, raw in zip(leaves, got):
        eax, ecx, edx, ebx = RECORD.unpack(raw)[:4]
        assert (eax, ebx, ecx, edx) == PROFILE.get(leaf, (0, 0, 0, 0)), hex(leaf)
    assert len(BRAND.rstrip(b"\0")) == 47
