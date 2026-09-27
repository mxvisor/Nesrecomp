# 0001 — Lag sequence is the primary demo-sync metric

- Status: Accepted
- Date: 2026-06 (recorded retroactively 2026-09-27)

## Context
Frame-hash (`--dump-frames`) debugging stalled: the framebuffer is the bottom of
the dependency chain and absorbs every cosmetic PPU difference, so most games
looked "broken" while their demos actually stayed in sync.

## Decision
Measure sync in this order: (1) lag sequence / cumulative lag drift,
(2) RAM hash `$0000-$07FF`, (3) framebuffer. `--dump-sync` + `tools/verify_all.sh`
implement it; the verdict is drift rate, not frame-to-frame match.

## Rejected alternatives
- Framebuffer CRC as primary metric — dominated by cosmetic noise.
- `1stSustDiv` lag-mismatch threshold — reported Contraf as "None" while it visibly desynced.
- Offset-fitting when comparing — hides real transient divergences.

## Consequences
Old framebuffer-era results ([`../investigations/framebuffer-era.md`](../investigations/framebuffer-era.md))
must be re-measured before acting on them. Details: [`../sync-methodology.md`](../sync-methodology.md).
