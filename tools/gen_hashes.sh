#!/usr/bin/env bash
# gen_hashes.sh — dump FCEUX frame hashes for all games to fm2/GAME.hashes
#
# Usage:  tools/gen_hashes.sh [GAME ...]
#   No args: all games with both fm2/GAME.fm2 and rom/GAME.nes
#
# Requires: fceux in PATH (or FCEUX=/path/to/fceux)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LUA="$SCRIPT_DIR/fceux_framehash.lua"
FCEUX="${FCEUX:-fceux}"

cd "$ROOT"

if [ "$#" -gt 0 ]; then
    GAMES=("$@")
else
    GAMES=()
    for fm2 in fm2/*.fm2; do
        name="$(basename "$fm2" .fm2)"
        if [ -f "rom/$name.nes" ]; then
            GAMES+=("$name")
        fi
    done
fi

echo "Games: ${GAMES[*]}"
echo

for game in "${GAMES[@]}"; do
    fm2="$ROOT/fm2/$game.fm2"
    rom="$ROOT/rom/$game.nes"
    out="$ROOT/fm2/$game.hashes"

    if [ ! -f "$fm2" ]; then echo "[SKIP] $game: no fm2"; continue; fi
    if [ ! -f "$rom" ]; then echo "[SKIP] $game: no rom"; continue; fi

    frames=$(grep -c '^|' "$fm2" || true)
    echo "[RUN]  $game — $frames frames → $out"

    pipe="/tmp/fceux_frames.pipe"
    rm -f "$pipe"
    mkfifo "$pipe"

    # Python reads from pipe in background, hashes frames, writes to out
    python3 "$SCRIPT_DIR/hash_frames.py" < "$pipe" > "$out" &
    PYPID=$!

    # FCEUX writes to pipe (blocks until Python opens the read end)
    FRAME_PIPE="$pipe" FRAME_LIMIT="$frames" \
        "$FCEUX" --sound 0 \
                 --loadlua "$LUA" \
                 --playmov "$fm2" \
                 "$rom" 2>&1 | grep "\[fceux_framehash\]" || true

    wait $PYPID
    rm -f "$pipe"
    echo
done

echo "Done."
