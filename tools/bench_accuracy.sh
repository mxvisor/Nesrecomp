#!/usr/bin/env bash
# bench_accuracy.sh — compare recompiler/interpreter accuracy against FCEUX hashes
#
# Usage: tools/bench_accuracy.sh [--interp | --recomp] [GAME ...]
#   --interp   only interpreter
#   --recomp   only recompiler
#   (default)  both
#   No GAME args: all games that have bin/, fm2/.hashes, and fm2/.fm2

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$ROOT"

MODE="both"  # both | interp | recomp
GAMES=()

for arg in "$@"; do
    case "$arg" in
        --interp) MODE="interp" ;;
        --recomp) MODE="recomp" ;;
        *) GAMES+=("$arg") ;;
    esac
done

if [ "${#GAMES[@]}" -eq 0 ]; then
    for hf in fm2/*.hashes; do
        name="$(basename "$hf" .hashes)"
        if [ -f "bin/$name" ] && [ -f "fm2/$name.fm2" ]; then
            GAMES+=("$name")
        fi
    done
fi

compare_hashes() {
    local our="$1" ref="$2"
    python3 - "$our" "$ref" <<'EOF'
import sys
our_f, ref_f = sys.argv[1], sys.argv[2]
our = {}
ref = {}
with open(our_f) as f:
    for line in f:
        p = line.split()
        if len(p) == 2: our[int(p[0])] = p[1]
with open(ref_f) as f:
    for line in f:
        p = line.split()
        if len(p) == 2: ref[int(p[0])] = p[1]
n = len(ref)
if n == 0:
    print("0/0 (0.00%)")
    sys.exit()
match = sum(1 for k in ref if our.get(k) == ref[k])
first = next((k for k in sorted(ref) if our.get(k) != ref.get(k)), None)
print(f"{match}/{n} ({100*match/n:.2f}%) first_mismatch={first}")
EOF
}

case "$MODE" in
    both)
        printf "%-14s  %-30s  %-30s\n" "Game" "Recompiler vs FCEUX" "Interpreter vs FCEUX"
        printf "%-14s  %-30s  %-30s\n" "----" "-------------------" "--------------------"
        ;;
    recomp)
        printf "%-14s  %-30s\n" "Game" "Recompiler vs FCEUX"
        printf "%-14s  %-30s\n" "----" "-------------------"
        ;;
    interp)
        printf "%-14s  %-30s\n" "Game" "Interpreter vs FCEUX"
        printf "%-14s  %-30s\n" "----" "--------------------"
        ;;
esac

for game in "${GAMES[@]}"; do
    bin="$ROOT/bin/$game"
    fm2="$ROOT/fm2/$game.fm2"
    ref="$ROOT/fm2/$game.hashes"

    if [ ! -f "$bin" ] || [ ! -f "$fm2" ] || [ ! -f "$ref" ]; then
        printf "%-14s  (skip — missing bin/fm2/hashes)\n" "$game"
        continue
    fi

    tmp_recomp="/tmp/bench_recomp_$game.txt"
    tmp_interp="/tmp/bench_interp_$game.txt"

    case "$MODE" in
        both)
            printf "[running] %s (recomp)...\n" "$game" >&2
            GAME=$game "$bin" --headless --playback "$fm2" --dump-frames "$tmp_recomp" 2>/dev/null || true
            printf "[running] %s (interp)...\n" "$game" >&2
            GAME=$game "$bin" --headless --interp --playback "$fm2" --dump-frames "$tmp_interp" 2>/dev/null || true
            r=$(compare_hashes "$tmp_recomp" "$ref")
            i=$(compare_hashes "$tmp_interp" "$ref")
            printf "%-14s  %-30s  %-30s\n" "$game" "$r" "$i"
            ;;
        recomp)
            printf "[running] %s (recomp)...\n" "$game" >&2
            GAME=$game "$bin" --headless --playback "$fm2" --dump-frames "$tmp_recomp" 2>/dev/null || true
            r=$(compare_hashes "$tmp_recomp" "$ref")
            printf "%-14s  %-30s\n" "$game" "$r"
            ;;
        interp)
            printf "[running] %s (interp)...\n" "$game" >&2
            GAME=$game "$bin" --headless --interp --playback "$fm2" --dump-frames "$tmp_interp" 2>/dev/null || true
            i=$(compare_hashes "$tmp_interp" "$ref")
            printf "%-14s  %-30s\n" "$game" "$i"
            ;;
    esac
done
