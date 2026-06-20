#!/usr/bin/env bash
# fceux_screenshot.sh — capture a PNG from FCEUX at a given frame during FM2 playback
#
# Usage:
#   tools/fceux_screenshot.sh GAME FRAME [OUT.png]
#
# Example:
#   tools/fceux_screenshot.sh Felix 116
#   tools/fceux_screenshot.sh Felix 116 /tmp/ref_116.png

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FCEUX="${FCEUX:-fceux}"

GAME="${1:?Usage: $0 GAME FRAME [OUT.png]}"
FRAME="${2:?Usage: $0 GAME FRAME [OUT.png]}"
OUT="${3:-$ROOT/tmp/screenshots/fceux_${GAME}/frame_$(printf '%06d' ${FRAME}).png}"
mkdir -p "$(dirname "$OUT")"

FM2="$ROOT/fm2/$GAME.fm2"
ROM="$ROOT/rom/$GAME.nes"
LUA="$SCRIPT_DIR/fceux_screenshot.lua"

if [ ! -f "$FM2" ]; then echo "ERROR: $FM2 not found"; exit 1; fi
if [ ! -f "$ROM" ]; then echo "ERROR: $ROM not found"; exit 1; fi

echo "[screenshot] $GAME frame $FRAME → $OUT"

SCREENSHOT_FRAME="$FRAME" SCREENSHOT_OUT="$OUT" \
    "$FCEUX" --sound 0 \
             --loadlua "$LUA" \
             --playmov "$FM2" \
             "$ROM" 2>&1 | grep -E "\[screenshot\]|Error" || true

# If GD2 fallback was used, convert to PNG via Python
if [ ! -f "$OUT" ] && [ -f "${OUT}.gd2" ]; then
    python3 - "${OUT}.gd2" "$OUT" << 'PYEOF'
import sys, struct, zlib
from pathlib import Path

gd2_path, png_path = sys.argv[1], sys.argv[2]
data = Path(gd2_path).read_bytes()
# GD2 header: FF FE, w(2BE), h(2BE), truecolor(1), bgcolor(4) = 11 bytes
w = struct.unpack_from(">H", data, 2)[0]
h = struct.unpack_from(">H", data, 4)[0]
pixels = data[11:]  # ARGB per pixel, A=0 means opaque

# Convert to RGB PNG
import zlib, struct

def png_chunk(tag, data):
    c = zlib.crc32(tag + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", c)

raw_rows = b""
for y in range(h):
    raw_rows += b"\x00"  # filter type None
    for x in range(w):
        i = (y * w + x) * 4
        a, r, g, b = data[11+i], data[11+i+1], data[11+i+2], data[11+i+3]
        raw_rows += bytes([r, g, b])

compressed = zlib.compress(raw_rows, 9)
ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
png = b"\x89PNG\r\n\x1a\n"
png += png_chunk(b"IHDR", ihdr)
png += png_chunk(b"IDAT", compressed)
png += png_chunk(b"IEND", b"")
Path(png_path).write_bytes(png)
print(f"[screenshot] converted GD2 → {png_path} ({w}x{h})")
PYEOF
    rm -f "${OUT}.gd2"
fi

if [ -f "$OUT" ]; then
    echo "[screenshot] saved: $OUT"
else
    echo "[screenshot] ERROR: screenshot not created"
    exit 1
fi
