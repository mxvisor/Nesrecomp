# Contraf (MMC3) — PowerNES carry fix

> Moved from `AGENTS.md`. Earlier Contraf analysis (open bus, RNG churn) is in footnote ² of [`lag-results-2026-06.md`](lag-results-2026-06.md).

## Contraf SOLVED — PowerNES discards the CPU↔PPU carry (2026-07-21)

**Fix:** FCEUX's `X6502_Power()` does `memset(&X, 0, sizeof(X))`, and that zeroes
`X.count` — the sub-frame CPU↔PPU remainder. So a **PowerNES throws away the
overshoot of the previous frame's last instruction and restarts the CPU exactly on
the frame boundary.** Our equivalent of `X.count` is `g_fceux_dot`'s carry past
`frame_dots`, and we were carrying it through the reset. Contraf's fm2 has a
`cmd=2` PowerNES at record 5948; we came out of it 8 dots ahead and never recovered.

Three lines in `runner.c`: `fceux_power_reset()` sets `g_fceux_drop_carry`, and the
carry line at the bottom of the frame loop zeroes `g_fceux_dot` instead of
subtracting `frame_dots`. Two things that are easy to get wrong:

- **`fceux_soft_reset()` must NOT set the flag.** `X6502_Reset` only sets
  `_IRQlow |= FCEU_IQRESET`; it does not touch `X.count`. Only PowerNES drops it.
- **The pre-loop `fceux_power_reset()` (the fm2 record-0 power-on marker) must
  disarm the flag again.** It runs before frame 1, where there is no carry yet —
  leaving it armed throws away frame 1's own legitimate overshoot instead. Getting
  this wrong cost Battletoads its 100.0% (it fell to 77.5% / DESYNC@13764) while
  Contraf still looked fixed, because Contraf's mid-movie reset dominated.

**The earlier "1-dot frame-length loss at frame 5943" in this file was WRONG** and
has been deleted. Direct measurement (log `dead`/`kook`/`len`/cumulative dots per
frame on both sides, compare positionally) shows frame lengths are **identical on
all 6000 frames** and the dot position is **identical through frame 5947** — the
divergence starts exactly at the reset. The theory that a 1-dot error must come
from `262*SL - skip` vs `89342 - kook` was sound reasoning from a bad measurement.

⚠️ **Do not compare `g_total_cpu_cycles` against the vendor's `timestamp`** — they
diverge by frame 1327 on Contraf purely as an accounting artefact: DMC stall cycles
advance `g_fceux_dot` but are never added to `g_total_cpu_cycles` (`runner.c`, the
`g_dmc_stall` branch). Compare **dot position**, not cycles. The vendor's absolute
dot is `cumulative_budget*16 - x6502v_count()` (`X.count` is in 1/16-dot units:
1 CPU cycle = 48, 1 dot = 16); ours is `(cumulative_frame_dots + g_fceux_dot)*16`.

Other measurements from the hunt, all still valid and all *negative* — the MMC3
scanline-hook stream is identical over 5900–5960 (12532 events incl. IRQ flags);
the APU frame IRQ is raised at the same instruction; `skip` and `kook` are in phase.
⚠️ When comparing MMC3 hooks, log **both** call sites — the visible-line one *and*
the pre-render one (`runner.c` ~907 / `ppu_vendor.c` 614). Logging only one makes
the vendor look like it has extra events; its `scanline` still reads 240 at the
pre-render hook (stale from the previous frame) — that is not "line 240".

**Corpus after the fix** — `our fceux` is **11/11 ok**. 100.0% +0 on Battlecity,
Battletoads, Contraf, Felix, Mario, Mermaid, Superc, Zelda; Adventure 99.0% +389,
Captain 99.6% −1634, Castle3 99.7% −643 (all `ok`). Note Adventure and Castle3
*moved* (+391→+389, −684→−643): their fm2s also contain a PowerNES, so they were
carrying stray dots through it too. Both are closer to zero.

**Residual, lag-invisible:** the RAM hash still diverges from the FCEUX reference on
some games while `--interp=fceux_vendor` matches it to the end. Lag is unaffected
across the whole corpus, so this is a tightening lead, not a sync bug. See
[`ram-hash.md`](ram-hash.md) — most of this residual was closed 2026-07-22.

