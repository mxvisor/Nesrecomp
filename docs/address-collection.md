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

A later, separate use of demos is to **verify the recompiler** is correct
(replay and compare). That is far off; this document is about collection.

## How it works

```
ROM ──► nesrecomp.py BFS (vectors + cfg seeds) ──► generated/_full.c + _dispatch.c
                                                         │
play demo ──► call_by_address(PC) ─ switch(PC) ──────────┤
                                     ├─ known: run recompiled func_XXXX()
                                     └─ default (MISS): runner_miss(PC)  ← log
                                                        cpu_interp_step() ← run
                                                         │
                                              cfg/GAME.cfg  (extra_func = XXXX)
                                                         │
                                  rebuild ──► those addresses now recompiled
                                  (repeat until no new misses)
```

- **`runner_miss(addr)`** (`src/runner.c`): on a dispatch miss it records
  `addr` (only `≥ $8000`, i.e. PRG-ROM) in a 64 Kbit `miss_map` and **appends**
  it to `cfg/GAME.cfg` immediately (crash-safe; a killed run keeps what it
  found). The missed address is then executed by the interpreter fallback, so
  the demo keeps running correctly through unknown code.
- **`runner_miss_init`**: enabled when `RECOMP_LEARN` is set (auto-set in
  `--headless`). It pre-loads the existing `cfg/GAME.cfg` into `miss_map` so
  known addresses are not re-logged. It accepts both `extra_func = XXXX` and
  bare `XXXX` lines.
- **`runner_miss_write_all`**: on clean exit it rewrites `cfg/GAME.cfg` sorted,
  as `extra_func = XXXX` lines (the form `nesrecomp.py` consumes as BFS seeds).

## Running it (the iterative loop)

```bash
# 1. build (normal dispatch build — see the critical constraint below)
make GAME=NesGame

# 2. play the demo to the ending; misses are logged to cfg/NesGame.cfg
./bin/NesGame --headless --playback fm2/NesGame.fm2

# 3. rebuild: the new cfg addresses are recompiled into the dispatch
make GAME=NesGame

# 4. repeat 2–3 until a run logs no new addresses (cfg stops growing)
```

`--headless` runs at full speed, plays the whole FM2 (no wall-clock limit), and
auto-enables learning. Each iteration the demo reaches the same (or further)
content, recompiles what it hit, and the next run logs only newly-reached code.

## Critical constraint: collection needs DISPATCH mode

`runner_miss` only fires from `call_by_address`'s default case. That path is
used **only by the normal recompiled build running the dispatch**. It is **NOT**
used when:

- **`--interp`** — the main loop calls `cpu_interp_step()` directly and never
  calls `call_by_address`, so nothing is logged; or
- **`INTERP=1` build** — links `src/stub_full.c` (a bare interpreter stub)
  instead of the generated dispatch.

So: **collect addresses with the normal build and WITHOUT `--interp`.** The
`--interp` / `INTERP=1` modes are for *sync verification and fast testing*, not
collection. (They were used to prove the interpreter plays demos correctly —
which is what makes the dispatch's interpreter-fallback trustworthy.)

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

This is the practical payoff of the lag-sync work (see AGENTS.md
"Synchronization Methodology"): with the unified FM2 timing model the test
demos play in sync deep into / through their playthroughs, so a single demo
collects far more of the game than a demo that desynced in the first seconds.

## Coverage limitations & strategy

- One demo = one route. Nonlinear games (branching level select, secret exits)
  need **multiple demos** to cover the alternatives.
- Warps/skips in a TAS *reduce* coverage (they skip levels) — a casual full
  playthrough can cover more code than a fast TAS.
- Code only reachable via specific in-game RNG/state may still be missed; those
  are filled by manual `cfg` seeds or more demos.
- Self-modifying / RAM code is intentionally left to the interpreter fallback
  (not collected as static functions).
