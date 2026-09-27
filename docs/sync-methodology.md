# Demo-Sync Methodology — READ BEFORE DEBUGGING DEMO DESYNC

> Moved from `AGENTS.md`. Stable reference: how to measure and debug
> demo (FM2) sync. Current per-game numbers live in [`STATUS.md`](STATUS.md);
> dated investigation logs live in [`investigations/`](investigations/).

This section supersedes ad-hoc frame-hash debugging. It encodes the
diagnostic order derived from the reference research (companion docs:
`ppu-and-fm2-playback.md`, `fceux-lua-dump.md`, `mesen-reference.md`,
`next-features.md`). Agents debugging desync MUST follow this order
instead of staring at framebuffer hashes.

## The metric problem (why current frame-hash debugging stalls)

The current `--dump-frames` hashes the **framebuffer** (PPU output).
This is the *worst* metric for demo sync, because the framebuffer is the
bottom of the dependency chain and absorbs every cosmetic PPU difference
that does NOT affect whether the demo desyncs:

```
CPU logic (what the game DECIDED)      ← "does the demo stay in sync" lives here
   ↓ RAM $0000-$07FF (positions, RNG)  ← direct consequence of logic
   ↓ PPU regs / VRAM (what was LOADED)
   ↓ Framebuffer (what was DRAWN)      ← we currently hash HERE (worst)
```

A sprite blinking one frame late, a palette emphasis shade, a 1-pixel
mid-frame scroll — all diverge the framebuffer while the game runs
perfectly. This is why most games in the accuracy table show "many PPU
differences": we measure the layer most polluted by cosmetic noise.

## Correct metrics, in priority order

1. **Lag sequence** (primary). One bit per frame: did the game read
   `$4016/$4017` this frame. Compute: `lag=true` at frame start;
   `lag=false` on controller read **by the game** (not by debug/Lua
   peeks); record bit at frame end. A lag bit divergence = the exact
   frame where input shifted = the demo's death point. Because one FM2
   record = one emulated frame, a single extra lag frame shifts all
   subsequent input by one and kills the run. This is the ONLY point
   worth debugging.
2. **RAM hash `$0000-$07FF`** (secondary). Whether game logic matches
   (positions, counters, RNG). Independent of how the PPU draws.
3. **Framebuffer** (last). Only for cosmetic verification once logic is
   provably in sync.

## Mandatory diagnostic order

1. Check the demo header `NewPPU` flag (see "Reference emulator
   caveat" below). If `0`/absent and the game is timing-sensitive,
   desync is EXPECTED on our cycle-accurate core — do not debug the
   core against this demo.
2. Compare **lag sequences** first. FCEUX side via `emu.lagged()`
   (see `fceux-lua-dump.md` for the verified Lua dump script). First
   lag divergence row = the frame to debug. Everything after it is
   downstream noise.
3. Only if lags match end-to-end: compare RAM hashes. A transient
   (diverge-one-frame-then-rejoin) RAM mismatch with matching lags is
   a hash-sample-phase artifact, NOT a desync — ignore it.
4. Framebuffer last.

## Tooling — implemented

Run the whole comparison with one command — `tools/verify_all.sh`:

```bash
tools/verify_all.sh                 # every game (builds, runs, compares)
tools/verify_all.sh Contraf         # one game + first-divergence detail block
REBUILD=0 tools/verify_all.sh       # skip make (use existing bin/GAME)
FCEUX=0   tools/verify_all.sh       # cached refs only, never launch FCEUX
```

For each game it (1) checks the FM2's romChecksum against `rom/GAME.nes`
(skips on mismatch — wrong ROM), (2) runs our interpreter
(`--dump-sync → lags/GAME.ours.txt`), (3) reuses `lags/GAME.fceux.txt`,
regenerating it via FCEUX only when missing or when the FM2 changed
(tracked by `lags/GAME.fm2.md5`), then (4) prints a summary table
(`lagMatch% drift 1stSustDiv 1stRAMdiv`); a single game also gets a
first-divergence detail block. Format of the dumps:
`frame lag lagcount djb2(RAM $0000-$07FF)`. Comparison is **frame-to-frame**
(no offset fitting — fitting an offset hides real transient divergences).

- [x] `--dump-sync out.txt`: emits `frame lag lagcount djb2(RAM
  $0000-$07FF)`. lag and RAM are captured **together** at the VBL
  boundary so they share one timing phase (an earlier version captured
  them at different boundaries, which no constant offset could align).
- [x] Lag flag plumbing: `g_lag_flag` in `memory.c` cleared ONLY by
  game-side `ctrl_read()` (`$4016/$4017`); reset each frame in the FM2
  advance. Debug peeks do not touch it.
- [x] FCEUX-side: `tools/fceux_dump.lua` (`emu.lagged()`,
  `memory.readbyterange(0,0x800)`, djb2, `emu.speedmode("nothrottle")`,
  `os.exit(0)` to stop at movie end). `verify_all.sh` bakes the OUT
  path and frame cap into a temp copy per run.

## Interpreting the result (cumulative lag drift is the verdict)

The lag sequence rarely matches 100% frame-to-frame even when a demo
plays perfectly, because borderline `wait-for-vblank` frames flip lag
one frame early/late then immediately re-sync (an NMI-moment phase
jitter). The **authoritative** "does the demo stay in sync" signal is
the **cumulative lag total**: if `ours ≈ fceux` (drift 0 or ±1), FM2
input stays aligned end-to-end and the demo plays correctly; only the
jittery frames differ. A *growing* drift = real desync — debug the
first frame where the running totals start to separate.

## FM2 compatibility — verify ROM before trusting a demo

An FM2 only plays correctly on the exact ROM it was recorded against.
The header's `romChecksum base64:...` is the **MD5 of the ROM minus the
16-byte iNES header**. Check it before any comparison:

```bash
grep '^romChecksum' fm2/GAME.fm2 | sed 's/.*base64://' | base64 -d | xxd -p
tail -c +17 rom/GAME.nes | md5sum            # must match
```

Two wrong Castle3 demos were hit this way: one for *Akumajo Densetsu (J)*
(VRC6 / mapper 24 — different game, unsupported mapper), and the old US
demo. The current `fm2/Castle3.fm2` (US Castlevania III, 367k frames)
matches `rom/Castle3.nes` (`bfc4d979…def4f`). Also confirm the demo
actually plays in FCEUX itself — a mismatch there means the FM2/ROM pair
is wrong, not our emulator.

## Interpreter-only build (`make GAME=X INTERP=1`)

For demo-sync work everything runs under `--interp`, so the recompiled
code is **never executed** (`runner.c`: `if (g_interp_mode) cpu_interp_step()
else call_by_address()`). `INTERP=1` links the tiny `src/stub_full.c`
(`call_by_address → cpu_interp_run`) instead of the generated
`_full.c`/`_dispatch.c`, and skips the `discover` step entirely (no cfg /
extra_func / bank-aware needed).

**Why it matters:** bank-aware games generate enormous `_full.c` files
(Mermaid UNROM ≈ 24500 functions); compiling that with `-O2` exhausts
RAM. `INTERP=1` drops Mermaid's build peak from OOM to ~53 MB and is
byte-identical at runtime under `--interp` (verified: Battlecity/Mermaid
lag results unchanged). **Use `INTERP=1` for all `verify_all.sh`
testing.** Editing a shared header (e.g. `apu.h`) under a normal build
forces recompiling the giant `_full.c` — another reason to use INTERP.

TODO (separate): the giant generated `_full.c` is a recompiler
scalability problem (compile time/RAM). Options: compile generated files
at `-O1`, or split into multiple translation units.

## Reference emulator caveat (resolves many "PPU bugs")

The accuracy table's "PPU/mapper bug; interp identical" rows may not be
our bugs at all — they may be **FCEUX old-PPU inaccuracies** we are
chasing:

- FCEUX default PPU is **scanline-based, not cycle-accurate**. Its NMI
  moment, sprite-0 dot, and A12 edges differ from hardware. A demo
  recorded on it is correct *relative to old PPU*, and our
  cycle-accurate core is allowed to diverge — that is not our bug.
- FCEUX **new PPU** (`NewPPU 1` in the FM2 header) is dot-level but has
  empirical constants (notably NMI fires ~20 dots into scanline 241,
  tuned to make Marble Madness work, vs hardware cycle 1). Details in
  `ppu-and-fm2-playback.md` §3.
- **Mesen 2** is the cycle-accuracy reference. For timing disputes,
  arbitrate against Mesen or a blargg/nesdev test ROM — NOT against
  FCEUX. If our core matches Mesen but not FCEUX, we are right and the
  FM2 is old-PPU-incompatible at the timing layer.
- Action: when a "PPU bug" is suspected, FIRST check `NewPPU` flag,
  THEN cross-check the disputed frame in Mesen before touching `ppu.c`.

## Most likely root cause of lag divergence (debug here first)

When lag sequences diverge, the game did a different amount of work per
frame. Ranked causes:

1. **NMI moment.** If VBlank/NMI is set on a different dot than the
   reference, the game's `wait-for-vblank` loop exits after a different
   instruction count → different per-frame budget → borderline frames
   lag differently. Causes #1. Consider a configurable `nmi_delay_dots`
   compat knob (hardware/Mesen = cycle 1; FCEUX-compat = tune to match
   lag sequence on a known demo).
2. **CPU cycle accuracy.** Unaccounted page-cross and taken-branch
   cycles (bugs 1.4/1.5 in `nesrecomp-bugs.md`) accumulate per-frame
   budget error. Cheapest to fix — do FIRST.
3. **DMC cycle stealing** if DPCM active (see `next-features.md` §6.2).

## Format strategy (FM2 vs MSM) — discovery vs verification

- **Discovery (collect addresses for recompilation): use FM2.** Reason:
  full playthroughs exist only in FM2/bk2 (TASVideos). Desync tolerance
  is high — even a desynced tail still executes real game code and
  yields valid addresses (a dynamic miss is almost never false: if the
  CPU jumped there, it is code). Tag discovery-log addresses with a
  sync marker (lag-match up to frame N) so post-desync addresses are
  known to be off-route but still valid as code.
- **Verification (prove core correctness): use Mesen.** Record short
  MSM in Mesen (cycle-accurate, hermetic settings) OR feed the same
  input table to both via Lua `setInput` in the `inputPolled` callback
  (scanline 241 — verified point). MSM playback is GUI-only; parse the
  MSM yourself (ZIP + Input.txt, button order `UDLRSsBA` — differs from
  FM2 `RLDUTSBA`). Details: `mesen-reference.md`.
- **Do NOT** rely on converting FM2→full playthrough on Mesen: timing
  layer (old PPU) makes long runs desync. Conversion is fine as an
  *input source* for short differential core checks, not as a way to
  replay a whole demo.

## Controller compatibility profile (FM2 playback)

For bit-exact FM2 replay, the controller must mimic **FCEUX, not
hardware** (verified in FCEUX source, see `fceux-fm2-playback.md` §4):
read while strobe high SHIFTS the register (hardware reloads); open bus
is `DB & 0xC0`; DPCM controller glitch is NOT emulated by FCEUX. Keep a
`controller_profile` switch: FCEUX-FM2 profile for replay, Hardware
profile (reload on strobe, console-model open-bus mask, glitch on) for
Mesen/hardware verification. Mixing profiles causes subtle desync on
games with non-standard polling.

## Entry point for desync work (external companion docs)

`ppu-and-fm2-playback.md` §6.0 (switch metric to lags+RAM) → `fceux-lua-dump.md`
(dump lag log) → find first lag divergence → almost certainly NMI moment or cycle
counts → [`nesrecomp-bugs.md`](nesrecomp-bugs.md) bugs 1.4/1.5.
