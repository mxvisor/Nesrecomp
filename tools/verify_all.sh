#!/usr/bin/env bash
# verify_all.sh — corpus lag/RAM divergence check (single full-featured tool).
#
# For each game:
#   1. ROM check: the FM2's romChecksum must match rom/GAME.nes (PRG+CHR, i.e.
#      the ROM minus its 16-byte iNES header / 512-byte trainer). On mismatch the
#      demo is for a different ROM → skipped (running it would desync from frame 1).
#   2. Our side: run the FM2 through the interpreter (--dump-sync → lags/GAME.ours.txt).
#   3. FCEUX reference (lags/GAME.fceux.txt): regenerated via FCEUX when MISSING
#      or when the FM2 changed (tracked by lags/GAME.fm2.md5); otherwise reused
#      without launching FCEUX. So replacing a demo is picked up automatically.
#   4. Compare and print a summary table; for a single game also a divergence
#      detail block.
#
# Usage:
#   tools/verify_all.sh                 # every game with fm2/GAME.fm2
#   tools/verify_all.sh Contraf         # one game (+ detail block)
#   REBUILD=0 tools/verify_all.sh       # skip `make` (use existing bin/GAME)
#   FCEUX=0   tools/verify_all.sh       # never launch FCEUX (cached refs only)
#   DETAIL=1  tools/verify_all.sh ...   # force per-game detail block
#
# Columns: n=frames; lagMatch%=frame-to-frame lag agreement; drift=cumulative
# (ours-fceux) lag; 1stSustDiv=first frame of a sustained desync (>150/200),
# None=plays through; 1stRAMdiv=first $0000-$07FF djb2 mismatch (phase-sensitive,
# a non-None can be a benign boot/counter offset).
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
    ref="$LAGDIR/$g.fceux.txt"; ours="$LAGDIR/$g.ours.txt"; md5f="$LAGDIR/$g.fm2.md5"
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
    printf 'run ' >&2
    ./bin/"$g" --headless --interp --playback "$fm2" --frames "$max" --dump-sync "$ours" >/dev/null 2>&1

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

    # ---- 4. compare ----
    row="$(GAME="$g" OURS="$ours" REF="$ref" python3 - <<'PY'
import os
def load(p):
    L=[]
    for ln in open(p):
        q=ln.split()
        if len(q)>=4: L.append((int(q[1]), q[3].upper()))
    return L
g=os.environ["GAME"]
o=load(os.environ["OURS"]); f=load(os.environ["REF"])
n=min(len(o),len(f))
if n==0:
    print(f"{g:<13}  (empty: ours={len(o)} ref={len(f)})"); raise SystemExit
match=sum(1 for i in range(n) if o[i][0]==f[i][0])
drift=sum(o[i][0]-f[i][0] for i in range(n))
sust=None
for i in range(n-200):
    if sum(1 for j in range(i,i+200) if o[j][0]!=f[j][0])>150:
        sust=i+1; break
rdiv=next((i+1 for i in range(n) if o[i][1]!=f[i][1]), None)
flag="" if (sust is None and 100*match/n>=99.0) else ("  *** DESYNC" if sust is not None else "  <-- check")
print(f"{g:<13} {n:8d} {100*match/n:8.1f}% {drift:+8d} {str(sust):>12} {str(rdiv):>11}{flag}")
PY
)"
    rows+=("$row"); echo ok >&2

    if [ "$DETAIL" = 1 ]; then
        det="$(GAME="$g" OURS="$ours" REF="$ref" python3 - <<'PY'
import os
def load(p):
    L=[]
    for ln in open(p):
        q=ln.split()
        if len(q)>=4: L.append((int(q[1]), q[3].upper()))
    return L
g=os.environ["GAME"]
o=load(os.environ["OURS"]); f=load(os.environ["REF"])
n=min(len(o),len(f))
first=next((i for i in range(n) if o[i][0]!=f[i][0]), None)
print(f"\n--- {g}: detail ---")
if first is None:
    print("  lag sequences match frame-to-frame end-to-end")
    rm=sum(1 for i in range(n) if o[i][1]==f[i][1])
    print(f"  RAM djb2 match: {rm}/{n} ({100*rm/n:.2f}%)")
    raise SystemExit
print(f"  first lag divergence at frame {first+1}")
print("   frame  ourLag fceuxLag   ourRAM     fceuxRAM")
for i in range(max(0,first-1), min(n, first+9)):
    mark=">>" if i==first else "  "
    print(f"  {mark}{i+1:6d}   {o[i][0]:5d} {f[i][0]:7d}   {o[i][1]} {f[i][1]}")
PY
)"
        details+=("$det")
    fi
done

# ---- summary table ----
echo
printf '%-13s %8s %9s %8s %12s %11s\n' GAME n lagMatch% drift 1stSustDiv 1stRAMdiv
printf '%.0s-' {1..72}; echo
for r in "${rows[@]}"; do echo "$r"; done
for d in "${details[@]+"${details[@]}"}"; do echo "$d"; done
