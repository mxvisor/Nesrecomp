# Project Status

> **Living document — update at the end of every working session.**
> Keep it short: current state only. History and reasoning go to
> [`investigations/`](investigations/), decisions to [`decisions/`](decisions/).
>
> Last updated: 2026-09-27 (restructured from `AGENTS.md`; numbers are the
> latest recorded there, 2026-07-22/23 — re-run `tools/verify_all.sh` to refresh).

## Demo sync — lag metric (primary)

`tools/verify_all.sh`, whole movie, vs real-FCEUX reference. Build: `make GAME=X INTERP=1`.
Metric definitions: [`sync-methodology.md`](sync-methodology.md).

| Game | Mapper | `--interp=fceux` lag | `--interp=fceux` RAM first mismatch | beam (`--interp`) |
|------|--------|----------------------|-------------------------------------|-------------------|
| Mario | NROM-256 | 100.0% / +0 | none | drift 0 |
| Battlecity | NROM-128 | 100.0% / +0 | none | drift 0 |
| Zelda | MMC1 | 100.0% / +0 | 3724 (stack page only — benign) | drift 0 |
| Felix | MMC3 | 100.0% / +0 | none | drift 0 |
| Battletoads | AxROM | 100.0% / +0 | none | **DESYNC ~5–6k** |
| Contraf | MMC3 | 100.0% / +0 | 8013 (RNG `$0029` churn) | **DESYNC ~11–13k** |
| Mermaid | UNROM | 100.0% / +0 | none | ok (drift ≠ 0) |
| Superc | MMC3 | 100.0% / +0 | 4 (vendored port ≠ real FCEUX — out of reach) | ok (drift ≠ 0) |
| Captain | MMC3 | +0 | none | ok (drift ≠ 0) |
| Castle3 | MMC5 | 100.0% / +0 | 6442 (stack page only — benign) | ok (drift ≠ 0) |
| Adventure | CNROM | +210 (ok) | 64950 (not yet categorized) | ok (drift ≠ 0) |

**Summary:** `our fceux` 11/11 demos play through; lag bit-exact on 10/11.
Beam backend: 9/11 play through, Battletoads and Contraf desync (timing-churned RNG).

Sources: [`investigations/contraf.md`](investigations/contraf.md),
[`investigations/ram-hash.md`](investigations/ram-hash.md),
[`investigations/battletoads.md`](investigations/battletoads.md),
older beam numbers in [`investigations/lag-results-2026-06.md`](investigations/lag-results-2026-06.md).

## Recompiler (dispatch mode)

Last measured only with the old framebuffer-hash metric — see
[`investigations/framebuffer-era.md`](investigations/framebuffer-era.md).
Indications of recompiler code-gen bugs (recomp ≪ interp): **Battlecity** (divergence from
frame ~12604) and **Battletoads**. Not yet re-measured with the lag/RAM metric.

## Found by the differential test (`tests/test_cpu_diff.py`, 2026-09-27)

Bugs in `src/cpu_interp.c` (shared by all backends — fix, then run `make test`
**and** `tools/verify_all.sh` locally):

- **Zero-page pointer wrap:** `rd16()` fetches the high byte of a `(zp,X)` / `(zp),Y`
  pointer at `$FF` from `$0100` instead of `$00`. Hardware and FCEUX (`GetIX`/`GetIY`)
  wrap; the emitter's `izx_addr`/`izy_addr` wrap. Test: `test_zero_page_pointer_wraps`
  (expected failure).
- **LAX abs,Y (`$BF`)** has a flat 4 cycles; missing the +1 page-cross penalty.
- **14 illegal opcodes not implemented** (fall to "skip 1 byte, 2 cycles"): ANC `$0B/$2B`,
  ALR `$4B`, ARR `$6B`, XAA `$8B`, AHX `$93/$9F`, TAS `$9B`, SHY `$9C`, SHX `$9E`,
  LAX #imm `$AB`, LAR `$BB`, SBX `$CB`, SBC #imm `$EB`. The emitter implements them.

Likely missing page-cross penalties in **both** emitter and interpreter (hardware
tables; check FCEUX before changing — it is the sync reference): LAX (zp),Y `$B3`,
LAR abs,Y `$BB`, NOP abs,X `$1C/$3C/$5C/$7C/$DC/$FC`.

Cosmetic (emitter): the STP comment prints a literal `${pc:04X}` (doubled braces in the f-string).

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

1. **Re-measure the recompiler with the lag/RAM metric** (Battlecity, Battletoads) — the framebuffer numbers are stale.
2. **FPS benchmark** (checklist 2.1 in [`nesrecomp-bugs.md`](nesrecomp-bugs.md)) — NROM vs MMC3 without vsync + `perf`: is time spent in `cpu_interp_step` or `func_*`? Decides whether bank-aware recompilation for MMC3 is worth it.
3. **RAM-hash residuals on `--interp=fceux`:** Adventure @64950 (categorize), Contraf @8013 (RNG churn). Lead: our `$2000`-write NMI is immediate, FCEUX `TriggerNMI2()` delays one instruction.
4. **cpu_interp dummy reads possibly still missing:** RMW abs,X/Y unfixed-address dummy read; page-cross dummy read on indexed loads ([`investigations/fceux-backend.md`](investigations/fceux-backend.md)).
5. **Beam backend desyncs** (Battletoads, Contraf) — deep sub-cycle timing; `--interp=fceux` is the sync path, beam is the hardware-accuracy path.
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
