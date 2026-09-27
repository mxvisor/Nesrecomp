#!/usr/bin/env bash
# ci_smoke.sh — build + run a synthetic ROM through the whole pipeline.
#
# No commercial ROM needed: tests/make_smoke_rom.py writes a tiny NROM program.
# Checks, for a GPL-free tree (no nogpl/):
#   1. full pipeline (embed -> parse_asm -> discover -> compile) builds
#   2. the binary runs headless in recompiled, --interp and --interp=fceux
#      modes and --dump-sync emits one line per frame with changing RAM
#   3. the INTERP=1 build works
#   4. --interp=fceux_vendor is refused (vendor oracle not compiled in)
#
# Usage: tools/ci_smoke.sh            (needs gcc, python3, SDL2 dev headers)
set -euo pipefail
cd "$(dirname "$0")/.."

GAME=CiSmoke
ROM=rom/$GAME.nes
FRAMES=120
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -d nogpl ] && fail "nogpl/ present — CI must build the GPL-free tree"

python3 tests/make_smoke_rom.py "$ROM"
rm -f "cfg/$GAME.cfg"                 # headless learning mode writes here

check_run() {   # check_run BIN LABEL [runner flags...]
    local bin=$1 label=$2; shift 2
    local dump="$OUT/$label.sync"
    timeout 120 "$bin" --headless --frames "$FRAMES" --dump-sync "$dump" "$@" \
        > "$OUT/$label.log" 2>&1 || { rc=$?; cat "$OUT/$label.log"; fail "$label: exit $rc"; }
    local lines hashes
    lines=$(wc -l < "$dump")
    hashes=$(awk '{print $4}' "$dump" | sort -u | wc -l)
    [ "$lines" -eq "$FRAMES" ] || fail "$label: $lines sync lines, expected $FRAMES"
    [ "$hashes" -gt 1 ] || fail "$label: RAM hash never changes — CPU not running?"
    echo "ok   $label  ($lines frames, $hashes distinct RAM hashes)"
}

echo "== full pipeline build =="
make GAME=$GAME ROM=$ROM
check_run "bin/$GAME" recompiled
check_run "bin/$GAME" interp-beam  --interp
check_run "bin/$GAME" interp-fceux --interp=fceux

echo "== INTERP=1 build =="
make GAME=${GAME}Interp ROM=$ROM INTERP=1
check_run "bin/${GAME}Interp" interp1-beam  --interp
check_run "bin/${GAME}Interp" interp1-fceux --interp=fceux

echo "== GPL-free: vendor oracle must be unavailable =="
if "bin/$GAME" --headless --frames 1 --interp=fceux_vendor > "$OUT/vendor.log" 2>&1; then
    fail "--interp=fceux_vendor accepted in a build without nogpl/"
fi
grep -q "built without the GPL FCEUX vendor" "$OUT/vendor.log" \
    || { cat "$OUT/vendor.log"; fail "unexpected --interp=fceux_vendor message"; }
echo "ok   fceux_vendor refused"

echo "smoke: all checks passed"
