# Project Status

> **Living document — update at the end of every working session.**
> Keep it short: current state only. History and reasoning go to
> [`investigations/`](investigations/), decisions to [`decisions/`](decisions/).
>
> Last updated: 2026-10-02 (lag table = full `tools/verify_all.sh` run after the
> `cpu_interp.c` fixes; the RAM first-mismatch column is from 2026-07-22/23 and was
> not re-measured).

## Demo sync — lag metric (primary)

`tools/verify_all.sh`, whole movie, vs real-FCEUX reference. Build: `make GAME=X INTERP=1`.
Metric definitions: [`sync-methodology.md`](sync-methodology.md).

| Game | Mapper | frames | `--interp=fceux` lag | `--interp=fceux` RAM first mismatch | beam (`--interp`) lag / drift |
|------|--------|--------|----------------------|-------------------------------------|-------------------------------|
| Mario | NROM-256 | 24429 | 100.0% / +0 | none | 100.0% / +0 |
| Battlecity | NROM-128 | 46898 | 100.0% / +0 | none | 100.0% / +0 |
| Zelda | MMC1 | 80384 | 100.0% / +0 | 3724 (stack page only — benign) | 100.0% / +0 |
| Felix | MMC3 | 81074 | 100.0% / +0 | none | 100.0% / +0 |
| Battletoads | AxROM | 78031 | 100.0% / +0 | none | **74.8% / +14692, DESYNC@6224** |
| Contraf | MMC3 | 169756 | 100.0% / +0 | 8013 (RNG `$0029` churn) | **69.8% / −50182, DESYNC@12969** |
| Mermaid | UNROM | 129000 | 100.0% / +0 | none | 99.6% / −130 (ok) |
| Superc | MMC3 | 181818 | 100.0% / +0 | 4 (vendored port ≠ real FCEUX — out of reach) | 99.9% / −188 (ok) |
| Captain | MMC3 | 713500 | 100.0% / +0 | none | 99.8% / −1261 (ok) |
| Castle3 | MMC5 | 367763 | 100.0% / +0 | 6442 (stack page only — benign) | 99.7% / −684 (ok) |
| Adventure | CNROM | 239999 | 100.0% / +0 | 64950 (not yet categorized) | 99.0% / +391 (ok) |

`--interp=fceux_vendor` (needs `nogpl/`) is also 100.0% / +0 on all 11.

**Summary:** `our fceux` 11/11 demos play through, lag bit-exact (100.0% / +0) on all 11.
Beam backend: 9/11 play through, Battletoads and Contraf desync (timing-churned RNG).
The 2026-10-02 `cpu_interp.c` fixes changed none of these verdicts.

Sources: [`investigations/contraf.md`](investigations/contraf.md),
[`investigations/ram-hash.md`](investigations/ram-hash.md),
[`investigations/battletoads.md`](investigations/battletoads.md),
older beam numbers in [`investigations/lag-results-2026-06.md`](investigations/lag-results-2026-06.md).

## Recompiler (dispatch mode)

The recompiled (dispatch) mode is **not** a sync target: the sync/discovery path is
`--interp=fceux`, which is now correct (11/11 lag-exact) and is what collects addresses
(learn mode) for the recompiler; beam's oracle is hardware, not FCEUX.

Known property (measured 2026-10-02, Battlecity, 46898 frames, dispatch vs `--interp`):
a recompiled block runs to its end before the main loop checks NMI/IRQ, so interrupts
are taken at block boundaries instead of instruction boundaries — the lag flag differs on
234 frames (first @13348; lag count 206 vs 128) and the pushed return PC differs in the
stack page. An experiment that checked interrupts between instructions inside blocks
(`IRQ_CHECK`, opt-in, slow) removed the lag mismatches; it was **dropped as not needed**.
Older framebuffer-hash numbers: [`investigations/framebuffer-era.md`](investigations/framebuffer-era.md).

## Fixed after the differential test (2026-10-02)

`cpu_interp.c` (shared by all backends) and the emitter now agree on every opcode;
`KNOWN` in `tests/test_cpu_diff.py` is empty. `make test` passes (36). **Still needs a
local `tools/verify_all.sh`** — `cpu_interp.c` is shared, and the changes below alter
observable behavior:

- Zero-page pointer wrap: `rd16()` → `rd16_zp()` (`$FF` pointer high byte from `$00`).
- Page-cross +1 cycle, following FCEUX (`LD_ABY`/`LD_IY`/`LD_ABX`): LAX abs,Y `$BF`,
  LAX (zp),Y `$B3`, NOP abs,X `$1C/$3C/$5C/$7C/$DC/$FC` — in both interpreter and emitter.
  LAR abs,Y `$BB` stays a flat 4 cycles (FCEUX `RMW_ABY` has no penalty).
- 14 illegal opcodes added to the interpreter (ANC, ALR, ARR, XAA, AHX, TAS, SHY, SHX,
  LAX #imm, LAR, SBX, SBC `$EB`). Unstable ones (XAA, LAX #imm, AHX, TAS, SHY, SHX) follow
  FCEUX semantics in **both** the interpreter and the emitter (the emitter's old
  best-effort versions differed from FCEUX).
- Emitter: STP comment printed a literal `${pc:04X}`.

## Found while building the CI smoke test (2026-09-27)

- **Recompiled vs `--interp` differ on interrupt timing.** On the smoke ROM
  (`INC $10; JMP loop` + NMI handler) the `--dump-sync` RAM hash differs on 399/600
  frames between dispatch mode and `--interp`: a recompiled function runs a whole
  block before the main loop checks NMI, the interpreter checks after every
  instruction. Likely relevant to "recomp ≪ interp" on Battlecity/Battletoads.
  Next: measure how far the counter drifts; decide on interrupt catch-up inside blocks.
- **Orphan phase is quadratic on 1-byte-terminator padding.** Every `$00` (BRK) —
  or `$02`-class STP — padding byte becomes its own "function", one per pass:
  1 KB of `$00` → 0.25 s / 1034 functions, 4 KB → 3.9 s / 4106 functions. Real ROMs
  with zero padding get thousands of bogus functions and slow `discover`.
  Fix idea: stop the orphan scan on runs of the same terminator byte, or treat
  BRK-only islands as data.

## Open problems / next steps

Ordered roughly by priority.

1. *(removed 2026-10-02: recompiler interrupt timing is not a goal — see "Recompiler (dispatch mode)".)*
2. **FPS benchmark** (checklist 2.1 in [`nesrecomp-bugs.md`](nesrecomp-bugs.md)) — NROM vs MMC3 without vsync + `perf`: is time spent in `cpu_interp_step` or `func_*`? Decides whether bank-aware recompilation for MMC3 is worth it.
3. **RAM-hash residuals on `--interp=fceux`:** Adventure @64950 (categorize), Contraf @8013 (RNG churn). Lead: our `$2000`-write NMI is immediate, FCEUX `TriggerNMI2()` delays one instruction.
4. **cpu_interp dummy reads possibly still missing:** RMW abs,X/Y unfixed-address dummy read; page-cross dummy read on indexed loads ([`investigations/fceux-backend.md`](investigations/fceux-backend.md)).
5. **Beam accuracy** — the target is hardware, not FCEUX: do not chase `lags/*.fceux.txt` with beam. Battletoads/Contraf desync vs FCEUX is not by itself a bug.
6. **Vendor oracle IRQ wiring is approximate** (level IRQ / rising edge) — don't trust `--interp=fceux_vendor` on IRQ-heavy games yet.
7. **Bank-aware recompilation for MMC1 / MMC3 / MMC5** — UNROM and AxROM are done; see [`bank-aware-recompilation.md`](bank-aware-recompilation.md).
8. **Block-boundary yield** (performance) — design in external `next-features.md`; its prerequisite (exact page-cross / branch cycles, bugs 1.4/1.5) is already done.
9. **Generated `_full.c` scalability** — huge TUs (Mermaid ≈ 24500 functions) OOM at `-O2`; options: `-O1` or split TUs.
10. **Discovery heuristics / usability** (open items of the `nesrecomp-bugs.md` checklist): auto-detect `inline_data_func` (PLA/PLA) and `jump_table`; orphan-candidate prologue validation (2.5); `--verbose` / `--dry-run` for `nesrecomp.py`; document ASM-parser limits (3.2) and no `$FFFF` wrap-around (3.5).
11. **MMC5 PRG mode 3** untested — need a ROM (Just Breed, Getsu Fuuma Den, Uncharted Waters).
12. Optional: Mesen-style dropping of `$2000/$2001/$2005/$2006` writes during warm-up; `controller_profile` switch (FCEUX vs hardware).

Possibly obsolete (verify before working on): "dead frame / ppudead=2 hash mismatch" and
Mermaid `$EE` @1892 in [`investigations/framebuffer-era.md`](investigations/framebuffer-era.md) —
both predate the unified FM2 timing and the fceux backend, where Mermaid is now 100% / +0.

## Recently done

- 2026-10-02 — Fixed the `cpu_interp.c` / emitter bugs found by the differential test (see above); the interrupt-granularity experiment (`BLOCK_IRQ_CATCHUP`) was tried and removed.
- 2026-09-27 — CI: GitHub Actions runs `make test` + `tools/ci_smoke.sh` (synthetic ROM, full pipeline, all backends, GPL-free check).
- 2026-09-27 — Test suite for the recompiler (`make test`, 36 tests, synthetic ROMs) incl. differential test emitter vs interpreter; found the `cpu_interp.c` bugs listed above.
- 2026-09-27 — Docs audited against code: fixed stale claims (bugs 1.4/1.5 done, bank-aware UNROM/AxROM done, `--interp=fceux` implemented, PPU dot offsets, interpreter-fallback strategy, file paths, CLI/keys).
- 2026-09-27 — Added `debug-desync` skill (`.claude/skills/`).
- 2026-09-27 — Docs restructured: `AGENTS.md` slimmed; status → `docs/STATUS.md`; logs → `docs/investigations/`; ADRs → `docs/decisions/`.
- `runner`: `--help` / `--verbose`; vendor `.o` linkage fix in non-INTERP build.
- 2026-07-23 — GPL FCEUX oracle de-vendored into gitignored `nogpl/`; tree builds GPL-free.
- 2026-07-22 — MMC5 scanline IRQ clocked in fceux backend (Castle3 → 100% / +0).
- 2026-07-22 — `$2000`-write NMI-enable edge fix (RAM-exact 4 → 7/11; Captain → +0).
- 2026-07-21 — Contraf solved (PowerNES drops CPU↔PPU carry); Battletoads solved on `--interp=fceux` (lazy line rendering + all nine LineUpdate triggers).
