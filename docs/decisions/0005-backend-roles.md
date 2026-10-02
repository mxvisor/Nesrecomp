# 0005 — Roles of the three execution paths; recompiler is not timing-equivalent

- Status: Accepted
- Date: 2026-10-02

## Context
The project has three ways to run a game, and earlier docs mixed up their
goals: a "planned change" demanded that recompiled code be observably
equivalent to per-instruction execution (interrupt point included), and the
recompiler was measured against demos as if it were a sync target. Measured
2026-10-02 (Battlecity, dispatch vs `--interp`): taking NMI only at block
boundaries changes the lag flag on 234 / 46898 frames. An opt-in per-instruction
interrupt check inside blocks removed that, at a large speed cost.

At the same time address collection — the only reason demos are played — ran
only in dispatch mode, i.e. on the one path that is *not* FCEUX-synced, so
demos that desync there collect less code.

## Decision
| Path | Role | Oracle |
|------|------|--------|
| Recompiler (dispatch, no flag) | Fast execution of known code | none — block-granular interrupts are accepted |
| beam (`--interp`; also the dispatch-miss fallback `cpu_interp_step`) | Safety net for code the recompiler does not know; maximum hardware accuracy | hardware (Mesen / test ROMs), not FCEUX |
| `--interp=fceux` | Play FM2 demos in sync with FCEUX to **collect addresses** | real FCEUX |

- The recompiler does **not** have to reproduce the interpreter cycle-for-cycle
  or match demos; its correctness bar is "runs the game", checked per opcode by
  the differential test (`tests/test_cpu_diff.py`), not by demo sync.
- Learn mode records interpreter control flow (`cpu_interp_flow_hook`), so
  `--interp=fceux` collects addresses directly; dispatch misses are still logged.

## Rejected alternatives
- Interrupt catch-up inside recompiled blocks (`IRQ_CHECK` /
  `BLOCK_IRQ_CATCHUP` experiment, 2026-10-02): fixes the lag mismatches but
  costs speed, and nothing needs it once collection runs on `--interp=fceux`.
- Collecting addresses only from dispatch misses: depends on the demo staying in
  sync on the path that is least able to.

## Consequences
- Supersedes the "observable equivalence" invariant of the block-boundary-yield
  plan in external `next-features.md`; that plan stays a performance idea only.
- Do not chase dispatch-vs-interp lag/RAM differences as bugs; do chase
  differential-test failures.
- Headless runs (incl. `tools/verify_all.sh`) now append learned addresses to
  `cfg/GAME.cfg`; set `RECOMP_LEARN=0` to opt out.
