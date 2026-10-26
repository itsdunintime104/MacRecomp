"""find_functions on small hand-assembled i386 byte strings."""

from __future__ import annotations

import importlib.util
from pathlib import Path


_PATH = Path(__file__).resolve().parents[1] / "tools" / "pe" / "analyze_pe.py"
_SPEC = importlib.util.spec_from_file_location("analyze_pe", _PATH)
assert _SPEC is not None and _SPEC.loader is not None
A = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(A)

BASE = 0x1000


def spans(code: bytes, candidates: set[int],
          entries: set[int]) -> dict[int, int]:
    """{start: end} of the kept functions, with `entries` as listed starts."""
    found = A.find_functions(A.Code(code, BASE, BASE, BASE + len(code)),
                             candidates,
                             {va: A.Evidence.LISTED for va in entries},
                             frozenset(), frozenset())
    return {start: f.end for start, f in found.items()}


def test_loop_alignment_padding_merges_into_owner() -> None:
    code = bytes.fromhex(
        "8b442404"          # 1000 mov eax, [esp+4]
        "eb0a"              # 1004 jmp 1010
        "8da42400000000"    # 1006 lea esp, [esp]   (padding, false start)
        "8d4900"            # 100d lea ecx, [ecx]
        "48"                # 1010 dec eax          (loop head, false start)
        "75fd"              # 1011 jne 1010
        "c3")               # 1013 ret
    assert spans(code, {0x1000, 0x1006, 0x1010}, {0x1000}) == {0x1000: 0x1014}


def test_switch_label_is_not_kept() -> None:
    code = bytes.fromhex(
        "83f801"            # 1000 cmp eax, 1
        "7713"              # 1003 ja 1018
        "ff24850c100000"    # 1005 jmp [eax*4 + 100c]
        "14100000"          # 100c dd 1014
        "16100000"          # 1010 dd 1016
        "40c3"              # 1014 caseD_0: inc eax; ret
        "48c3"              # 1016 caseD_1: dec eax; ret
        "31c0c3")           # 1018 default: xor eax, eax; ret
    assert spans(code, {0x1000, 0x1014, 0x1016}, {0x1000}) == {0x1000: 0x101b}


def test_call_target_is_kept() -> None:
    code = bytes.fromhex(
        "e803000000"        # 1000 call 1008
        "c3"                # 1005 ret
        "cccc"              # 1006 int3 fill
        "31c0c3")           # 1008 xor eax, eax; ret (not a candidate)
    assert spans(code, {0x1000}, {0x1000}) == {0x1000: 0x1006,
                                                0x1008: 0x100b}


def test_backward_jump_is_inside_owner_extent() -> None:
    code = bytes.fromhex(
        "31c0"              # 1000 xor eax, eax
        "40"                # 1002 inc eax          (loop head)
        "83f805"            # 1003 cmp eax, 5
        "7403"              # 1006 je 100b
        "90"                # 1008 nop              (false start)
        "ebf7"              # 1009 jmp 1002         (backward)
        "c3")               # 100b ret
    assert spans(code, {0x1000, 0x1008}, {0x1000}) == {0x1000: 0x100c}

