"""Lift a PE32 image to C with the engine's lifter, without changing it.

Usage:
  python -I run_lift_pe.py --engine <MacRecomp engine root> --analysis <json> \
      --switch <json> --bin <exe> --out <dir> --prefix <lift prefix> [--jobs N] ...

The engine's tools/xrecomp/run.py reads the entry point by parsing the image
as a Mach-O. Everything else in the lift path is format-agnostic: the
translator only needs a virtual-address-to-file-offset map, which comes from
analysis.json. So this driver supplies the entry point from analysis.json and
hands the rest to the engine unchanged. When the PE front end moves into the
engine, this becomes configure_from_pe() plus an --format flag and this file
goes away.

It also names the MSVC frame helpers (__SEH_prolog4, __EH_prolog3*, and their
epilogs) for the engine's --seh-prolog/--seh-epilog, read by name from
names.json next to analysis.json. They build or tear down the caller's frame,
so the lifter must hand ebp across each call through g_seh_ebp; the engine's
byte-pattern detection only knows the older Xbox-era helpers.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path


# __EH_epilog3_GS has no FID name in names.json. It is
# `mov ecx,[ebp-0x10]; xor ecx,ebp; call __security_check_cookie;
# jmp __EH_epilog3`, found by those bytes in the executable sections.
EPILOG3_GS = re.compile(rb"\x8b\x4d\xf0\x33\xcd\xe8(.{4})\xe9(.{4})", re.S)
COOKIE_CHECK = {"__security_check_cookie", "@__security_check_cookie@4"}
PE_SCN_MEM_EXECUTE = 0x20000000


def epilog3_gs(image: bytes, names: dict[int, str]) -> list[int]:
    """VAs of `__EH_epilog3_GS`: each EPILOG3_GS match in an executable
    section whose call goes to the cookie check and whose jmp goes to
    __EH_epilog3, both by name."""
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    count, = struct.unpack_from("<H", image, pe + 6)
    opt_size, = struct.unpack_from("<H", image, pe + 20)
    image_base, = struct.unpack_from("<I", image, pe + 24 + 28)
    found = []
    for i in range(count):
        sh = pe + 24 + opt_size + 40 * i
        vsize, rva, rsize, rptr = struct.unpack_from("<IIII", image, sh + 8)
        flags, = struct.unpack_from("<I", image, sh + 36)
        if not flags & PE_SCN_MEM_EXECUTE:
            continue
        start = image_base + rva
        for m in EPILOG3_GS.finditer(image, rptr, rptr + min(vsize, rsize)):
            va = start + m.start() - rptr
            call = va + 10 + struct.unpack("<i", m[1])[0]
            jmp = va + 15 + struct.unpack("<i", m[2])[0]
            if names.get(call) in COOKIE_CHECK and names.get(jmp) == "__EH_epilog3":
                found.append(va)
    return found


def frame_helpers(names_path: Path, image: bytes) -> tuple[list[int], list[int]]:
    """(prolog VAs, epilog VAs) of the MSVC frame helpers, by name, plus
    __EH_epilog3_GS by its bytes."""
    names = {int(va, 16): rec["name"] for va, rec in json.loads(names_path.read_text()).items()}
    prologs, epilogs = [], []
    for va, name in names.items():
        if name == "__SEH_prolog4" or name.startswith("__EH_prolog3"):
            prologs.append(va)
        elif name == "__SEH_epilog4" or name.startswith("__EH_epilog3"):
            epilogs.append(va)
    return sorted(prologs), sorted({*epilogs, *epilog3_gs(image, names)})


class _ImageEntry:
    """Stands in for the Mach-O header object, which only .entry is read from."""

    def __init__(self, entry: int) -> None:
        self.entry = entry


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--engine", type=Path, required=True)
    ap.add_argument("--analysis", type=Path, required=True)
    ap.add_argument("--bin", type=Path, required=True)
    known, passthrough = ap.parse_known_args()

    sys.path.insert(0, str(known.engine.resolve()))
    from tools.hobbit import macho  # pyright: ignore[reportMissingImports]  (engine path is --engine)
    from tools.xrecomp import run as xrun  # pyright: ignore[reportMissingImports]

    entry = json.loads(known.analysis.read_text())["binary"]["entry"]
    macho.parse = lambda data: _ImageEntry(entry)  # type: ignore[assignment]

    prologs, epilogs = frame_helpers(known.analysis.parent / "names.json", known.bin.read_bytes())
    seh = ["--seh-prolog", ",".join(f"0x{a:X}" for a in prologs),
           "--seh-epilog", ",".join(f"0x{a:X}" for a in epilogs)]
    # The CRT x87 math entries read ZF from their callers (notes/lift_review.md,
    # group A); entry_zf.json next to analysis.json holds that contract for
    # the lifter, when the game has one.
    entry_zf = known.analysis.parent / "entry_zf.json"
    if "--entry-zf" not in passthrough and entry_zf.exists():
        seh += ["--entry-zf", str(entry_zf)]
    sys.argv = [sys.argv[0], "--analysis", str(known.analysis), "--bin", str(known.bin), *seh,
                *passthrough]
    return xrun.main()


if __name__ == "__main__":
    raise SystemExit(main())
