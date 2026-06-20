#!/usr/bin/env python3
"""
nmv2fm2.py — convert Nintendulator .nmv movie to FCEUX .fm2

NMV binary layout (Nintendulator source, Movie.cpp / States.cpp):
  [4]  magic "NSS\x1a"
  [4]  version (ASCII digits, e.g. "0960")
  [4]  total content length (LE int32)
  [4]  sentinel tag "NMOV"           <- offset 12
  ...  optional savestate blocks
  NMOV data block:
    [4]  tag "NMOV"
    [4]  block data length (LE int32)
    [1]  ControllerTypes[0]  (1=standard, 5=fourscore, …)
    [1]  ControllerTypes[1]
    [1]  ControllerTypes[2]
    [1]  Flags: bit7=PAL, bit6=GameGenie, bits5-0=FrameLen
    [4]  ReRecords (LE int32)
    [4]  Description length N (LE int32)
    [N]  Description text (UTF-8)
    [4]  Movie data length M (LE int32, in bytes)
    [M]  Frame data: FrameLen bytes per frame

Standard controller byte bit layout (LSB first):
  bit0=A  bit1=B  bit2=Select  bit3=Start
  bit4=Up bit5=Down bit6=Left  bit7=Right

FM2 button order per field: R L D U T S B A
"""

import struct
import sys
import uuid
import os
from pathlib import Path


STDCONT_UNCONNECTED   = 0
STDCONT_STANDARD      = 1
STDCONT_FOURSCORE     = 5


def nmv_byte_to_fm2(byte: int) -> str:
    """Convert one NMV standard-controller byte to an 8-char FM2 field (RLDUTSBA)."""
    R = 'R' if byte & 0x80 else '.'
    L = 'L' if byte & 0x40 else '.'
    D = 'D' if byte & 0x20 else '.'
    U = 'U' if byte & 0x10 else '.'
    T = 'T' if byte & 0x08 else '.'
    S = 'S' if byte & 0x04 else '.'
    B = 'B' if byte & 0x02 else '.'
    A = 'A' if byte & 0x01 else '.'
    return R + L + D + U + T + S + B + A


def find_nmov_block(data: bytes) -> int:
    """Return offset of the NMOV *data* block (second occurrence of tag).
    The first 'NMOV' at offset 12 is a sentinel; the block starts at offset 16
    and each block is: [4-byte tag][4-byte LE length][length bytes].
    """
    off = 16
    while off + 8 <= len(data):
        tag = data[off:off+4]
        length = struct.unpack_from('<I', data, off+4)[0]
        if tag == b'NMOV':
            return off
        off += 8 + length
    raise ValueError("NMOV data block not found")


def convert(nmv_path: str, fm2_path: str) -> None:
    raw = Path(nmv_path).read_bytes()

    if raw[:4] != b'NSS\x1a':
        raise ValueError(f"Not a Nintendulator movie file (bad magic): {nmv_path}")

    version_str = raw[4:8].decode('ascii', errors='replace')
    version = int(version_str)
    if version < 950:
        raise ValueError(f"NMV version {version} too old (need >= 950)")

    if raw[12:16] != b'NMOV':
        raise ValueError("Missing NMOV sentinel at offset 12")

    block_off = find_nmov_block(raw)
    block_len = struct.unpack_from('<I', raw, block_off + 4)[0]
    payload = raw[block_off + 8 : block_off + 8 + block_len]

    p = 0
    ct0, ct1, ct2, flags = struct.unpack_from('BBBB', payload, p); p += 4
    rerecords = struct.unpack_from('<I', payload, p)[0]; p += 4
    desc_len  = struct.unpack_from('<I', payload, p)[0]; p += 4
    description = payload[p:p+desc_len].decode('utf-8', errors='replace'); p += desc_len
    movie_len = struct.unpack_from('<I', payload, p)[0]; p += 4
    frame_data = payload[p:p+movie_len]

    pal       = bool(flags & 0x80)
    frame_len = flags & 0x3F          # bytes per frame (sum of all ports)
    fourscore = (ct0 == STDCONT_FOURSCORE)

    if frame_len == 0:
        raise ValueError("FrameLen is 0 — cannot determine frame count")

    frame_count = movie_len // frame_len
    print(f"[nmv2fm2] version={version}, frames={frame_count}, "
          f"FrameLen={frame_len}, PAL={pal}, rerecords={rerecords}, "
          f"fourscore={fourscore}")
    if description:
        print(f"[nmv2fm2] description: {description!r}")

    # FM2 header
    lines = []
    lines.append("version 3")
    lines.append("emuVersion 20500")
    lines.append(f"rerecordCount {rerecords}")
    lines.append(f"palFlag {1 if pal else 0}")
    lines.append(f"romFilename {Path(nmv_path).stem}")
    lines.append("romChecksum base64:AAAAAAAAAAAAAAAAAAAAAA==")
    lines.append(f"guid {str(uuid.uuid4()).upper()}")
    lines.append(f"fourscore {1 if fourscore else 0}")
    if not fourscore:
        lines.append(f"port0 {1 if ct0 == STDCONT_STANDARD else 0}")
        lines.append(f"port1 {1 if ct1 == STDCONT_STANDARD else 0}")
    else:
        lines.append("port0 1")
        lines.append("port1 1")
    lines.append("port2 0")
    lines.append("FDS 0")
    lines.append("NewPPU 0")

    # Frame lines
    off = 0
    for _ in range(frame_count):
        chunk = frame_data[off:off+frame_len]; off += frame_len

        if fourscore:
            # P1+P3 in port0's bytes, P2+P4 in port1's bytes
            p1 = nmv_byte_to_fm2(chunk[0]) if len(chunk) > 0 else '........'
            p2 = nmv_byte_to_fm2(chunk[2]) if len(chunk) > 2 else '........'
            p3 = nmv_byte_to_fm2(chunk[1]) if len(chunk) > 1 else '........'
            p4 = nmv_byte_to_fm2(chunk[3]) if len(chunk) > 3 else '........'
            lines.append(f"|0|{p1}|{p2}|{p3}|{p4}|")
        else:
            p1 = nmv_byte_to_fm2(chunk[0]) if ct0 == STDCONT_STANDARD and len(chunk) > 0 else '........'
            p2 = nmv_byte_to_fm2(chunk[1]) if ct1 == STDCONT_STANDARD and len(chunk) > 1 else '........'
            lines.append(f"|0|{p1}|{p2}|")

    Path(fm2_path).write_text('\n'.join(lines) + '\n')
    print(f"[nmv2fm2] written {frame_count} frames -> {fm2_path}")


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(description="Convert Nintendulator .nmv to FCEUX .fm2")
    ap.add_argument("input",  help=".nmv input file")
    ap.add_argument("output", nargs='?', help=".fm2 output file (default: same name)")
    args = ap.parse_args()

    out = args.output or str(Path(args.input).with_suffix('.fm2'))
    convert(args.input, out)
