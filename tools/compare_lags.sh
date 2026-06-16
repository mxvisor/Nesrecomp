#!/usr/bin/env bash
#
# compare_lags.sh — compare lag sequences (primary demo-sync metric) between
# our interpreter and the FCEUX reference, per the methodology in AGENTS.md
# ("Synchronization Methodology"). Lags are the primary metric, RAM hash the
# secondary; the framebuffer is NOT compared here.
#
# Dump format (both sides), one line per emulated frame:
#     frame  lag(0/1)  lagcount  djb2(RAM $0000-$07FF)
#
# Usage:
#     tools/compare_lags.sh GAME [MAX_FRAMES]
#
#   GAME        base name, e.g. Battlecity (uses rom/GAME.nes, fm2/GAME.fm2)
#   MAX_FRAMES  optional frame cap (0 / omitted = whole movie)
#
# Outputs (kept for inspection):
#     lags/GAME.fceux.txt   FCEUX reference (emu.lagged + RAM djb2)
#     lags/GAME.ours.txt    our interpreter (--dump-sync)
#
# Exit status: 0 if lag sequences match end-to-end, 1 otherwise.

set -u

GAME="${1:-}"
MAX_FRAMES="${2:-0}"

if [ -z "$GAME" ]; then
    echo "usage: $0 GAME [MAX_FRAMES]" >&2
    exit 2
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ROM="rom/${GAME}.nes"
FM2="fm2/${GAME}.fm2"
BIN="bin/${GAME}"
LAGDIR="lags"
OURS="${LAGDIR}/${GAME}.ours.txt"
FCEUX_OUT="${LAGDIR}/${GAME}.fceux.txt"

mkdir -p "$LAGDIR"

for f in "$ROM" "$FM2"; do
    [ -f "$f" ] || { echo "missing: $f" >&2; exit 2; }
done
[ -x "$BIN" ] || { echo "missing binary $BIN — run: make GAME=$GAME" >&2; exit 2; }

# MAX_FRAMES=0 means "whole movie". FCEUX does not reliably stop at movie end
# (it keeps emulating idle frames past it), which pollutes offset detection.
# So always cap both sides at the exact FM2 input-frame count.
if [ "$MAX_FRAMES" = "0" ]; then
    MAX_FRAMES="$(grep -c '^|' "$FM2")"
    echo "[compare_lags] whole movie: capping both sides at $MAX_FRAMES frames"
fi

# ---- our side --------------------------------------------------------------
echo "[compare_lags] our interpreter → $OURS"
OUR_ARGS=(--headless --interp --playback "$FM2" --dump-sync "$OURS")
if [ "$MAX_FRAMES" != "0" ]; then
    OUR_ARGS+=(--frames "$MAX_FRAMES")
fi
"./$BIN" "${OUR_ARGS[@]}" >/dev/null 2>&1

# ---- FCEUX side ------------------------------------------------------------
if command -v fceux >/dev/null 2>&1; then
    echo "[compare_lags] FCEUX reference → $FCEUX_OUT"
    LUA="$(mktemp /tmp/fceux_dump.XXXX.lua)"
    sed -e "s|local OUT = .*|local OUT = \"${ROOT}/${FCEUX_OUT}\"|" \
        -e "s|local MAX_FRAMES = .*|local MAX_FRAMES = ${MAX_FRAMES}|" \
        tools/fceux_dump.lua > "$LUA"
    # os.exit(0) from Lua makes FCEUX 2.6.6 dump core on shutdown AFTER the file
    # is written and closed — harmless. Suppress the core file.
    ( ulimit -c 0; fceux --playmov "$FM2" --loadlua "$LUA" "$ROM" ) >/dev/null 2>&1
    rm -f "$LUA"
else
    echo "[compare_lags] fceux not found — skipping reference capture." >&2
    echo "[compare_lags] (re)using existing $FCEUX_OUT if present." >&2
fi

[ -f "$OURS" ]      || { echo "no output: $OURS" >&2; exit 2; }
[ -f "$FCEUX_OUT" ] || { echo "no FCEUX reference: $FCEUX_OUT" >&2; exit 2; }

# ---- compare ---------------------------------------------------------------
python3 - "$OURS" "$FCEUX_OUT" "$GAME" <<'PY'
import sys

ours_path, ref_path, game = sys.argv[1], sys.argv[2], sys.argv[3]

def load(path):
    lag, ram = [], []
    with open(path) as fh:
        for line in fh:
            p = line.split()
            if len(p) < 4:
                continue
            lag.append(int(p[1]))
            ram.append(p[3].upper())
    return lag, ram

o_lag, o_ram = load(ours_path)
f_lag, f_ram = load(ref_path)

print(f"\n=== {game}: lag-sequence comparison ===")
print(f"frames: ours={len(o_lag)} fceux={len(f_lag)}")

if not o_lag or not f_lag:
    print("no data on one side"); sys.exit(1)

n = min(len(o_lag), len(f_lag))

# PRIMARY METRIC: frame-to-frame, NO offset fitting. Both emulators play the
# same FM2 (one record per emulated frame), so frame i must equal frame i. The
# first lag divergence is THE frame to debug (AGENTS.md "Mandatory diagnostic
# order"). Fitting a constant offset is deliberately avoided here: it hides real
# transient divergences by sliding past them.
lag_match = sum(1 for i in range(n) if o_lag[i] == f_lag[i])
divs  = [i for i in range(n) if o_lag[i] != f_lag[i]]
first = divs[0] if divs else None

print(f"\nLAG match (frame-to-frame): {lag_match}/{n} ({100*lag_match/n:.3f}%)")
print(f"LAG divergent frames: {len(divs)}")

# Cumulative lag totals are the "did FM2 input shift" signal: if they stay equal,
# input alignment is preserved even across isolated transient flips.
oc, fc = sum(o_lag[:n]), sum(f_lag[:n])
print(f"cumulative lag frames: ours={oc} fceux={fc} (drift={oc-fc:+d})")

if first is None:
    print("\nLAG sequences MATCH frame-to-frame end-to-end ✓")
    rmatch = sum(1 for i in range(n) if o_ram[i] == f_ram[i])
    print(f"RAM match: {rmatch}/{n} ({100*rmatch/n:.2f}%)")
    sys.exit(0)

print(f"\n*** FIRST LAG DIVERGENCE at frame#{first+1} ***")
print("   frame  ourLag fceuxLag | ourRAM      fceuxRAM")
for i in range(max(0, first-3), min(n, first+6)):
    mark = ">>" if i == first else "  "
    print(f"{mark}{i+1:6}    {o_lag[i]}      {f_lag[i]}     | "
          f"{o_ram[i]}   {f_ram[i]}")

# Diagnostic hint only: does a small constant shift recover near-100%? If so the
# divergence is a pure startup/phase shift rather than scattered jitter.
def shifted_match(s):
    lo = max(0, -s); hi = min(len(o_lag), len(f_lag) - s)
    if hi - lo < 8: return -1.0
    return sum(1 for i in range(lo, hi) if o_lag[i] == f_lag[i+s]) / (hi - lo)
hints = sorted(((shifted_match(s), s) for s in range(-4, 5) if s != 0), reverse=True)
print(f"\n(hint) best non-zero shift: {hints[0][1]:+d} → {100*hints[0][0]:.2f}% "
      "(≈100% ⇒ constant startup shift, not jitter)")
print(f"→ debug frame#{first+1}: most likely NMI moment or CPU cycle counts "
      "(AGENTS.md 'Most likely root cause of lag divergence').")
sys.exit(1)
PY
