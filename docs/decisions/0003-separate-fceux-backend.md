# 0003 — FCEUX-faithful playback is a separate backend

- Status: Accepted
- Date: 2026-06-21 (recorded retroactively 2026-09-27)

## Context
FM2 demos are recorded on FCEUX's old (scanline-based, lazy-rendering) PPU.
A beam-accurate per-dot PPU cannot reproduce FCEUX-specific timing (sprite-0
visibility, NMI delay, LineUpdate triggers), so frame-perfect demos desync.

## Decision
Add `--interp=fceux` as a separate headless playback backend (gated by
`g_ppu_backend`) that replicates FCEUX's DoLine chunking and lazy renderer.
The beam path (`--interp` / `--interp=beam`) and the recompiler stay the default
and remain hardware-oriented.

## Rejected alternatives
Tuning the beam PPU toward FCEUX — would trade hardware accuracy for
emulator-specific quirks and regress bit-exact games.

## Consequences
Two timing models to maintain; shared code (`cpu_interp.c`, parts of `ppu.c`)
changes affect both — run `tools/verify_all.sh` on both backends.
Design: [`../interp-fceux-design.md`](../interp-fceux-design.md).
