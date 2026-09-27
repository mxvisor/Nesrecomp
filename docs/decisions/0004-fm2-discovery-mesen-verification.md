# 0004 — FM2 for discovery, Mesen for verification

- Status: Accepted
- Date: 2026-06 (recorded retroactively 2026-09-27)

## Context
Full playthroughs exist almost only as FM2/bk2 (TASVideos), recorded on FCEUX,
which is not cycle-accurate. We need both wide code coverage and a trustworthy
correctness reference.

## Decision
- **Discovery** (collect addresses for recompilation): FM2. Desync tolerance is
  high — a desynced tail still executes real game code.
- **Verification** (prove core correctness): Mesen 2 / test ROMs, with short MSM
  recordings or shared input tables.

## Rejected alternatives
Converting FM2 into full Mesen playthroughs — old-PPU timing makes long runs desync.

## Consequences
Timing disputes are arbitrated against Mesen, not FCEUX.
Details: [`../sync-methodology.md`](../sync-methodology.md) ("Format strategy", "Reference emulator caveat").
