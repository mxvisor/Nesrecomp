#!/usr/bin/env python3
# Reads raw frame stream from stdin, writes CRC32 hashes to stdout.
# Format in:  [4-byte LE frame number] [245760 bytes ARGB pixels] per frame
# Format out: "NNNNNN HHHHHHHH\n" per frame
import sys, zlib

FRAME_SIZE = 256 * 240 * 4

stdin  = sys.stdin.buffer
stdout = sys.stdout

while True:
    hdr = stdin.read(4)
    if len(hdr) < 4:
        break
    frame = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16)
    data  = stdin.read(FRAME_SIZE)
    if len(data) < FRAME_SIZE:
        break
    h = zlib.crc32(data) & 0xFFFFFFFF
    stdout.write(f"{frame:06d} {h:08X}\n")

stdout.flush()
