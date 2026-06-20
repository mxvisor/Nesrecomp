#!/usr/bin/env python3
"""Convert GD2 files from a directory to PNG files.

Usage:
    gd2_to_png.py GD2_DIR OUT_DIR PREFIX

GD2_DIR contains files named frame_NNNNNN.gd2
Output files: OUT_DIR/PREFIX_NNNNNN.png
"""
import sys, struct, zlib
from pathlib import Path

def gd2_to_png_bytes(data: bytes) -> bytes:
    # GD2 header: FF FE, w(2BE), h(2BE), truecolor(1), bgcolor(4) = 11 bytes
    w = struct.unpack_from(">H", data, 2)[0]
    h = struct.unpack_from(">H", data, 4)[0]

    def png_chunk(tag, d):
        c = zlib.crc32(tag + d) & 0xFFFFFFFF
        return struct.pack(">I", len(d)) + tag + d + struct.pack(">I", c)

    raw_rows = bytearray()
    for y in range(h):
        raw_rows += b"\x00"  # filter type None
        for x in range(w):
            i = 11 + (y * w + x) * 4
            a, r, g, b = data[i], data[i+1], data[i+2], data[i+3]
            raw_rows += bytes([r, g, b])

    compressed = zlib.compress(bytes(raw_rows), 6)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n"
    png += png_chunk(b"IHDR", ihdr)
    png += png_chunk(b"IDAT", compressed)
    png += png_chunk(b"IEND", b"")
    return png

if __name__ == "__main__":
    gd2_dir = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    prefix = sys.argv[3] if len(sys.argv) > 3 else "frame"
    out_dir.mkdir(parents=True, exist_ok=True)

    files = sorted(gd2_dir.glob("frame_*.gd2"))
    for f in files:
        num = f.stem.split("_")[1]
        out_path = out_dir / f"{prefix}_{num}.png"
        data = f.read_bytes()
        if len(data) > 11:
            out_path.write_bytes(gd2_to_png_bytes(data))
    print(f"[gd2_to_png] converted {len(files)} files → {out_dir}")
