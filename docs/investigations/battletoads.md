# Battletoads (AxROM, mapper 7) — investigation log

> Moved from `AGENTS.md`. Chronological; later sections supersede earlier ones. Current status: [`../STATUS.md`](../STATUS.md).

## Early history (AxROM status, 2026-06)

**Startup + copy protection: FIXED** by the unified FM2 timing (see
"Unified FM2 timing + ppudead=1" in [`timing-fixes.md`](timing-fixes.md)). The new FM2 (US Battletoads,
MD5-verified, plays in FCEUX) used to desync at frame 4; now it stays in
lag-sync (drift +1) through the whole copy-protection relay and level-1
start — **~5592 frames**.

How it was found: a bank-switch write trace (ours vs an FCEUX
`memory.registerwrite` Lua trace) showed the copy-protection relay is
**bit-identical** to FCEUX (same $FFB3/$FFB9/$FFC9/$FFB4 writes/values/
order); the only difference was a one-frame shift from the FM2 frame-1
reset being applied a frame late. Disassembly: reset $FFF2 = `LDA #0;
STA $FFB3; JMP $82A9`, then a two-VBL wait (`$82BA: LDA $2002; BPL`).
The unified record-N-at-frame-start model removed the extra boot
iteration and the relay now lines up. (Ruled out along the way:
illegal-op cycles, soft-reset completeness, blanket pre-load — which
broke Zelda.)

**Remaining: gameplay desync at ~frame 5592 — ROOT-CAUSED (2026-06-18).**
The 5592 level-load VBL spin is only where the slip *surfaces*; the real
cause is an accumulating drift that starts at **gameplay frame ~1680** and
grows a steady **+1 `$2002`-read/frame**. Method: per-frame `$2002`-read
count, ours vs an FCEUX `memory.registerread` Lua trace, frames 1-5600.
The cumulative diff is **dead flat (≈0) for frames 1-1680** (we are
cycle-exact through boot, title, copy protection), then **linear +1.0/frame**
to 5592 (~+3900 reads). Our instr-cycles/frame (29773.67) and NMI/frame (1)
are **correct** — it is *not* a cycle/time deficit.

The +1 read/frame is a **sprite-0-hit poll off-by-one**. Battletoads'
gameplay NMI handler busy-waits on sprite-0 hit:
`$854B: BIT $2002; $854E: BEQ $854B` (preceded by `LDA #$40` = bit6;
also $862E/$863E). FCEUX old-PPU **catches the PPU up to the exact CPU
cycle on every `$2002` read** (`FCEUPPU_LineUpdate`;
`lastpixel=(timestamp*48-linestartts)>>4`, `CheckSpriteHit` fires when
beam>`sphitx`+16). OUR PPU steps in **bulk after each instruction**, so a
`$2002` read observes a PPU lagging by up to ~(cycles×3) dots → the tight
poll loops **one extra time** before our bit6 sets → +1 read/frame →
accumulates to a one-frame slip by the 5592 level load.

Why the 4 bit-exact games (Mario/Battlecity/Felix/Zelda, all NewPPU-0) are
unaffected: VBL (bit7) is a sticky scanline-boundary event (±1 dot doesn't
change the poll count), and Mario's sprite-0 hit lands with slack before
its poll starts (first `BIT` already sees it set). Only a *tight* sprite-0
poll, as in Battletoads, exposes the lag.

**Fix attempted (2026-06-18): FCEUX-style PPU catch-up before a PPU register
read (interp mode).** Implemented in `src/`: `cpu_base_cycles[256]`
(cpu_interp.c), `g_ppu_catchup_dots`/`g_ppu_caught_up` armed per top-level
interp instruction (runner.c), and `ppu_read()` steps the PPU by
`base_cycles*3` dots at the read so a `$2002` poll is observed at the read's
cycle (not a whole instruction behind); runner_run steps only the remainder.
Plus an NMI/`$2002` race-suppression window at scanline 241. Gated only to
interp (dispatch sets `g_ppu_catchup_dots=0`).

Result: **all bit-exact games stay bit-exact** (Mario/Battlecity/Felix/Zelda
100% drift 0; Adventure 100% drift −4) — zero regressions. It **cut
Battletoads' `$2002`-read drift ~64%** (cum@5600 +3354 → +1213; slope
+1.0→+0.39/frame) but did **not** move the 5580 level-load slip: the residual
is the **sprite-0-hit visibility timing**. FCEUX old-PPU exposes sprite-0 hit
via its *lazy* renderer (`lastpixel` derived from the CPU timestamp runs ahead
of the real beam; `CheckSpriteHit` fires when `lastpixel>sphitx+16`), which is
fundamentally different from our beam-accurate per-dot PPU. Empirically,
delaying our hit only worsens the drift and firing earlier than the overlap
pixel is non-physical — so closing the last 36% needs replicating FCEUX's
lazy lastpixel model (large architectural change, on an approximate NewPPU-0
reference). The catch-up is kept as a genuine correctness improvement. Ruled
out: DMC steal, illegal-op cycles, per-frame cycle deficit, catch-up *amount*
(non-monotonic/overfit), uniform sprite-0 dot offset.

**Optional future task:** add a selectable **FCEUX-faithful interpreter mode**
(`--interp=fceux`, extensible to `--interp=accurate|fast`) that replicates
FCEUX's lazy renderer (`lastpixel`/`CheckSpriteHit lastpixel>sphitx+16`) so
frame-perfect demos play back exactly like FCEUX (Battletoads past ~5580) for
fuller address collection. Keep the beam-accurate `--interp` as default; the
register catch-up above is the foundation, the lazy sprite-0 visibility is the
remaining (architectural) piece. Verify with `tools/verify_all.sh`.

**IMPLEMENTED (2026-06-21): `--interp=fceux` chunk-driven backend.** A separate
headless playback backend (gated by `g_ppu_backend`; default beam path + the
recompiler stay byte-identical). `runner_run_fceux()` (src/runner.c) drives the
CPU in FCEUX `DoLine` per-scanline dot chunks (`fceux_run_to`, dots; 3 dots = 1
CPU cyc, fractional remainder carried across frames via `g_fceux_dot -=
frame_dots`); rendering is lazy in src/ppu.c (`fceux_line_begin/line_end/
prerender/on_2002_read`): BG opacity computed once per visible line, sprite-0 hit
checked only at `$2002` reads (lastpixel) and line end (EndRL CheckSpriteHit(272))
— exactly FCEUX. Also matched FCEUX's VBL→`X6502_Run(12)`→TriggerNMI 12-dot NMI
delay and the unconditional odd-frame dot skip (`kook`, not rendering-gated).
Result: Battletoads `--interp=fceux` drift **+69199 → +19217 (−72%)**, lagMatch
11.3% → 69.5%; Mario `--interp=fceux` 100%/drift 0.

**MMC3 scanline IRQ added to the fceux backend (2026-06-21).** `runner_run_fceux`
fires `mapper_scanline()` per visible scanline at dot 266 (FCEUX `DoLine`
GameHBIRQHook = X6502_Run(256)+6+4) when rendering and `(PPU[0]&0x38)!=0x18`.
Brings **Felix `--interp=fceux` 99.3% → 100%/drift 0** (bit-exact) and
**Contraf 1stSustDiv 10581 → None** (Mermaid/Superc fceux drift also improved).

**MMC5 added to the fceux backend (2026-06-22).** The chunk loop now manages the
MMC5 in-frame state itself (beam does it in `ppu_step`, uncalled here): set
`m5_in_frame=1`/`m5_scanline=0` before the visible scanlines, clock
`mapper_scanline()` once per rendered visible line (beam dot 260, no PPUCTRL gate
— distinct from the MMC3 dot-266 GameHBIRQHook path), and reset `m5_in_frame=0`
at VBL. `fceux_render_bg_opacity` sets `m5_bg_chr=1` so MMC5 BG CHR banking is
correct. **Castle3 `--interp=fceux` 2.9% → 99.7%/None** (matches beam 99.7%); no
regression (Felix/Mario/Captain/Contraf fceux unchanged, Castle3 beam unchanged).
The scanline-IRQ clock in `runner_run_fceux` is now mapper-aware (id==4 MMC3 vs
id==5 MMC5).

**Battletoads residual ROOT-CAUSED (2026-06-21) — copy-protection NMI cycle
precision.** Traced byte-by-byte vs FCEUX RAM dumps: the only persistent RAM
divergence is a 3-byte RNG `$25-$27` (generator `LDA$25;EOR$28;ADC$27;ROL;SBC$28;
…` at `$8743`/`$DAD1`) that diverges at **exactly frame 15** and feeds gameplay
~5000 frames later → the level-load desync. Frame 15 is the first frame of a
**wait-for-NMI RNG churn loop** (`$871F: JSR $8728; JSR $8743; JMP $871F`, ~562
cyc/iter, spins advancing the RNG until NMI). FCEUX runs **52** iterations there,
we run **53** — one extra. The extra iteration comes from a **timing-gated
`$2007` VRAM upload** (`$8150-$81A6`, rendering off during the load) being
interrupted by the NMI at a slightly different point: our per-frame upload split
differs ±4-13 bytes (frame 11 −4, 13 +1, 14 +9, 15 −13), shifting the upload→RNG
handoff by one iteration. The NMI handler is the **bank-switching copy-protection
relay** — the NMI *vector itself is bank-dependent* (`$FF98` only at frame 15;
other frames enter a different bank's handler), so its duration varies per frame.
Verified NOT the cause: `cpu_base_cycles` is byte-identical to FCEUX `CycTable`;
per-frame totals match (~29780.5 cyc); the 12-dot NMI delay and odd-frame skip
are now exact. Remaining gap = sub-frame cycle-exactness of the copy-protection
bank-switching NMI handler (deliberately intricate anti-piracy; likely at/beyond
the NewPPU-0 approximation limit). Next: per-instruction dual cycle trace of the
`$FF98` handler across bank switches (frames 11-15).

**Earlier title-screen fixes (historical):** the title was previously
stuck; two root fixes were applied:

**Fix 1 — STP dispatch (`tools/nesrecomp.py`):**
When a bank-switch write happens mid-function (e.g. SLO izx in `func_b5_D2B5` writes to ROM),
the real CPU continues execution at the same PC in the *new* bank's code. Our STP handler now
emits `cpu.PC = 0x{addr:04X};` before return so the dispatch loop fetches that address in the
new bank, rather than re-calling the original function entry.

**Fix 2 — Illegal opcodes (`src/cpu_interp.c`):**
ISB ($FF/$FB/$EF/$F7/$F3/$E7/$E3), SLO ($1F/$1B/$0F/$13/$17/$07/$03), and RRA ($7F/$7B/$6F/
$73/$77/$67/$63) were missing from the interpreter. They were treated as 1-byte unknowns,
causing stuck loops (especially ISB abs,X $FF $FF $FF — all-$FF open-bus regions).

**Copy-protection relay (for reference):**
Battletoads uses a multi-stage copy-protection relay: BRK → bank3 IRQ ($FF46) → 13× ISB abs,X
at $FFFF+X (each increments the current bank's IRQ-vector hi byte, triggering a bank switch to
bank0 or bank7) → bank5 ($D2B5) → bank4 ($D2CC) → bank1 ($D2D7) → RTI → BRK cascade in bank1
→ bank0. The relay depends on cpu.X loaded from RAM via LAX izy($FF).

**Remaining known issues (low priority — game runs):**
- Some debug `fprintf(stderr,...)` traces left in `generated/Battletoads_full.c`; strip before
  shipping (they are in the auto-generated file so will disappear on next `make discover`).
- Sprite-0 Y position: OAM[0]Y=$F8 (off-screen) on title — split-screen effects may be off.

## Battletoads SOLVED — lazy line rendering, and all nine LineUpdate triggers (2026-07-21)

**`our fceux` Battletoads: 11.6% / +68944 / DESYNC@5960 → 100.0% / +0 / ok.**
No lag mismatch anywhere in the 78 031-frame demo. Three changes, and they only
work as a set — each is a no-op without the others.

**1. `fceux_refresh_line()` — render the line lazily and incrementally.** FCEUX
never draws a scanline up front. `ResetRL` only blanks the buffer and rewinds the
cursor; `RefreshLine(lastpixel)` then draws tile columns `[firsttile, lasttile)`
whenever the CPU touches the PPU, and `EndRL` finishes whatever is left at
`lastpixel = 272`. Key details, all load-bearing:
- `lasttile = lastpixel >> 3`, capped at 34; `numtiles <= 0` returns early.
- Pixels are emitted only for `X1 >= 2` — the two-tile fetch delay. At a given
  `lastpixel` only pixels below `(lasttile-2)*8` exist, which is exactly where
  `CheckSpriteHit`'s `-16` comes from.
- `RefreshAddr` (our `ppu.v_addr`) is advanced by the loop **and written back**.
  Rendering from a local copy of `v` leaves it stale for a mid-line `$2007`.
- `!ScreenON && !SpriteON` → backdrop and **no** address advance; `!ScreenON` with
  sprites on → fetches still happen (address advances), pixels are backdrop.

**2. The `tofix` latch (previously judged neutral, and that judgement was wrong).**
`Fixit1` (inc_vert) fires inside the *first* `RefreshLine` with
`lastpixel >= TOFIXNUM (268)`, not at `EndRL` — so the line's last tile is fetched
with `fine_y` already advanced. In the old whole-line renderer one `fine_y` covered
the entire scanline, so moving `Fixit1` **could not change a single pixel**; it
measured neutral because it was unobservable, not because it was wrong. Tile 33
paints screen x 248..255 — which is precisely the "254 of 256 pixels match, x=254
and x=255 differ" symptom recorded in [`fceux-backend.md`](fceux-backend.md).

**3. Hook ALL nine `FCEUPPU_LineUpdate()` triggers — this was the actual bug.**
We hooked only the `$2002` **read**. FCEUX calls `LineUpdate` on reads of `$2002`,
`$2004`, `$2007` and the open-bus default (i.e. *every* `$200x` read), and on writes
of `$2000`, `$2001`, `$2005`, `$2006`, `$2007` (**not** `$2002`/`$2003`/`$2004`).
That is what makes a mid-line register write a raster split instead of a retroactive
repaint. Measured on frame 5905 line 14: the vendor issued three `RefreshLine` calls
(driven by `$2007` writes), we issued one, and our `v_addr` ended the line at `0260`
against the vendor's `0110`.

**Lesson worth more than the fix:** a fidelity change that measures neutral may be
*unobservable in the current structure* rather than refuted. Both #2 and the
incremental render scored bit-identical on their own. Re-test shelved "neutral"
changes after any structural change to the thing they depend on.

