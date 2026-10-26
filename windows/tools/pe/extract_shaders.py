#!/usr/bin/env python3
"""Extract the HLSL source bodies and the compiled D3D9 shader blobs from a game exe.

Usage: extract_shaders.py <exe> <out_dir>

Writes <out_dir>/hlsl/<name>.hlsl and <out_dir>/blobs/<profile>_<file_offset>.bin.
The exe is read as data only; nothing in it is executed.

HLSL bodies are found as long printable-text runs in the data sections that contain
HLSL keywords. Blobs are found by the D3D9 token layout: a version token, a comment
token holding "CTAB", then the instruction tokens up to the 0x0000FFFF end token.
"""
import os
import struct
import sys

MIN_TEXT_RUN = 1000
HLSL_MARKERS = (b"float4", b"float3", b"half4", b"#define", b"#include", b"sampler", b"return ")
TEXT_NAMES = (
    (b"#define UBERSHADER", "ubershader"),
    (b"deferredFilterCommon.hlsl", "deferredFilterCommon"),
    (b"deferredFilterShadows.hlsl", "deferredFilterShadows"),
    (b"deferredFilterLighting.hlsl", "deferredFilterLighting"),
    (b"fog.hlsl", "fog"),
    (b"HDRToLDR", "hdr"),
)
PROFILES = {
    (0xFFFE, 0x0101): "vs_1_1",
    (0xFFFE, 0x0200): "vs_2_0",
    (0xFFFE, 0x0300): "vs_3_0",
    (0xFFFF, 0x0101): "ps_1_1",
    (0xFFFF, 0x0200): "ps_2_0",
    (0xFFFF, 0x0300): "ps_3_0",
}
END_TOKEN = 0x0000FFFF
COMMENT_OPCODE = 0xFFFE
MAX_BLOB_BYTES = 16384


def read_sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("not a PE file")
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    sec = pe + 24 + opt_size
    out = []
    for i in range(nsec):
        name, vsize, rva, rsize, raw = struct.unpack_from("<8sIIII", data, sec + 40 * i)
        out.append((name.rstrip(b"\0").decode("ascii", "replace"), rva, raw, rsize))
    return out


def text_runs(data, start, end):
    """Yield (offset, bytes) for printable runs of at least MIN_TEXT_RUN bytes."""
    printable = bytes(range(32, 127)) + b"\t\r\n"
    table = bytearray(256)
    for b in printable:
        table[b] = 1
    run_start = None
    for i in range(start, end):
        if table[data[i]]:
            if run_start is None:
                run_start = i
        elif run_start is not None:
            if i - run_start >= MIN_TEXT_RUN:
                yield run_start, data[run_start:i]
            run_start = None
    if run_start is not None and end - run_start >= MIN_TEXT_RUN:
        yield run_start, data[run_start:end]


def extract_hlsl(data, sections):
    found = []
    for name, _rva, raw, rsize in sections:
        if name != ".data":
            continue
        for off, body in text_runs(data, raw, raw + rsize):
            hits = sum(1 for m in HLSL_MARKERS if m in body)
            if hits >= 2 and b"\0" not in body:
                found.append((off, body))
    return found


def name_for(body, taken):
    head = body[:600]
    for marker, label in TEXT_NAMES:
        if marker in head:
            break
    else:
        label = "hlsl_%d" % len(taken)
    while label in taken:
        label += "_"
    taken.add(label)
    return label


def walk_blob(data, off, limit):
    """Return the blob length in bytes if data[off:] is a plausible shader, else 0."""
    if off + 16 > limit:
        return 0
    ver = struct.unpack_from("<I", data, off)[0]
    if (ver >> 16, ver & 0xFFFF) not in PROFILES:
        return 0
    second = struct.unpack_from("<I", data, off + 4)[0]
    if (second & 0xFFFF) != COMMENT_OPCODE or data[off + 8:off + 12] != b"CTAB":
        return 0
    pos = off + 4
    end = min(limit, off + MAX_BLOB_BYTES)
    while pos + 4 <= end:
        tok = struct.unpack_from("<I", data, pos)[0]
        if tok == END_TOKEN:
            return pos + 4 - off
        if (tok & 0xFFFF) == COMMENT_OPCODE and (tok >> 31) == 0:
            pos += 4 + 4 * ((tok >> 16) & 0x7FFF)
        else:
            pos += 4
    return 0


def extract_blobs(data, sections):
    found = []
    for name, _rva, raw, rsize in sections:
        if name not in (".rdata", ".data"):
            continue
        off = raw
        limit = raw + rsize
        while off + 16 <= limit:
            n = walk_blob(data, off, limit)
            if n:
                ver = struct.unpack_from("<I", data, off)[0]
                found.append((off, PROFILES[(ver >> 16, ver & 0xFFFF)], data[off:off + n]))
                off += (n + 3) & ~3
            else:
                off += 4
    return found


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    exe, out_dir = sys.argv[1], sys.argv[2]
    with open(exe, "rb") as f:
        data = f.read()
    sections = read_sections(data)
    os.makedirs(os.path.join(out_dir, "hlsl"), exist_ok=True)
    os.makedirs(os.path.join(out_dir, "blobs"), exist_ok=True)

    bodies = extract_hlsl(data, sections)
    taken = set()
    for off, body in bodies:
        label = name_for(body, taken)
        with open(os.path.join(out_dir, "hlsl", label + ".hlsl"), "wb") as f:
            f.write(body)
        print("hlsl %-24s file_offset=0x%x bytes=%d" % (label, off, len(body)))

    blobs = extract_blobs(data, sections)
    counts = {}
    for off, prof, blob in blobs:
        counts[prof] = counts.get(prof, 0) + 1
        with open(os.path.join(out_dir, "blobs", "%s_%x.bin" % (prof, off)), "wb") as f:
            f.write(blob)

    print("wrote %d HLSL bodies (%d bytes) and %d shader blobs (%d bytes)" % (
        len(bodies), sum(len(b) for _, b in bodies), len(blobs), sum(len(b) for _, _, b in blobs)))
    print("blobs by profile: " + ", ".join("%s x%d" % kv for kv in sorted(counts.items())))


if __name__ == "__main__":
    main()
