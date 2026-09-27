# Lag-sequence results — all games (snapshot 2026-06)

> Moved from `AGENTS.md`. **Historical snapshot** — numbers predate the 2026-07 fceux-backend fixes. Current numbers: [`../STATUS.md`](../STATUS.md).

Generated with `tools/verify_all.sh`. "drift" = cumulative lag
total (ours − fceux); near-zero = FM2 input stays aligned = demo plays
in sync. "f2f" = frame-to-frame lag match (jitter sensitive — low f2f
with near-zero drift just means many transient single-frame flips).

After **hermetic-SRAM** + **unified FM2 timing** (record N applied at the
START of frame N, like FCEUX) + **ppudead=1** + **odd-frame dot-skip** +
**DMC steal** + **FCEUX-style PPU-register catch-up** (commit 437f989), and
now the **`--interp=fceux` chunk-driven backend** (chunk-render, phase-carry,
12-dot NMI delay, MMC3+MMC5 scanline IRQ) + the **$4016/$4017 open-bus fix**,
full corpus (11 games, whole movie). Both backends vs the real-FCEUX ref;
`tools/verify_all.sh` reproduces this. **Verdict is by drift RATE** (|drift|/n):
synced demos stay ≤0.003, real desyncs jump to ≥0.24. (The old `1stSustDiv`
>150/200 lag-mismatch metric was MISLEADING — it read Contraf-fceux as "None"
even though the demo visibly desyncs at the same place as beam; the lag flips
just stay under threshold. Drift rate / final drift is the honest signal.)

| Game | Mapper | beam drift | beam | fceux drift | fceux |
|------|--------|-----------|------|-------------|-------|
| Mario | NROM-256 | **0** | ok | **0** | ok |
| Battlecity | NROM-128 | **0** | ok | **0** | ok |
| Zelda | MMC1 | **0** | ok | **0** | ok |
| Felix | MMC3 | **0** | ok | **0** | ok |
| Adventure | CNROM | +385 | ok | +386 | ok |
| Superc | MMC3 | −188 | ok | −102 | ok |
| Castle3 | MMC5 | −641 | ok | −684 | ok |
| Captain | MMC3 | −1634 | ok | −1634 | ok |
| Mermaid | UNROM | −368 | ok | −99 | ok |
| Contraf | MMC3 | −50267 | **DESYNC@~12k** | −50179 | **DESYNC@~13k** |
| Battletoads | AxROM | +69199 | **DESYNC@~5.7k** | +19217 | **DESYNC@~6.2k** |

**9/11 demos play through in BOTH backends.** The two desyncs — **Contraf**
and **Battletoads** — fail in **both** beam and fceux at essentially the same
place (Contraf ~11–13k, Battletoads ~5–6k). Both are **timing-churned RNG**
(Battletoads `$25-$27`, Contraf `$0029`): a wait-for-interrupt accumulator whose
value depends on the exact CPU cycle the NMI/IRQ fires; neither backend is
cycle-perfect there. **The fceux backend does NOT make these play through** — its
real wins are: Castle3 (MMC5) & Felix (MMC3) become bit-exact, and Battletoads'
drift is cut −72% (0.887→0.246) — but the RNG residual remains. Wide
address-collection coverage across mappers 0,1,2,3,4,5,7. See footnotes.

¹ Near-100% group: old-PPU (**NewPPU 0**) sub-cycle jitter / heavy-scene
  slowdown FCEUX models and we don't — `drift` accumulates but **no
  sustained divergence**, so the demo stays in sync to the end. Adventure
  is now measured over the full 240k-frame movie (the old cached ref was
  capped at 20k). Low priority; would need a Mesen arbiter to call ours-vs-FCEUX.
² **Contraf** (MMC3) — ROOT-CAUSED (2026-06-21). The 10581 beam desync had two
  parts, both since fixed/explained:
  (a) **Controller-port open bus** — `$4016/$4017` reads must return the open-bus
  $40 (bit 6, the high byte of the $40xx address) in the upper bits; we returned
  $00. Contra Force stores the raw read (`$FFD6: STA $04`, `$FFE1: STA $05`), so
  $04/$05 diverged from frame ~5. Fixed in `ctrl_read()` (src/memory.c, `| 0x40`).
  Hardware/FCEUX-correct; **no regression** (Mario/Battlecity/Felix/Zelda beam
  byte-identical; only Superc/Castle3 RAM hashes shift, and those never matched
  FCEUX anyway — same engine, same churn).
  (b) **Timing-churned RNG (the real blocker, NOT fixed)** — `$0029` is an entropy
  accumulator churned by an infinite wait loop `$FD62: LDA $29; ADC $23; STA $29;
  JMP $FD62` (CLI'd, broken only by NMI/IRQ). Its per-frame value depends on the
  exact CPU cycle the interrupt fires — same precision class as Battletoads' RNG
  churn, **not** copy protection. `$0029` diverges from frame ~5 and RAM never
  bit-matches again, so the **gameplay desyncs at ~11–13k in BOTH backends**
  (verified visually). The controller-open-bus fix + MMC3 IRQ only nudged the lag
  flips under the old `1stSustDiv` threshold (false "None"); the honest drift-rate
  metric (0.296 in both) and the visual desync agree it does NOT play through.
³ **Battletoads** (AxROM) desyncs at ~5–6k in **both** backends — timing-churned
  RNG `$25-$27` (wait-for-NMI churn `$8743`, off-by-one at the copy-protection
  bank-switching NMI handler, frame 15). The fceux backend cuts the drift −72%
  (rate 0.887→0.246) but does not close it. Startup + copy protection are in sync.

**Key takeaways:**
- The lag metric is the demo-sync verdict (the framebuffer accuracy table
  in [`framebuffer-era.md`](framebuffer-era.md) measures cosmetic PPU noise, not sync). 4 games bit-exact
  (100%/drift-0), 5 more in sync to the end, 2 desync.
- Decisive fixes, by impact: (1) **hermetic SRAM**; (2) **unified FM2
  timing** (record N at the START of frame N, like FCEUX); (3) **ppudead=1**
  warm-up; (4) **PPU-register catch-up** (cycle-accurate $2002 reads in
  interp — cut Battletoads' read-drift ~64%, zero regressions).
- Remaining frontier: the 2 desyncs, both **deep sub-cycle timing** —
  **Contraf @10565** (MMC3 raster-split; IRQ-dot & sprite-0 ruled out — see
  note ²) and **Battletoads @5580** (lazy-render sprite-0, needs
  `--interp=fceux`).

