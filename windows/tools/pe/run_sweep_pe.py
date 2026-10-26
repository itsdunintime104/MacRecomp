"""Run the engine's capstone sweep over a PE32 image.

Usage: python run_sweep_pe.py --engine <HobbitRecomp/engine> \
           --analysis <json> --bin <exe> -o <recon dir>

Same sweep as tools.hobbit.disasm, with one PE difference: MSVC emits switch
jump tables inside the code section, so the absolute-table finder has to accept
a table address in __TEXT,__text. In a Mach-O those tables only ever sit in
__DATA,__const, __DATA,__data or __TEXT,__const.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--engine", type=Path, required=True)
    ap.add_argument("--analysis", type=Path, required=True)
    ap.add_argument("--bin", type=Path, required=True)
    ap.add_argument("-o", "--output-dir", type=Path, required=True)
    args = ap.parse_args()

    sys.path.insert(0, str(args.engine.resolve()))
    from tools.hobbit import disasm  # pyright: ignore[reportMissingImports]  (engine path is --engine)

    disasm.ABS_TABLE_SECTIONS = frozenset(
        disasm.ABS_TABLE_SECTIONS | {"__TEXT,__text"})

    analysis = json.loads(args.analysis.read_text())
    stats, switch = disasm.sweep(analysis, args.bin.read_bytes())
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "disasm_stats.json").write_text(
        json.dumps(stats, indent=1) + "\n")
    (args.output_dir / "switch_tables.json").write_text(
        json.dumps(switch, indent=1) + "\n")
    print(f'switches {len(switch["switch"])}  tailcalls {len(switch["tailcall"])}')


if __name__ == "__main__":
    main()
