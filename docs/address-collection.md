# Address Collection via FM2 Demos

## Purpose

The static recompiler (`tools/nesrecomp.py`) discovers code by BFS from the
reset/NMI/IRQ vectors plus any `cfg` seeds. BFS **cannot** reach code that is
only entered through data-driven dispatch (RTS-trick jump tables, computed
jumps, pointers built at runtime, bank-switched relays). Those addresses are
discovered **dynamically**: play a real demo, and every time execution lands on
an address the recompiler doesn't know, log it.

**Demos are used ONLY for address collection.** They are an input source that
drives the game through real code paths so we can record which addresses are
actually executed. Whether the demo matches the reference emulator frame-for-
frame does *not* matter for address validity — *if the CPU jumped there, it is
code* (a dynamic miss is essentially never a false positive).

**"End of a demo" means the game's ending.** The goal is to run a full
playthrough so every address reachable *along that route* is collected.
Coverage is inherently incomplete:

- Nonlinear games skip levels/branches a single run never visits → use
  **additional demos** that take different routes to fill the gaps.
- A demo that **desyncs early** stops visiting the intended content, so it
  collects far fewer addresses. This is why demo sync matters here — not for
  correctness, but for **coverage** (see "Why sync matters for coverage").

Demos are not used to judge the recompiler: it is not required to match the
interpreter's timing (ADR [0005](decisions/0005-backend-roles.md)). Its
per-opcode correctness is checked by `tests/test_cpu_diff.py`.

## How it works

```
ROM ──► nesrecomp.py BFS (vectors + cfg seeds) ──► generated/_full.c + _dispatch.c
                                                         ▲
play demo ──► interpreter (--interp=fceux / --interp / dispatch fallback)
              every JMP/JSR/RTS/BRK/taken-branch target ─┐
          or  dispatch: call_by_address(PC) MISS ────────┤
                                                         ▼
                                    cfg/GAME.cfg  (extra_func = XXXX / N:XXXX)
                                                         │
                                  rebuild ──► those addresses now recompiled
```

Learn mode is on when `RECOMP_LEARN` is set — automatically in `--headless`
(`RECOMP_LEARN=0` opts out). Two sources feed it (`src/runner.c`, `learn_record`):

- **Interpreter control flow** — `cpu_interp_step()` calls `cpu_interp_flow_hook`
  with the new PC after JMP, JSR, RTS, BRK and taken branches: exactly the places
  where a recompiled block ends and the dispatch looks up the next function.
  Not after RTI (it returns wherever the interrupt hit, mid-block under the
  interpreter). This is what lets **`--interp=fceux`, the FCEUX-synced demo
  player, collect addresses**, also in an `INTERP=1` build. It works under
  `--interp` and for interpreted code in dispatch mode too. Not hooked:
  `--interp=fceux_vendor` (GPL vendor CPU) and the `INTERP=1` dispatch stub
  (`cpu_interp_run`, used without any `--interp` flag).
- **Dispatch misses** — `call_by_address()` found no recompiled function
  (`runner_miss`).

Rules for what is written:

- Only `≥ $8000` (PRG ROM). RAM code stays with the interpreter.
- UNROM `$8000-$BFFF` and AxROM: **bank-qualified**, `extra_func = N:XXXX`, N =
  the bank selected when it ran; `nesrecomp.py` seeds that bank only.
- MMC1/MMC3 `$8000-$BFFF`, MMC5 `$8000-$DFFF`: **skipped** — that code always
  runs in the interpreter (no per-bank recompilation yet), so the seed would be
  dropped anyway. Mirrors `Disassembler.is_switchable()`.
- New entries are **appended** as soon as they are seen (crash-safe). The file is
  never rewritten: `data_region`, `jump_table`, comments survive. Existing
  `extra_func` lines (both forms) are pre-loaded so nothing is logged twice.

## Running it (the iterative loop)

```bash
# 1. fast interpreter-only build (no generated code needed for collection)
make GAME=NesGame INTERP=1

# 2. play the demo to the ending in sync with FCEUX; addresses go to cfg/NesGame.cfg
./bin/NesGame --headless --interp=fceux --playback fm2/NesGame.fm2

# 3. recompile with the collected addresses
make GAME=NesGame

# 4. more demos (other routes) → repeat 2–3
```

Because `--interp=fceux` executes every instruction in the interpreter, one pass
per demo collects everything that demo reaches — no "rebuild until no new
misses" iterations. A dispatch-mode run afterwards
(`./bin/NesGame --headless --playback ...`) is a check: it should log (almost)
nothing new. `tools/verify_all.sh` runs headless too, so it also feeds the cfg.

## `cfg/GAME.cfg` directives

| Directive | Source | Purpose |
|-----------|--------|---------|
| `extra_func = XXXX` | learning mode (auto) / manual | force a BFS entry point |
| `data_region = XXXX,YYYY` | manual | exclude an inline-data range from BFS |
| `inline_data_func = XXXX` | manual | subroutine that eats bytes after the JSR (PLA/PLA) — skip the `pc+3` fallthrough |
| `jump_table = XXXX,N` | manual | N-entry `.word` table at XXXX — seed each valid PRG word |

Only `extra_func` is produced automatically by collection. The other three are
filled by hand after inspecting a disassembly. `cfg/*.cfg` is git-ignored
(regenerable); only `cfg/game.cfg.example` is tracked.

## Why sync matters for coverage (not correctness)

A demo that desyncs from its intended route stops pressing the right buttons,
so the on-screen game wanders off (dies, sits on a menu, replays an early
area). It still executes *valid* code, but it stops reaching the **new** content
that would yield new addresses. So better demo sync → the playthrough reaches
the real ending → maximal addresses per demo.

This is the practical payoff of the lag-sync work (see docs/sync-methodology.md
"Synchronization Methodology"): with the unified FM2 timing model the test
demos play in sync deep into / through their playthroughs, so a single demo
collects far more of the game than a demo that desynced in the first seconds.
`--interp=fceux` plays all 11 test demos lag-exact to the end (see
`docs/STATUS.md`), which is why it is the collection path.

## Coverage limitations & strategy

- One demo = one route. Nonlinear games (branching level select, secret exits)
  need **multiple demos** to cover the alternatives.
- Warps/skips in a TAS *reduce* coverage (they skip levels) — a casual full
  playthrough can cover more code than a fast TAS.
- Code only reachable via specific in-game RNG/state may still be missed; those
  are filled by manual `cfg` seeds or more demos.
- Self-modifying / RAM code is intentionally left to the interpreter fallback
  (not collected as static functions).
