#!/usr/bin/env bash
# capture_range.sh — capture frames 1..N for both our emulator and FCEUX
#
# Usage:
#   tools/capture_range.sh GAME FRAME_LIMIT
#
# Output:
#   ./tmp/screenshots/ours_GAME_NNN.png
#   ./tmp/screenshots/fceux_GAME_NNN.png
#
# Example:
#   tools/capture_range.sh Felix 116

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

GAME="${1:?Usage: $0 GAME FRAME_LIMIT}"
LIMIT="${2:?Usage: $0 GAME FRAME_LIMIT}"

OUTDIR="$ROOT/tmp/screenshots"
mkdir -p "$OUTDIR"

BIN="$ROOT/bin/$GAME"
FM2="$ROOT/fm2/$GAME.fm2"
ROM="$ROOT/rom/$GAME.nes"
FCEUX="${FCEUX:-fceux}"
LUA_RANGE="$SCRIPT_DIR/fceux_screenshot_range.lua"
LUA_SINGLE="$SCRIPT_DIR/fceux_screenshot.lua"

if [ ! -f "$BIN" ]; then echo "ERROR: $BIN not found"; exit 1; fi
if [ ! -f "$FM2" ]; then echo "ERROR: $FM2 not found"; exit 1; fi
if [ ! -f "$ROM" ]; then echo "ERROR: $ROM not found"; exit 1; fi

# ── OUR EMULATOR: loop frames 1..LIMIT ──────────────────────────────────────
echo "[capture] Our emulator: frames 1..$LIMIT"
for ((f=1; f<=LIMIT; f++)); do
    out="$OUTDIR/ours_${GAME}_$(printf '%06d' $f).png"
    if [ -f "$out" ]; then continue; fi
    "$BIN" --headless --playback "$FM2" --frames "$f" --screenshot "$out" 2>/dev/null
done
echo "[capture] Our emulator done."

# ── FCEUX: batch capture all frames in one run ───────────────────────────────
echo "[capture] FCEUX: frames 1..$LIMIT via range lua"
GD2_DIR="$ROOT/tmp/screenshots/fceux_gd2_${GAME}"
mkdir -p "$GD2_DIR"

FRAME_LIMIT="$LIMIT" SCREENSHOT_DIR="$GD2_DIR" \
    "$FCEUX" --sound 0 \
             --loadlua "$LUA_RANGE" \
             --playmov "$FM2" \
             "$ROM" 2>&1 | grep -E "\[screenshot_range\]" || true

# Convert GD2 → PNG
echo "[capture] Converting GD2 → PNG..."
python3 "$SCRIPT_DIR/gd2_to_png.py" "$GD2_DIR" "$OUTDIR" "fceux_${GAME}"

echo "[capture] Done. Files in $OUTDIR"
