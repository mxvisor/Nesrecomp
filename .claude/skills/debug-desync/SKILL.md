---
name: debug-desync
description: Step-by-step procedure for debugging an FM2 demo desync or a lag/RAM-hash divergence against FCEUX in NESRecomp (beam, --interp=fceux or vendor backend). Use when a game "desyncs", verify_all.sh shows DESYNC / non-zero drift / a RAM first-mismatch, or a PPU/timing/NMI/IRQ change needs to be checked for sync regressions.
---

# Debug a demo desync

Follow the steps in order. Do not skip ahead to framebuffer comparison or to
editing `ppu.c`: most wasted sessions in this project came from debugging the
wrong layer or a downstream symptom. Background: `docs/sync-methodology.md`.

## 0. Preconditions

- **ROMs and demos are local-only** (`rom/`, `fm2/`, `lags/` are gitignored). If
  `rom/GAME.nes` or `fm2/GAME.fm2` is missing (e.g. a cloud session), you cannot
  run anything: say so, and limit yourself to reading code and investigation
  logs. Never invent numbers.
- **Read the history first:** `docs/STATUS.md` and the game's log in
  `docs/investigations/` (index: `docs/investigations/README.md`). Check the
  REFUTED hypotheses there before proposing one — e.g. frame-loop phase for
  Contraf/Battletoads was measured and refuted.

## 1. Validate the demo, not the emulator

1. ROM match: `verify_all.sh` does this automatically and prints `ROM MISMATCH`.
   Manually: FM2 `romChecksum` = base64(MD5 of the ROM minus the 16-byte iNES header).
2. Does the demo play correctly **in FCEUX itself**? If not, the FM2/ROM pair is wrong.
3. FM2 header `NewPPU`: `0`/absent means it was recorded on FCEUX's old PPU. Our
   beam backend is allowed to diverge from it on timing-sensitive games — compare
   against `--interp=fceux` instead of "fixing" the beam PPU.

## 2. Reproduce with the standard tool

```bash
make GAME=X INTERP=1            # always INTERP=1 for sync work
tools/verify_all.sh X           # one game → summary + first-divergence detail block
REBUILD=0 FCEUX=0 tools/verify_all.sh X   # rerun without rebuilding / relaunching FCEUX
```

Record the baseline for **every backend column** (beam, fceux, vendor if
`nogpl/` is present): `lag%`, `drift`, verdict, first RAM mismatch.

## 3. Pick the frame to debug — by metric priority

1. **Lag / cumulative drift** decides sync. Drift rate ≤ 0.003 = in sync;
   ≥ 0.24 = real desync. Transient single-frame lag flips with drift ≈ 0 are
   NMI-phase jitter, not a bug. Debug the first frame where the running
   totals start to separate.
2. **RAM hash** only once lag matches end-to-end. Before calling a RAM mismatch
   real, split the diff by page: a mismatch **only in `$0100-$01FF`** that
   re-converges next frame is dead stack bytes (benign — Zelda @3724, Castle3 @6442).
3. **Framebuffer** (`--dump-frames`) last, and only for cosmetic issues.

Everything after the first real divergence is downstream noise — do not analyse it.

## 4. Isolate the layer (A/B/C)

At the first divergent frame, dump raw RAM `$000-$7FF` from:

- **A** = `--interp=fceux` (cpu_interp + our ppu + our loop)
- **B** = hybrid: vendor CPU + our ppu + our loop (temporarily force
  `g_cpu_vendor=1` while still calling `runner_run_fceux`; needs `nogpl/`)
- **C** = `--interp=fceux_vendor` (oracle; needs `nogpl/`)

A == B ⇒ the CPU core is not the cause; look at `ppu.c` / the runner loop /
mapper IRQ. A ≠ B ⇒ `cpu_interp.c` (cycles, dummy reads, illegal opcodes).
B == C == A but ≠ real FCEUX ⇒ vendored port differs from FCEUX — usually out of reach.

## 5. Localise inside the frame

Useful per-frame counters (add temporarily, both backends):
- NMI / IRQ delivery count and interrupted PC per frame (found the 2-NMIs/frame
  `$2000` edge bug and the never-clocked MMC5 IRQ).
- Instruction count per frame; `$2002`-read count per frame.
- Dot position per frame boundary. **Compare dot position, not
  `g_total_cpu_cycles` vs vendor `timestamp`** — DMC stall cycles make the cycle
  counters drift apart as an accounting artefact.

Then a per-instruction `(cycle, PC)` trace gated on a cycle window, in both
backends, plus a RAM-write watch (ours via `mem_write`; the vendor writes zero
page directly through its `WrRAM` macro, not `mem_write`). **Diff the streams
positionally**; frame labels are artefacts. When comparing MMC3 hooks, log both
call sites (visible line and pre-render).

This instrumentation is ad-hoc: keep it out of commits.

## 6. Usual root causes (check first)

1. NMI/IRQ moment (VBL dot, 12-dot NMI delay, `$2000` NMI-enable edge, mapper IRQ clocking).
2. CPU cycle accuracy — page-cross / taken-branch penalties are implemented
   (bugs 1.4/1.5), so check dummy reads first: indexed-store dummy read is done,
   RMW abs,X/Y and page-cross indexed-load dummy reads may still be missing
   (they matter when they hit `$2002`/`$2007`/`$4016/$4017`).
3. Reset handling (PowerNES drops the CPU↔PPU carry; soft reset does not).
4. PPU register side effects (`LineUpdate` triggers on `$200x` reads/writes).
5. Timing-churned RNG loops (`wait-for-NMI` accumulators) — they amplify any of the above.

## 7. Verify the fix

- Rerun `tools/verify_all.sh` on the **whole corpus**, not just the target game.
  `cpu_interp.c` and most of `ppu.c` are shared by beam, recompiler and fceux
  backends — a fix for one game can regress another.
- No regressions allowed in games that are currently bit-exact (see `docs/STATUS.md`).
- A change that measures **neutral** may be unobservable in the current
  structure rather than wrong — note it, don't discard the idea permanently.

## 8. Write it down

- Append a dated `## <title> (YYYY-MM-DD)` section to
  `docs/investigations/<game-or-topic>.md`: symptom, measurements, what was
  refuted (keep it, marked **REFUTED**), the fix, and the method that worked.
- Update the table and "Recently done" in `docs/STATUS.md`.
- Architectural choice (new backend behaviour, compat knob) → ADR in `docs/decisions/`.
- Commit only after the user confirms (project convention).
