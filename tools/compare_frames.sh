#!/bin/bash
# compare_frames.sh — compare frame hashes between our recompiler and FCEUX
#
# Usage: ./tools/compare_frames.sh [GAME] [FRAMES]
#   GAME   = Battlecity | Mario | Zelda | ...  (default: Battlecity)
#   FRAMES = how many frames to compare        (default: 600 = ~10 sec)
#            0 = entire movie
#
# Requires: FCEUX with Lua support, rom/GAME.nes, fm2/GAME.fm2, bin/GAME
#
# Output:
#   /tmp/nes_frames_OUR.txt    — our emulator hashes
#   /tmp/nes_frames_FCEUX.txt  — FCEUX hashes
#   /tmp/nes_frames_diff.txt   — first mismatches

set -euo pipefail

GAME="${1:-Battlecity}"
FRAMES="${2:-600}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ROM="$ROOT/rom/${GAME}.nes"
FM2="$ROOT/fm2/${GAME}.fm2"
BIN="$ROOT/bin/${GAME}"

OUR_OUT="/tmp/nes_frames_OUR.txt"
FCEUX_OUT="/tmp/nes_frames_FCEUX.txt"
DIFF_OUT="/tmp/nes_frames_diff.txt"

if [ ! -f "$ROM" ]; then echo "ROM not found: $ROM"; exit 1; fi
if [ ! -f "$FM2" ]; then echo "FM2 not found: $FM2"; exit 1; fi
if [ ! -f "$BIN" ]; then echo "Binary not found: $BIN — run: make GAME=$GAME"; exit 1; fi

echo "=== Our emulator: $GAME (frames: ${FRAMES:-all}) ==="
GAME=$GAME "$BIN" --headless --playback "$FM2" \
    ${FRAMES:+--frames "$FRAMES"} \
    --dump-frames "$OUR_OUT" 2>/dev/null || true
echo "  → $OUR_OUT ($(wc -l < "$OUR_OUT") frames)"

echo ""
echo "=== FCEUX: $GAME ==="
HASHES="$ROOT/fm2/${GAME}.hashes"
if [ ! -f "$HASHES" ]; then
    echo "  No precomputed hashes found: $HASHES"
    echo "  Run: tools/gen_hashes.sh $GAME"
    exit 1
fi
if [ "$FRAMES" -gt 0 ]; then
    head -"$FRAMES" "$HASHES" > "$FCEUX_OUT"
else
    cp "$HASHES" "$FCEUX_OUT"
fi
echo "  → $FCEUX_OUT ($(wc -l < "$FCEUX_OUT") frames)"

echo ""
echo "=== Comparison ==="

python3 - "$OUR_OUT" "$FCEUX_OUT" "$DIFF_OUT" <<'EOF'
import sys

our_file, fceux_file, diff_file = sys.argv[1], sys.argv[2], sys.argv[3]

our   = {}
fceux = {}

with open(our_file) as f:
    for line in f:
        parts = line.split()
        if len(parts) == 2:
            our[int(parts[0])] = parts[1]

with open(fceux_file) as f:
    for line in f:
        parts = line.split()
        if len(parts) == 2:
            fceux[int(parts[0])] = parts[1]

all_frames = sorted(set(our) | set(fceux))
mismatches = []
for fr in all_frames:
    h_our   = our.get(fr,   "MISSING")
    h_fceux = fceux.get(fr, "MISSING")
    if h_our != h_fceux:
        mismatches.append((fr, h_our, h_fceux))

with open(diff_file, 'w') as f:
    f.write(f"# Total frames: our={len(our)} fceux={len(fceux)}\n")
    f.write(f"# Mismatches: {len(mismatches)} / {len(all_frames)}\n")
    for fr, h_our, h_fceux in mismatches[:50]:
        f.write(f"frame {fr:6d}: OUR={h_our} FCEUX={h_fceux}\n")

print(f"Frames compared: {len(all_frames)}")
print(f"Mismatches:      {len(mismatches)}")
if mismatches:
    print(f"First mismatch:  frame {mismatches[0][0]}")
    print(f"  OUR  = {mismatches[0][1]}")
    print(f"  FCEUX= {mismatches[0][2]}")
    print(f"Full diff → {diff_file}")
else:
    print("All frames match!")
EOF
