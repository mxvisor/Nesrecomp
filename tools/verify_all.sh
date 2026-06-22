#!/usr/bin/env bash
# verify_all.sh — corpus lag/RAM divergence check (single full-featured tool).
#
# For each game:
#   1. ROM check: the FM2's romChecksum must match rom/GAME.nes (PRG+CHR, i.e.
#      the ROM minus its 16-byte iNES header / 512-byte trainer). On mismatch the
#      demo is for a different ROM → skipped (running it would desync from frame 1).
#   2. Our side: run the FM2 through BOTH backends —
#        --interp        (beam-accurate)  → lags/GAME.ours.txt
#        --interp=fceux  (FCEUX-faithful) → lags/GAME.ours_fceux.txt
#   3. FCEUX reference (lags/GAME.fceux.txt): regenerated via FCEUX when MISSING
#      or when the FM2 changed (tracked by lags/GAME.fm2.md5); otherwise reused
#      without launching FCEUX. So replacing a demo is picked up automatically.
#   4. Compare BOTH backends against the real-FCEUX reference and print a summary
#      table (one block per backend); for a single game also a detail block.
#
# Usage:
#   tools/verify_all.sh                 # every game with fm2/GAME.fm2
#   tools/verify_all.sh Contraf         # one game (+ detail block)
#   REBUILD=0 tools/verify_all.sh       # skip `make` (use existing bin/GAME)
#   FCEUX=0   tools/verify_all.sh       # never launch FCEUX (cached refs only)
#   DETAIL=1  tools/verify_all.sh ...   # force per-game detail block
#
# Columns (per backend): lag%=frame-to-frame lag agreement vs FCEUX; drift=
# cumulative (ours-fceux) lag; Sust=first frame of a sustained desync (>150/200),
# None=plays through. The real-FCEUX side is the reference (100%/0/None), so it is
# the baseline, not a column.
set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
REBUILD="${REBUILD:-1}"
FCEUX="${FCEUX:-1}"
DETAIL="${DETAIL:-}"
LAGDIR="lags"; mkdir -p "$LAGDIR"

if [ "$#" -gt 0 ]; then
    GAMES=("$@")
else
    GAMES=(); for f in fm2/*.fm2; do [ -e "$f" ] && GAMES+=("$(basename "$f" .fm2)"); done
fi
[ "${#GAMES[@]}" -eq 1 ] && [ -z "$DETAIL" ] && DETAIL=1

# base64(md5(ROM payload)) — payload = file minus 16B iNES header (+512B trainer)
rom_checksum() {
    local rom="$1" hdr=16 f6
    f6=$(dd if="$rom" bs=1 skip=6 count=1 2>/dev/null | od -An -tu1 | tr -d ' ')
    [ -n "${f6:-}" ] && [ $((f6 & 4)) -ne 0 ] && hdr=528
    tail -c +$((hdr + 1)) "$rom" | openssl dgst -md5 -binary | base64
}
fm2_checksum() { awk -F'base64:' '/^romChecksum/{print $2; exit}' "$1"; }

rows=(); details=()

for g in "${GAMES[@]}"; do
    printf '[verify] %-12s ' "$g" >&2
    fm2="fm2/$g.fm2"; rom="rom/$g.nes"
    ref="$LAGDIR/$g.fceux.txt"; ours="$LAGDIR/$g.ours.txt"
    ours_f="$LAGDIR/$g.ours_fceux.txt"; md5f="$LAGDIR/$g.fm2.md5"
    [ -e "$fm2" ] || { rows+=("$(printf '%-13s  (no %s)' "$g" "$fm2")"); echo skip >&2; continue; }
    max="$(grep -c '^|' "$fm2")"

    # ---- 1. ROM suitability ----
    if [ -e "$rom" ]; then
        want="$(fm2_checksum "$fm2")"; have="$(rom_checksum "$rom")"
        if [ -n "$want" ] && [ "$want" != "$have" ]; then
            rows+=("$(printf '%-13s  ROM MISMATCH (fm2=%s rom=%s)' "$g" "$want" "$have")")
            echo "rom-mismatch" >&2; continue
        fi
    fi

    # ---- 2. build + run our side ----
    if [ "$REBUILD" = 1 ]; then
        printf 'build ' >&2
        if ! make GAME="$g" INTERP=1 -s > "/tmp/verify_$g.build" 2>&1; then
            rows+=("$(printf '%-13s  BUILD FAILED (/tmp/verify_%s.build)' "$g" "$g")"); echo FAIL >&2; continue
        fi
    fi
    [ -x "bin/$g" ] || { rows+=("$(printf '%-13s  (no bin/%s)' "$g" "$g")"); echo skip >&2; continue; }
    printf 'run(beam) ' >&2
    ./bin/"$g" --headless --interp        --playback "$fm2" --frames "$max" --dump-sync "$ours"   >/dev/null 2>&1
    printf 'run(fceux) ' >&2
    ./bin/"$g" --headless --interp=fceux  --playback "$fm2" --frames "$max" --dump-sync "$ours_f" >/dev/null 2>&1

    # ---- 3. FCEUX reference: regenerate if missing or FM2 changed (md5 sidecar) ----
    cur="$(md5sum "$fm2" | cut -d' ' -f1)"
    need=0
    [ -e "$ref" ] || need=1                                    # no ref -> generate
    [ -e "$md5f" ] && [ "$(cat "$md5f")" != "$cur" ] && need=1 # fm2 changed -> regenerate
    # (ref present but no md5 sidecar -> adopt: trust the ref, just record md5 below)
    if [ "$need" = 1 ] && [ "$FCEUX" = 1 ] && [ -e "$rom" ] && command -v fceux >/dev/null 2>&1; then
        printf 'fceux ' >&2
        lua="$(mktemp /tmp/fceux_dump.XXXX.lua)"
        sed -e "s|local OUT = .*|local OUT = \"${ROOT}/${ref}\"|" \
            -e "s|local MAX_FRAMES = .*|local MAX_FRAMES = ${max}|" \
            tools/fceux_dump.lua > "$lua"
        # FCEUX SIGSEGVs on shutdown after os.exit(0) — harmless, dump already
        # written; `; true` keeps the subshell exit 0 so no signal line leaks.
        ( ulimit -c 0; fceux --playmov "$fm2" --loadlua "$lua" "$rom" >/dev/null 2>&1; true ) 2>/dev/null
        rm -f "$lua"
    fi
    if [ ! -e "$ref" ]; then
        rows+=("$(printf '%-13s  (no FCEUX ref; FCEUX=%s rom=%s)' "$g" "$FCEUX" "$([ -e "$rom" ] && echo yes || echo MISSING)")")
        echo no-ref >&2; continue
    fi
    echo "$cur" > "$md5f"   # ref valid for this fm2 (just generated, or adopted)

    # ---- 4. compare both backends vs the FCEUX reference ----
    row="$(GAME="$g" BEAM="$ours" FCX="$ours_f" REF="$ref" python3 - <<'PY'
import os
def load(p):
    L=[]
    try: fh=open(p)
    except OSError: return L
    for ln in fh:
        q=ln.split()
        if len(q)>=4: L.append((int(q[1]), q[3].upper()))
    return L
def stats(o,f):
    n=min(len(o),len(f))
    if n==0: return None
    match=sum(1 for i in range(n) if o[i][0]==f[i][0])
    drift=sum(o[i][0]-f[i][0] for i in range(n))
    sust=None
    for i in range(n-200):
        if sum(1 for j in range(i,i+200) if o[j][0]!=f[j][0])>150:
            sust=i+1; break
    return (n, 100.0*match/n, drift, sust)
g=os.environ["GAME"]
f=load(os.environ["REF"]); b=load(os.environ["BEAM"]); x=load(os.environ["FCX"])
sb=stats(b,f); sx=stats(x,f)
if sb is None and sx is None:
    print(f"{g:<12} (empty)"); raise SystemExit
n = (sb or sx)[0]
def fmt(s):
    if s is None: return f"{'--':>6} {'--':>8} {'--':>6}"
    _,lm,d,su = s
    return f"{lm:5.1f}% {d:+8d} {str(su):>6}"
print(f"{g:<12} {n:7d}  | {fmt(sb)} | {fmt(sx)}")
PY
)"
    rows+=("$row"); echo ok >&2

    if [ "$DETAIL" = 1 ]; then
        det="$(GAME="$g" BEAM="$ours" FCX="$ours_f" REF="$ref" python3 - <<'PY'
import os
def load(p):
    L=[]
    try: fh=open(p)
    except OSError: return L
    for ln in fh:
        q=ln.split()
        if len(q)>=4: L.append((int(q[1]), q[3].upper()))
    return L
g=os.environ["GAME"]; f=load(os.environ["REF"])
print(f"\n--- {g}: detail (vs real FCEUX) ---")
for label,p in (("our beam",os.environ["BEAM"]),("our fceux",os.environ["FCX"])):
    o=load(p); n=min(len(o),len(f))
    if n==0: print(f"  {label}: (no data)"); continue
    first=next((i for i in range(n) if o[i][0]!=f[i][0]), None)
    rm=sum(1 for i in range(n) if o[i][1]==f[i][1])
    if first is None:
        print(f"  {label}: lag matches end-to-end; RAM djb2 {rm}/{n} ({100*rm/n:.2f}%)")
    else:
        print(f"  {label}: first lag divergence at frame {first+1}; RAM djb2 {100*rm/n:.2f}%")
PY
)"
        details+=("$det")
    fi
done

# ---- summary table ----
# Both our backends compared against the real FCEUX reference (the baseline, so
# real-fceux is 100%/0/None by definition and not shown as a column).
# Each backend block: lagMatch% | drift (ours-fceux) | 1stSustDiv (None=plays through).
echo
printf '%-12s %7s  | %-22s | %-22s\n' GAME n 'our beam (vs fceux)' 'our fceux (vs fceux)'
printf '%-12s %7s  | %6s %8s %6s | %6s %8s %6s\n' '' '' lag% drift Sust lag% drift Sust
printf '%.0s-' {1..70}; echo
for r in "${rows[@]}"; do echo "$r"; done
for d in "${details[@]+"${details[@]}"}"; do echo "$d"; done
