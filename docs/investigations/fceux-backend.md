# `--interp=fceux` backend — vendor oracle, phase hypothesis, post-render-first

> Moved from `AGENTS.md`. Includes REFUTED hypotheses, kept on purpose so nobody re-derives them. Design doc: [`../interp-fceux-design.md`](../interp-fceux-design.md).

## `--interp=fceux_vendor` — vendored x6502 differential oracle (2026-06-22)

> **GPL / de-vendored (2026-07-23):** the four vendored FCEUX sources
> (`x6502_vendor.c`, `x6502_ops.inc`, `ppu_vendor.c`, `apu_vendor.c`) are GPL, so
> they live in a **gitignored `nogpl/`** dir, NOT in `src/` — the tree builds &
> commits GPL-free. The Makefile auto-detects them (`VENDOR_SRCS := $(wildcard
> nogpl/…)`): present → compiled into the `INTERP=1` build with `-DHAVE_VENDOR`,
> which is what enables the vendor CPU path + the `--interp=fceux_vendor` flag in
> `runner.c` (all guarded by `#ifdef HAVE_VENDOR`). Absent → default GPL-free
> build, `--interp=fceux_vendor` prints an error and exits 1. Keep `nogpl/` locally
> as the RAM-hash oracle. Vendor line-number refs below are unchanged (same file
> contents, only the directory moved from `src/` to `nogpl/`).

To settle whether the 2 desyncs are a CPU bug, `nogpl/x6502_vendor.c` +
`nogpl/x6502_ops.inc` vendor FCEUX's **exact x6502 core** (GPL); built with
`make GAME=X INTERP=1` (with `nogpl/` present), selected by `--interp=fceux_vendor`
(same fceux PPU/loop/`--dump-sync`, only the CPU core differs). Findings:
- **Mario vendor = 100% RAMmatch / drift 0** (bit-exact) — *more* accurate than
  cpu_interp (which had a frame-43 RAM blip → a real, now-known cpu_interp bug).
- **Contraf vendor STILL desyncs identically** (drift −50740 vs cpu_interp
  −50267 — Δ473 over 170k frames). **The cycle-exact FCEUX CPU does not fix
  Contraf** ⇒ the desync is **NOT a CPU bug**; it lives in code shared by both
  backends — the **runner-loop NMI-delivery timing + ppu.c VBL/$2002 timing**
  vs FCEUX's DoLine chunking. This **overturns** the earlier "sub-cycle CPU
  residual, needs vendoring" conclusion. Next hunt target: `runner_run_fceux`
  NMI scheduling, using the exact-CPU vendor as the confound-free oracle.
- **Vendor IRQ wiring is still approximate** (IQTEMP transfer + catch-up arming
  during interrupt-service): Felix 100%→98.8%, Battletoads 69.5%→6.8% in vendor
  mode. Needs level-IRQ (IQEXT) + rising-edge before trusting vendor on
  IRQ/copy-protection games. Non-IRQ Contraf/Mario oracle is already valid.

## `--interp=fceux` de-vendor — the phase hypothesis, and its refutation (2026-07-03 / 2026-07-20)

Goal: make the non-GPL `--interp=fceux` (cpu_interp + ppu.c + apu.c/apu_fceux)
bit-match the GPL `--interp=fceux_vendor` oracle so the vendor `.c` can be
dropped. As of the July verify_all, only **Contraf** (69.7% DESYNC) and
**Battletoads** (12.8% DESYNC) still diverge on `our fceux`; the other 9 are ok.
**Both remaining desyncs are ONE root cause** — proven by instruction-level trace:

**Two separate findings this session:**

1. **cpu_interp was missing the 6502 indexed-store dummy read** (opcodes `$9D`
   STA abs,X, `$99` STA abs,Y, `$91` STA (zp),Y). The real 6502 (and vendor
   `GetABIWR`/`GetIYWR`) does a dummy read of the UNFIXED target
   `(base&0xFF00)|((base+idx)&0xFF)` before the write — a real bus cycle with I/O
   side-effects. Battletoads' reset APU-clear loop `LDX #$17; STA $4000,X` sweeps
   X over `$4016/$4017`; that dummy read hits `ctrl_read` → clears the lag flag,
   exactly like FCEUX. **FIXED** in `cpu_interp.c` → Battletoads first lag
   divergence 4 → 5602. **verify_all: 0 regressions** (all 9 green games unchanged,
   Mario RAM-blip@43 is pre-existing). NOTE `cpu_interp.c` is SHARED by beam +
   recompiled + fceux — the dummy read now fires on any indexed store to
   `$2000-$401F` (all correct HW behaviour). *Still possibly missing:* the same
   unfixed dummy read on RMW abs,X/Y and page-cross dummy read on indexed LOADs.

2. **~~The remaining desync = a CPU↔PPU warm-up PHASE offset; restructuring
   `runner_run_fceux` to postrender-first resolves BOTH games.~~ — REFUTED
   2026-07-20. The restructure was done. It fixed neither game.** Kept here so
   nobody re-derives it.

   What was measured in 2026-07-03 and still holds: the CPU instruction/cycle
   streams are byte-identical between the backends, offset by **+714 instructions
   (2499 cyc / 7502 dots)** of power-on warm-up, because `runner_run_fceux` was
   **visible-first** while `ppuv_loop_frame` is **postrender-first** (a rotation of
   the 81840 visible dots). That part was correct.

   What was WRONG was the conclusion that this phase offset *caused* the Contraf /
   Battletoads divergence. See the 2026-07-20 section below.

## post-render-first restructure — DONE, and it did NOT fix either game (2026-07-20)

`runner_run_fceux` now emits a frame in the vendor's order, and the phase gap is
closed and verified. **Both target games still desync.** Do not spend another
session on frame-loop phase.

**What the loop looks like now** (`SL = 341`, `vbase = 22*SL - skip` = dot of
visible line 0, `skip` = the odd-frame dot drop):

| dots | what |
|---|---|
| `[0, 341)` | post-render line 240 |
| `341` | VBL set; `+12` → NMI |
| `[341, 7161)` | vblank lines 241..260 |
| `[7161, vbase)` | pre-render line 261 (odd-frame dot skip lands here) |
| `[vbase, …)` | visible lines 0..239 |

Two things are BOTH required, and either alone leaves a whole-frame error:
the rotation, **and** `g_ppudead = 2` (the vendor always warms up 2 dead frames;
we used 1). `2*89342 + 341 = 179025` = the vendor's first VBL dot, exactly.
`g_ppudead` is set locally in `runner_run_fceux` so the beam backend, which is
calibrated for 1, is untouched.

**Verification the phase is now identical:** instrument `g_total_cpu_cycles` (NV)
against `timestamp` (vendor, `x6502_vendor.c`) at each frame boundary — equal for
thousands of frames. That closes the phase theory by measurement.

**What it bought (real, but not the goal):**
- Deleted two reset hacks that existed *only* to compensate for the wrong frame
  boundary: the `fm2_peek_cmd` soft-reset peek-ahead and the `g_reset_pending`
  defer-to-next-frame. Reset is now applied inline from the just-consumed record
  via `fceux_power_reset()` / `fceux_soft_reset()`, mirroring
  `runner_run_fceux_full` one-for-one.
- Mario/Battlecity/Felix/Zelda are bit-identical to the oracle (incl. RAM hash);
  Mermaid and Superc reach 100.0% +0 on `our fceux` where `our beam` is 99.6%/99.9%.

**What it cost:** Battletoads 12.8% DESYNC@6904 → 11.6% DESYNC@5960. Contraf went
the other way, 69.7% DESYNC@12516 → 70.2% DESYNC@14458. Roughly a wash.

**Also fixed (kept):** `fceux_on_2002_read` must rebase by `g_fceux_vbase` (visible
line 0 is no longer at dot 0 — without this the `sl` check never matches and the
mid-line sprite-0 check is silently dead) **and add +16**, because FCEUX's
`ResetRL` sets a line's pixel origin 16 dots before the nominal boundary
(`DoLine: … ResetRL(); X6502_Run(16);`), so `GETLASTPIXEL` runs 16 ahead of our
dot. Measured: +16 alone is worth Battletoads @5847 → @5960.

**Tried and measured EXACTLY NEUTRAL — but see "Battletoads SOLVED" in [`battletoads.md`](battletoads.md) before trusting
that:** splitting sprite-0 evaluation into FCEUX's two phases (`FetchSpriteData` at
dot 256 / `RefreshSprites` at dot 325), moving `copy_hori` (`Fixit2`) to dot 262 of
the previous line, making the pre-render do the full `RefreshAddr = TempAddr` at dot
325, and the `tofix` latch (`inc_vert` on the first `$2002` read past dot 268, else
at EndRL). All four are faithful to `ppu_vendor.c`, all four changed Battletoads by
**zero** (11.6% / +68944 / @5960 with and without, bit-identical). Reverted at the
time. ⚠️ **The `tofix` latch was later proven necessary** — it was unobservable in
the whole-line-at-line-start renderer, not wrong. See [`battletoads.md`](battletoads.md).

**Where Battletoads actually stands (@5911 in RAM terms):** line 30, the raster
split. BG opacity matches the vendor for **254 of 256 pixels** — only x=254 and
x=255 differ, and x=254 is precisely the sprite-0 test pixel (`sphitx=254`). The
cause is upstream: `ppu.v_addr` differs by one `inc_vert` — ours `02A0` (fine_y 0,
coarse_y 21) vs vendor `7280` (fine_y 7, coarse_y 20). The `tofix` latch did not
close it, so the extra increment comes from somewhere else. **Next lead:** FCEUX
renders a line *incrementally* — `RefreshLine` draws tiles `[firsttile, lasttile)`
as the CPU advances, advancing `RefreshAddr` as it goes, plus a "render one extra
tile while a sprite-0 is pending" hack. We render the whole line at line start from
a local copy of `v`. Matching that is the next real piece of work.

**Method that worked, reuse it:** per-instruction `(cycle, PC)` trace gated on a
cycle window in both backends (`dbg_ins` before `cpu_interp_step` and before
`ADDCYC(CycTable[b1])`), plus a RAM-write watch (NV via `mem_write`, vendor via the
`WrRAM` macro — the vendor writes zero page directly, NOT through `mem_write`).
Diff the streams **positionally**; the frame label is an artifact.

