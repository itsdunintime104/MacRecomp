"""exe_icon on a small synthetic PE32 image: one RT_GROUP_ICON with one PNG RT_ICON."""

from __future__ import annotations

import importlib.util
import struct
import sys
import zlib
from pathlib import Path

_PATH = Path(__file__).resolve().parent / "exe_icon.py"
_SPEC = importlib.util.spec_from_file_location("exe_icon", _PATH)
assert _SPEC is not None and _SPEC.loader is not None
E = importlib.util.module_from_spec(_SPEC)
sys.modules["exe_icon"] = E  # dataclass looks up its module here
_SPEC.loader.exec_module(E)

RSRC_RVA = 0x1000
RSRC_FILE = 0x200


def png(size: int) -> bytes:
    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    rows = b"".join(b"\0" + b"\xff\x00\x00\xff" * size for _ in range(size))
    return (E.PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def resource_tree(icon: bytes, group: bytes) -> bytes:
    """Root -> type (3, 14) -> name 1 -> language 0x409 -> data entry."""
    def directory(entries: list[tuple[int, int]]) -> bytes:
        head = struct.pack("<IIHHHH", 0, 0, 0, 0, 0, len(entries))
        return head + b"".join(struct.pack("<II", i, o) for i, o in entries)
    # Layout: root (32), two type dirs, two name dirs, two language dirs (24 each),
    # two data entries (16 each), then the data.
    type_dirs, name_dirs, lang_dirs, data_entries = 32, 32 + 48, 32 + 96, 32 + 144
    blobs = 32 + 144 + 32
    root = directory([(E.RT_ICON, 0x80000000 | type_dirs), (E.RT_GROUP_ICON, 0x80000000 | type_dirs + 24)])
    tree = root
    tree += directory([(1, 0x80000000 | name_dirs)]) + directory([(1, 0x80000000 | name_dirs + 24)])
    tree += directory([(0x409, 0x80000000 | lang_dirs)]) + directory([(0x409, 0x80000000 | lang_dirs + 24)])
    tree += directory([(0x409, data_entries)]) + directory([(0x409, data_entries + 16)])
    tree += struct.pack("<IIII", RSRC_RVA + blobs, len(icon), 0, 0)
    tree += struct.pack("<IIII", RSRC_RVA + blobs + len(icon), len(group), 0, 0)
    return tree + icon + group


def pe32(rsrc: bytes | None) -> bytes:
    """MZ header, PE header, PE32 optional header (224 bytes), one .rsrc section."""
    dos = b"MZ" + b"\0" * 0x3A + struct.pack("<I", 0x40)
    n_sections = 0 if rsrc is None else 1
    coff = b"PE\0\0" + struct.pack("<HHIIIHH", 0x14C, n_sections, 0, 0, 0, 224, 0x102)
    dirs = [(0, 0)] * 16
    if rsrc is not None:
        dirs[2] = (RSRC_RVA, len(rsrc))
    opt = struct.pack("<H", 0x10B) + b"\0" * 90 + struct.pack("<I", 16)
    opt += b"".join(struct.pack("<II", rva, size) for rva, size in dirs)
    head = dos + coff + opt
    if rsrc is None:
        return head
    head += struct.pack("<8sIIIIIIHHI", b".rsrc", len(rsrc), RSRC_RVA, len(rsrc), RSRC_FILE, 0, 0, 0, 0, 0)
    return head.ljust(RSRC_FILE, b"\0") + rsrc


def test_png_group_icon_becomes_icns(tmp_path: Path) -> None:
    icon = png(32)
    group = struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHIH", 32, 32, 0, 0, 1, 32, len(icon), 1)
    images = E.read_icons(pe32(resource_tree(icon, group)))
    assert [(m.size, m.is_png, m.data) for m in images] == [(32, True, icon)]
    out = tmp_path / "AppIcon.icns"
    upscaled = E.write_icns(images, out)
    assert out.read_bytes()[:4] == b"icns"
    assert upscaled == [64, 128, 256, 512, 1024]


def test_no_resource_section_gives_no_icon() -> None:
    assert E.read_icons(pe32(None)) == []
