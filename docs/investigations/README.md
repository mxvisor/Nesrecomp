# Investigations

Dated logs of debugging work: hypotheses, measurements, refutations, and methods
that worked. Read the relevant log **before** touching a subsystem, so refuted
ideas are not re-derived.

Conventions:
- One file per topic (game, subsystem or metric); new findings are appended as
  `## <title> (YYYY-MM-DD)` sections.
- Never delete a refuted hypothesis — strike it through / mark **REFUTED** with the
  date and the evidence.
- Once something is settled, put the current fact in [`../STATUS.md`](../STATUS.md)
  (or `AGENTS.md` if every session needs it) and the choice in [`../decisions/`](../decisions/).

| Log | Period | Topic | State |
|-----|--------|-------|-------|
| [`battletoads.md`](battletoads.md) | 2026-06 → 07-21 | Copy-protection relay, sprite-0 poll drift, RNG churn, lazy line rendering | Solved on `--interp=fceux` |
| [`contraf.md`](contraf.md) | 2026-07-21 | PowerNES must drop the CPU↔PPU carry | Solved (lag); RAM residual open |
| [`fceux-backend.md`](fceux-backend.md) | 2026-06-22 → 07-20 | Vendor oracle, phase hypothesis (refuted), post-render-first loop | Reference |
| [`ram-hash.md`](ram-hash.md) | 2026-07-22 | `$2000` NMI-edge fix, MMC5 IRQ, stack-page caveat, A/B/C isolation method | Residuals open |
| [`timing-fixes.md`](timing-fixes.md) | 2026-06 | Unified FM2 timing, ppudead, odd-frame skip, DMC steal, hermetic SRAM | Implemented |
| [`lag-results-2026-06.md`](lag-results-2026-06.md) | 2026-06 | Lag-sequence results snapshot, both backends | Historical |
| [`framebuffer-era.md`](framebuffer-era.md) | early 2026 | Framebuffer-hash accuracy table, Battlecity/Mermaid/dead-frame notes | Historical, partly obsolete |
