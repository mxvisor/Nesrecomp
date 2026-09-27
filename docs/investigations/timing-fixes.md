# Timing & playback fixes — implemented

> Moved from `AGENTS.md`. Why each fix exists and what it changed; useful before touching FM2 timing, warm-up, odd-frame skip, DMC steal or SRAM handling.

## Unified FM2 timing + PPU warm-up (ppudead=1) — implemented

**The single biggest sync fix.** FCEUX applies each movie record at the
START of its frame: `FCEU_UpdateInput()` latches controllers + processes
reset/power commands, THEN `FCEUPPU_Loop()` emulates the frame. So record
N drives frame N.

Our loop ticks the record at the VBL that ENDS a frame, which made record
N drive frame N+1 — one frame late. Harmless for steady-state controller
input on most games (the poll happens in the next frame's NMI handler
anyway), but wrong for a frame-1 reset: the game booted once, then got
reset and booted again (an extra startup iteration → Battletoads desync).

**Fix (runner_run):** pre-load record 1 before the loop (apply its
controllers + reset, reset the lag flag), then the in-loop tick at each
frame end advances to record N+1. Net: frame N consumes record N, exactly
like FCEUX. Mid-movie resets are handled the same way (applied before the
frame they belong to).

**PPU warm-up:** `ppu.c` gates the scanline-241 VBL/NMI block on
`g_ppudead`; `runner.c` decrements it once per frame AFTER the dumps read
it (keep the decrement last — an earlier ordering bug made frame 2 not
gray → 36% framebuffer). Under the unified timing the correct value is
**`ppudead=1`** (not 2 — the old value compensated for the late-input
model). 

**Result (full-movie lag vs FCEUX):** Mario/Battlecity/Felix/Zelda go to
**exact 100% / drift 0**; Adventure 99.96%; Battletoads' startup +
copy-protection now in sync (was desyncing at frame 4 → now ~5800).
Castle3/Mermaid stay in sync with their own slow heavy-scene drift. This
replaced the previous hermetic-SRAM+ppudead=2 result (99.7–99.9%, ±1).

**Still TODO (optional, Mesen-style):** drop `$2000/$2001/$2005/$2006`
writes during the ~29658-cycle warm-up window. Not needed for lag sync.

## Odd-frame dot skip (NTSC) — implemented

`ppu.c` now skips the idle dot at (scanline 261, dot 340) on odd frames
when rendering is enabled, so frames alternate 89342/89341 dots
(29780.5 CPU cyc avg) instead of always 89342. Implemented in the dot
advance block via `ppu.frame_odd` (toggled at the 261→0 wrap).

**Result:** improved **Mermaid** (lag drift −137 → **−26**); neutral on
the 6 in-sync games. Battletoads' lag number swung (69%→11%) but it is
already desynced at frame 4 (copy protection) so its lag count is noise
— two different desynced trajectories, not a real regression. The skip
is hardware-correct and FCEUX old-PPU also models it, hence the Mermaid
gain toward the reference.

## DMC DMA cycle stealing — implemented

Models the ~4-cycle CPU stall per DMC sample-byte fetch: `apu_step()`
accumulates the stolen cycles in `g_dmc_stall` (file-scope in apu.c,
referenced via inline `extern` in runner.c so it does NOT touch a shared
header / trigger giant generated-file rebuilds), and `runner_run()`
drains them by advancing PPU/APU without running CPU.

First validated on the **full 367k Castle3 playthrough** which drives DMC
hard (1.35M byte fetches): drift −825 → **−793**. Marginal because DMC
steal is inherently small (~15 cyc/frame even at that fetch rate), so it
can't explain Castle3's heavy-scene slowdown — but it is hardware-correct
and **neutral on non-DMC games** (`g_dmc_stall` stays 0; verified
on/off-identical on Battletoads/Mermaid). Kept for correctness.

## Hermetic SRAM during playback/dump (implemented)

Battery-backed games (e.g. Zelda) were loading `bin/sav/GAME_battery.sav`
(a save from a previous run) over the cleared power-on SRAM, booting the
game into a different state than the FCEUX movie (recorded from power-on)
→ instant desync. Fix: `g_hermetic` flag (set when `--playback` /
`--dump-sync` / `--dump-frames`) skips `sram_load()`/`sram_save()` so the
run starts from clean power-on SRAM and never persists it. Normal play
still loads/saves the battery. This cut Zelda's startup offset +8→+1.

