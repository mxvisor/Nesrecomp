#!/usr/bin/env bash
# Take a screenshot from every game that has a TAS file in fm2/.
# Each game runs headless with TAS playback up to a chosen point in GAME time,
# then saves a PNG screenshot to docs/assets/.
#
# The point is given in seconds of *game* time (60 fps). Because headless
# playback runs at maximum speed (not real time), seconds are converted to a
# frame count (seconds * 60) and passed as --frames, so the shot is taken at
# the right game moment regardless of how fast the host runs.
#
# Usage:
#   ./tools/make_screenshots.sh [--seconds N] [--scale N] [game ...]
#
# Examples:
#   ./tools/make_screenshots.sh                  # all games, 5 s of game time
#   ./tools/make_screenshots.sh --seconds 10     # all games, 10 s of game time
#   ./tools/make_screenshots.sh Mario Zelda      # specific games only

set -euo pipefail
cd "$(dirname "$0")/.."

SECONDS_ARG=5
SCALE=1
GAMES=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --seconds) SECONDS_ARG="$2"; shift 2 ;;
        --scale)   SCALE="$2";       shift 2 ;;
        *)         GAMES+=("$1");    shift   ;;
    esac
done

mkdir -p docs/assets

if [[ ${#GAMES[@]} -eq 0 ]]; then
    for fm2 in fm2/*.fm2; do
        [[ -f "$fm2" ]] || continue
        game=$(basename "$fm2" .fm2)
        GAMES+=("$game")
    done
fi

ok=0; fail=0
for game in "${GAMES[@]}"; do
    bin="bin/$game"
    fm2="fm2/$game.fm2"
    out="docs/assets/$game.png"

    if [[ ! -f "$bin" ]]; then
        echo "[$game] binary not found — skipping (run: make GAME=$game)"
        ((fail++)) || true
        continue
    fi
    if [[ ! -f "$fm2" ]]; then
        echo "[$game] no TAS file ($fm2) — skipping"
        ((fail++)) || true
        continue
    fi

    frames=$(( SECONDS_ARG * 60 ))   # seconds of game time → frames @ 60 fps
    echo -n "[$game] running ${SECONDS_ARG}s (${frames} frames) ... "
    if timeout 120 \
            "./$bin" --headless --frames "$frames" \
                     --playback "$fm2" \
                     --screenshot "$out" \
                     --scale "$SCALE" \
            2>&1 | grep -o "\[screenshot\].*"; then
        ((ok++)) || true
    else
        echo "FAILED"
        ((fail++)) || true
    fi
done

echo ""
echo "Done: $ok ok, $fail failed"
