"""
Recompiler configuration - section mappings and constants.

These describe the memory layout of the image being translated. Call
configure_from_macho() before translating anything; it installs the section
table from analysis.json. The module-level values below are only a fallback for
callers that never configure (and for tests); they come from an Xbox title and
are wrong for any other binary.
"""

from dataclasses import dataclass
from typing import List, Optional


@dataclass
class Section:
    name: str
    va: int
    va_size: int
    raw_addr: int
    raw_size: int
    is_code: bool


# Populated by configure_from_macho(). Fallback: Xbox Dashboard build 3944.
_SECTIONS: List[Section] = [
    Section(".text", 0x00012000, 636684, 0x00002000, 636684, True),
    Section("D3D", 0x000AD720, 72432, 0x0009E000, 72432, True),
    Section("D3DX", 0x000BF220, 153536, 0x000AD000, 153536, True),
    Section("XGRPH", 0x000E49E0, 7624, 0x000D3000, 7624, True),
    Section("DSOUND", 0x000E67C0, 31344, 0x000D5000, 31344, True),
    Section("WMADECXM", 0x000EE240, 4912, 0x000DD000, 4912, True),
    Section("WMADEC", 0x000EF580, 101720, 0x000DF000, 101720, True),
    Section("DVDTHUNK", 0x001082E0, 1116, 0x000F8000, 1116, True),
    Section("XPP", 0x00108740, 31436, 0x000F9000, 31436, True),
    Section(".data", 0x00110220, 140392, 0x00101000, 140392, False),
    Section("DOLBY", 0x001326A0, 28056, 0x00113000, 28056, True),
    Section(".data1", 0x00139440, 7536, 0x0011A000, 7536, False),
    Section("XIPS", 0x0013B1C0, 20760, 0x0011C000, 20760, True),
]

# Legacy tuple view: (name, va, va_size, raw_addr). Several callers iterate this.
SECTIONS = [(s.name, s.va, s.va_size, s.raw_addr) for s in _SECTIONS]

TEXT_VA_START = 0x00012000
TEXT_VA_END = 0x000AD720
RDATA_VA_START = 0x00110220
RDATA_VA_END = 0x00170000
DATA_VA_START = 0x00110220
DATA_VA_END = 0x00170000
KERNEL_THUNK_ADDR = 0x00012000
ENTRY_POINT = 0x00052A81

# Display name in generated-code banners when the image has no title.
GAME_NAME = "Xbox Game"


def banner_name(title: Optional[str] = None) -> str:
    """The name to stamp into a generated-code banner: the title, else GAME_NAME."""
    return title or GAME_NAME


# Mach-O sections that hold code. Everything else is data.
MACHO_CODE_SECTIONS = {"__text", "__stub_helper"}
# Section types (flags & SECTION_TYPE) with no file bytes.
_MACHO_ZEROFILL_TYPES = {0x01, 0x0C, 0x12}


def configure_from_macho(analysis: dict, image_path, entry_point: int) -> None:
    """Install the section layout from our analyzer's analysis.json.

    Each section becomes one Section named SEG.sect. Zero-fill sections get
    raw_size 0, so va_to_file_offset returns None inside them.
    """
    sections = []
    for seg in analysis["segments"]:
        for sec in seg["sections"]:
            zerofill = (sec["flags"] & 0xFF) in _MACHO_ZEROFILL_TYPES
            sections.append(Section(
                name=f"{seg['name']}.{sec['name']}",
                va=sec["addr"],
                va_size=sec["size"],
                raw_addr=sec["offset"],
                raw_size=0 if zerofill else sec["size"],
                is_code=sec["name"] in MACHO_CODE_SECTIONS,
            ))
    if not sections:
        raise ValueError(f"analysis has no sections: {image_path}")
    _install(sections, entry_point, 0)


def _install(sections, entry_point, kernel_thunk_addr):
    global _SECTIONS, SECTIONS
    global TEXT_VA_START, TEXT_VA_END, RDATA_VA_START, RDATA_VA_END
    global DATA_VA_START, DATA_VA_END, KERNEL_THUNK_ADDR, ENTRY_POINT

    _SECTIONS = sorted(sections, key=lambda s: s.va)
    SECTIONS = [(s.name, s.va, s.va_size, s.raw_addr) for s in _SECTIONS]

    by_name = {s.name: s for s in _SECTIONS}

    text = by_name.get(".text") or next(
        (s for s in _SECTIONS if s.is_code), _SECTIONS[0]
    )
    TEXT_VA_START = text.va
    TEXT_VA_END = text.va + text.va_size

    rdata = by_name.get(".rdata")
    data = by_name.get(".data")
    # Some titles (the Dashboard among them) ship no .rdata; fall back to .data
    # so the read-only-data range is never left pointing at another game.
    rdata = rdata or data
    data = data or rdata
    if rdata:
        RDATA_VA_START = rdata.va
        RDATA_VA_END = rdata.va + rdata.va_size
    if data:
        DATA_VA_START = data.va
        DATA_VA_END = data.va + data.va_size

    if entry_point:
        ENTRY_POINT = entry_point
    if kernel_thunk_addr:
        KERNEL_THUNK_ADDR = kernel_thunk_addr


def va_to_file_offset(va):
    """File offset of a virtual address, or None if it has no file bytes."""
    for s in _SECTIONS:
        if s.va <= va < s.va + s.va_size:
            offset = va - s.va + s.raw_addr
            # Sections are zero-padded when virtual_size exceeds raw_size (BSS
            # tails); those addresses exist at runtime but have no file bytes.
            if va - s.va >= s.raw_size:
                return None
            return offset
    return None


def is_code_address(va):
    """True if VA is in a code section."""
    for s in _SECTIONS:
        if s.va <= va < s.va + s.va_size:
            return s.is_code
    return False
