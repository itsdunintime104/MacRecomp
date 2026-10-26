"""Write the icon of a Windows PE image (the game exe) as a macOS .icns.

Usage: exe_icon.py <exe> <out.icns>

It takes the RT_GROUP_ICON with the largest image, converts each of its RT_ICON
images (PNG, or BMP DIB through sips) to PNG, fills the iconset sizes from them,
and runs iconutil. Exit 1 when the exe has no icon.
"""

from __future__ import annotations

import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

RT_ICON = 3
RT_GROUP_ICON = 14
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
SIPS = "/usr/bin/sips"
ICONUTIL = "/usr/bin/iconutil"
# (pixel size, iconset file name) for every image an .iconset holds.
ICONSET_SLOTS = [
    (16, "icon_16x16"), (32, "icon_16x16@2x"),
    (32, "icon_32x32"), (64, "icon_32x32@2x"),
    (128, "icon_128x128"), (256, "icon_128x128@2x"),
    (256, "icon_256x256"), (512, "icon_256x256@2x"),
    (512, "icon_512x512"), (1024, "icon_512x512@2x"),
]


@dataclass(frozen=True)
class IconImage:
    size: int        # width in pixels
    bit_count: int   # colour depth; PNG counts as 32
    data: bytes      # the RT_ICON resource: a PNG file or a BMP DIB
    is_png: bool


def _u16(data: bytes, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


def _u32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def _resource_root(data: bytes) -> tuple[int, list[tuple[int, int, int]]] | None:
    """File offset of the resource directory and the sections as (va, span, file offset)."""
    if data[:2] != b"MZ" or len(data) < 0x40:
        return None
    pe = _u32(data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    n_sections, opt_size = _u16(data, pe + 6), _u16(data, pe + 20)
    opt = pe + 24
    magic = _u16(data, opt)
    if magic == 0x10B:
        n_dirs, dirs = _u32(data, opt + 92), opt + 96
    elif magic == 0x20B:
        n_dirs, dirs = _u32(data, opt + 108), opt + 112
    else:
        return None
    if n_dirs <= 2:
        return None
    rva, size = struct.unpack_from("<II", data, dirs + 16)
    if rva == 0 or size == 0:
        return None
    sections: list[tuple[int, int, int]] = []
    for i in range(n_sections):
        vsize, va, raw_size, raw_ptr = struct.unpack_from("<IIII", data, opt + opt_size + 40 * i + 8)
        sections.append((va, max(vsize, raw_size), raw_ptr))
    root = _rva_to_offset(sections, rva)
    return None if root is None else (root, sections)


def _rva_to_offset(sections: list[tuple[int, int, int]], rva: int) -> int | None:
    for va, span, raw_ptr in sections:
        if va <= rva < va + span:
            return rva - va + raw_ptr
    return None


def _entries(data: bytes, directory: int) -> list[tuple[int, int]]:
    """(name or id, offset field) of each entry of one IMAGE_RESOURCE_DIRECTORY."""
    count = _u16(data, directory + 12) + _u16(data, directory + 14)
    return [struct.unpack_from("<II", data, directory + 16 + 8 * i) for i in range(count)]


def _leaf(data: bytes, root: int, sections: list[tuple[int, int, int]], field: int) -> bytes | None:
    """Bytes of the first data entry under one entry (the first language)."""
    while field & 0x80000000:
        sub = _entries(data, root + (field & 0x7FFFFFFF))
        if not sub:
            return None
        field = sub[0][1]
    rva, size = struct.unpack_from("<II", data, root + field)
    off = _rva_to_offset(sections, rva)
    return None if off is None else data[off:off + size]


def _image(raw: bytes) -> IconImage:
    if raw.startswith(PNG_SIGNATURE):
        return IconImage(struct.unpack_from(">I", raw, 16)[0], 32, raw, True)
    return IconImage(struct.unpack_from("<i", raw, 4)[0], _u16(raw, 14), raw, False)


def read_icons(data: bytes) -> list[IconImage]:
    """Images of the group icon with the largest image; [] when the exe has none."""
    found = _resource_root(data)
    if found is None:
        return []
    root, sections = found
    icons: dict[int, bytes] = {}
    groups: list[bytes] = []
    for type_id, type_field in _entries(data, root):
        if type_id not in (RT_ICON, RT_GROUP_ICON) or not type_field & 0x80000000:
            continue
        for name_id, field in _entries(data, root + (type_field & 0x7FFFFFFF)):
            raw = _leaf(data, root, sections, field)
            if raw is None:
                continue
            if type_id == RT_ICON:
                icons[name_id] = raw
            else:
                groups.append(raw)
    best: list[IconImage] = []
    for group in groups:
        ids = [_u16(group, 6 + 14 * i + 12) for i in range(_u16(group, 4))]
        images = [_image(icons[i]) for i in ids if i in icons]
        if images and (not best or max(m.size for m in images) > max(m.size for m in best)):
            best = images
    return best


def _to_png(image: IconImage, out: Path) -> None:
    if image.is_png:
        out.write_bytes(image.data)
        return
    # sips reads a DIB as a one-image .ico file.
    ico = out.with_suffix(".ico")
    entry = struct.pack("<BBBBHHII", image.size % 256, image.size % 256, 0, 0, 1,
                        image.bit_count, len(image.data), 22)
    ico.write_bytes(struct.pack("<HHH", 0, 1, 1) + entry + image.data)
    subprocess.run([SIPS, "-s", "format", "png", str(ico), "--out", str(out)],
                   check=True, capture_output=True)


def write_icns(images: list[IconImage], out: Path) -> list[int]:
    """Write the .icns; return the iconset sizes made by upscaling."""
    upscaled: list[int] = []
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        pngs: dict[int, Path] = {}
        # Keep the deepest image of each size.
        for image in sorted(images, key=lambda m: m.bit_count):
            pngs[image.size] = work / f"src_{image.size}.png"
            _to_png(image, pngs[image.size])
        iconset = work / "icon.iconset"
        iconset.mkdir()
        for px, name in ICONSET_SLOTS:
            dest = iconset / f"{name}.png"
            if px in pngs:
                shutil.copyfile(pngs[px], dest)
                continue
            larger = [s for s in pngs if s > px]
            src = min(larger) if larger else max(pngs)
            if not larger and px not in upscaled:
                upscaled.append(px)
            subprocess.run([SIPS, "-z", str(px), str(px), str(pngs[src]), "--out", str(dest)],
                           check=True, capture_output=True)
        subprocess.run([ICONUTIL, "-c", "icns", str(iconset), "-o", str(out)],
                       check=True, capture_output=True)
    return upscaled


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: exe_icon.py <exe> <out.icns>", file=sys.stderr)
        return 2
    exe, out = Path(argv[1]), Path(argv[2])
    images = read_icons(exe.read_bytes())
    if not images:
        print(f"exe_icon: no icon in {exe.name}", file=sys.stderr)
        return 1
    upscaled = write_icns(images, out)
    sizes = " ".join(str(s) for s in sorted({m.size for m in images}))
    print(f"exe_icon: {exe.name}: icon sizes {sizes}; upscaled {' '.join(map(str, upscaled)) or 'none'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
