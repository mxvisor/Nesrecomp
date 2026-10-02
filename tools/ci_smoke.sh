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
#   5. learn mode: --interp=fceux appends executed jump targets to the cfg
#      without clobbering other directives; on UNROM the switchable-bank target
#      is written bank-qualified (N:XXXX) and a rebuild recompiles it for that
#      bank only, after which the dispatch build learns nothing new
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
rm -f cfg/${GAME}*.cfg                # headless learning mode writes here

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

echo "== learn mode (--interp=fceux) =="
learned() {     # learned BIN [runner flags...]  -> prints the "N new" count
    local bin=$1; shift
    "$bin" --headless --frames 60 "$@" 2>&1 | sed -n 's/^\[learn\] \([0-9]*\) new.*/\1/p'
}
cfg=cfg/${GAME}Interp.cfg
printf 'data_region = 9000,90FF\n' > "$cfg"
n=$(learned "bin/${GAME}Interp" --interp=fceux)
grep -qx 'extra_func = 800A' "$cfg"     || { cat "$cfg"; fail "JMP target \$800A not learned"; }
grep -qx 'data_region = 9000,90FF' "$cfg" || { cat "$cfg"; fail "learn mode clobbered data_region"; }
[ "$(learned "bin/${GAME}Interp" --interp=fceux)" = 0 ] || fail "second learn run added addresses"
echo "ok   learn NROM ($n new, cfg preserved, idempotent)"

U=${GAME}Unrom
python3 tests/make_smoke_rom.py rom/$U.nes --unrom
make GAME=${U}Interp ROM=rom/$U.nes INTERP=1
learned "bin/${U}Interp" --interp=fceux > /dev/null
grep -qx 'extra_func = 1:9000' cfg/${U}Interp.cfg || { cat cfg/${U}Interp.cfg; fail "UNROM target not learned as 1:9000"; }
cp cfg/${U}Interp.cfg cfg/$U.cfg
make GAME=$U ROM=rom/$U.nes
grep -q 'case 1: func_b1_9000(); return;' generated/${U}_dispatch.c || fail "learned 1:9000 not recompiled"
grep -q 'func_b[02]_9000' generated/${U}_dispatch.c && fail "1:9000 leaked into other banks"
[ "$(learned "bin/$U")" = 0 ] || fail "dispatch build still misses after relearn"
echo "ok   learn UNROM (1:9000 -> func_b1_9000, no misses after rebuild)"

echo "smoke: all checks passed"
