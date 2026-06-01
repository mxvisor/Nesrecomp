#!/usr/bin/env bash
# Take a screenshot from every game that has a TAS file in fm2/.
# Each game runs headless for SECONDS seconds with TAS playback,
# then saves a PNG screenshot to docs/assets/.
#
# Usage:
#   ./tools/make_screenshots.sh [--seconds N] [--scale N] [game ...]
#
# Examples:
#   ./tools/make_screenshots.sh                  # all games, 5 s
#   ./tools/make_screenshots.sh --seconds 10     # all games, 10 s
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

    echo -n "[$game] running $SECONDS_ARG s ... "
    if timeout $((SECONDS_ARG + 10)) \
            "./$bin" --headless --seconds "$SECONDS_ARG" \
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
