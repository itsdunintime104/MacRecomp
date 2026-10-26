"""Rewrite guest addresses in a log to function names.

Usage: python3 -I symbolize.py [RECON_DIR] < crash.log   (RECON_DIR default: recon)

Every `0x` followed by 6 to 8 hex digits, and every `sub_XXXXXXXX`, that falls inside a
function named in RECON_DIR/names.json becomes `<name>+0x<offset>`. The function bodies come
from RECON_DIR/ghidra_functions.json. Other text and unnamed addresses pass through unchanged.
"""

from __future__ import annotations

import bisect
import json
import re
import sys
from pathlib import Path

ADDRESS_RE = re.compile(r"\bsub_([0-9A-Fa-f]{8})\b|\b0x([0-9A-Fa-f]{6,8})\b")


class Symbolizer:
    def __init__(self, names: dict[str, dict[str, str]], functions: list[dict[str, object]]) -> None:
        by_start = {int(addr, 16): entry["name"] for addr, entry in names.items()}
        spans: list[tuple[int, int, int, str]] = []  # (range start, range end exclusive, entry, name)
        for func in functions:
            entry = func["addr"]
            ranges = func["ranges"]
            assert isinstance(entry, int) and isinstance(ranges, list)
            if entry not in by_start:
                continue
            for lo, hi in ranges:
                spans.append((lo, hi + 1, entry, by_start[entry]))
        spans.sort()
        self.spans = spans
        self.starts = [span[0] for span in spans]

    def lookup(self, addr: int) -> str | None:
        i = bisect.bisect_right(self.starts, addr) - 1
        if i < 0:
            return None
        lo, hi, entry, name = self.spans[i]
        return f"{name}+0x{addr - entry:X}" if lo <= addr < hi else None

    def rewrite(self, line: str) -> str:
        def replace(match: re.Match[str]) -> str:
            found = self.lookup(int(match[1] or match[2], 16))
            return found if found else match[0]

        return ADDRESS_RE.sub(replace, line)


def main() -> None:
    recon = Path(sys.argv[1] if len(sys.argv) > 1 else "recon")
    symbolizer = Symbolizer(json.loads((recon / "names.json").read_text()),
                            json.loads((recon / "ghidra_functions.json").read_text()))
    for line in sys.stdin:
        sys.stdout.write(symbolizer.rewrite(line))


if __name__ == "__main__":
    main()
