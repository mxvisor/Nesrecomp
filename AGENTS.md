# AGENTS.md — NESRecomp Project Guide for AI Agents

## Conventions

- **Commit messages**: always in English
- **Code comments**: always in English
- **AGENTS.md / README**: English
- **Commits**: never commit without explicit user confirmation
- **After every change to `tools/nesrecomp.py` or `src/cpu_interp.c`**: run `make test` (no ROMs needed; see "Tests").
- **After every change to `tools/nesrecomp.py`**: also run a full recompile + build on a real ROM before continuing. Do not proceed to the next task until the build is clean. Example:
  ```bash
  make GAME=Battle
  ```
- **ROMs / demos are local-only**: `rom/`, `fm2/`, `asm/`, `lags/` are gitignored. In cloud sessions they are absent — do not try to build real games or run `tools/verify_all.sh`, and never invent accuracy numbers; limit yourself to code and docs, and say what needs a local run.
- **Docs layout**: see "Documentation Map & Session Workflow" at the end of this file. Keep this file short — it is loaded into every agent session. Status goes to `docs/STATUS.md`, investigation logs to `docs/investigations/`.

---

## Project Overview

NESRecomp is a **static recompiler for NES ROM files**. It converts 6502 machine code into native C functions, then compiles them into a standalone executable. Unknown code paths fall back to a cycle-accurate 6502 interpreter. The result is a per-game binary with the ROM data embedded — no external ROM file needed at runtime.

---

## Architecture

```
ROM file
   ↓
tools/nesrecomp.py    — BFS static disassembler → emits C
   ↓
generated/
  NesGame_full.c         — one void func_XXXX(void) per discovered block
  NesGame_dispatch.c     — call_by_address(addr): switch → func or interpreter
  NesGame_embedded_data.h/.c — PRG/CHR ROM compiled into binary
   ↓
Compiled binary (runner.c main loop drives execution)
```

### Execution Model

The main loop in `runner.c` is **yield-based**, not a tight interpreter loop:

1. Check NMI/IRQ pending flags
2. `call_by_address(cpu.PC)` — dispatches to a recompiled function OR interpreter
3. Step PPU: `ppu_step()` called 3× per accumulated CPU cycle
4. Step APU: `apu_step()` called 1× per accumulated CPU cycle
5. On scanline 241 (VBlank): render frame, flush audio

A recompiled function runs straight-line code and returns at the first control-flow instruction (JMP, JSR, branch taken, RTS, RTI, BRK), yielding back to the main loop — so the yield granularity is a **basic block, not an instruction**. NMI/IRQ are only taken between yields: in dispatch mode an interrupt can be serviced up to one block later than under `--interp`, so recompiled and interpreted runs are **not** bit-identical even on trivial programs (measured with `tools/ci_smoke.sh`'s ROM: RAM hash differs on ~2/3 of frames). Before a timing-sensitive access (reads/writes of `$2000-$3FFF`, `$4000-$401F`, writes to `$8000+` — see `is_sensitive_access()`) the emitter inserts `tick_ppu_apu()` so PPU/APU catch up to the access.

`--interp` / `--interp=fceux` replace step 2 with `cpu_interp_step()`; the fceux backend uses its own chunked loop (`runner_run_fceux`, `cpu_interp_run_cycles`).

> **Planned change:** the target architecture is **block-boundary yield with catch-up**: keep yielding only at control flow and timing-sensitive accesses (as today), but make observable timing — including the interrupt point — equivalent to per-instruction execution. This is a large change with a strict correctness invariant (observable equivalence to the per-instruction mode). Its prerequisite — exact per-instruction cycle counts (bugs 1.4/1.5: page-cross and taken-branch penalties) — is already implemented in the emitter and in `cpu_interp.c`. Full design + checklist: `next-features.md` (external).

---

## Key Files

| File | Purpose |
|------|---------|
| `tools/nesrecomp.py` | Static recompiler — BFS discovery + C code emitter |
| `tools/asm_parser.py` | ca65 label parser — extracts labeled addresses ≥ $8000 as extra BFS seeds |
| `src/runner.c`, `src/include/runner.h` | Main loop (beam + `runner_run_fceux`), SDL2 window, input, save states, CLI, interrupts, learning mode, sync dumps |
| `src/fm2_player.c` | FM2 movie parser/playback (`fm2_open()`) |
| `src/memory.c` | CPU address map ($0000–$FFFF): RAM, PPU regs, APU I/O, ROM; controller + lag flag |
| `src/cpu_interp.c` | Full 6502 interpreter incl. illegal opcodes: `cpu_interp_step`, `cpu_interp_run`, `cpu_interp_run_cycles` |
| `src/ppu.c` | 2C02 PPU: beam-accurate per-dot path + FCEUX-style lazy renderer (`fceux_*`) |
| `src/apu.c`, `src/apu_fceux.c` | APU: pulse ×2, triangle, noise, DMC (+ DMC DMA stall); FCEUX-timing variant |
| `src/mapper.c` | Bank switching: NROM(0), MMC1(1), UNROM(2), CNROM(3), MMC3(4), MMC5(5), AxROM(7) |
| `src/stub_full.c` | `INTERP=1` stand-in for the generated code (`call_by_address → cpu_interp_run`) |
| `src/include/interrupts.h` | NMI/IRQ pending flags and vector logic |
| `cfg/NesGame.cfg` | Extra entry points and discovery hints (manual, `asm_parser.py`, learning mode) — not in git |
| `rom/NesGame.nes` | NES ROM files (not in git — local copies) |
| `asm/NesGame.asm` | Optional ca65 assembly for label-based discovery (not in git; auto-detected by `make`) |
| `fm2/NesGame.fm2` | Optional FCEUX TAS file for playback-based discovery (not in git) |
| `tools/extract_rom_data.py` | ROM parser → embedded data header/source |
| `generated/` | Auto-generated files — do not edit manually |

---

## Static Recompiler (tools/nesrecomp.py)

### Discovery (BFS)

Seeds: RESET ($FFFC), NMI ($FFFA), IRQ ($FFFE) vectors.

For each entry point:
- Decode instructions sequentially
- On JSR: enqueue target + return address ($PC+3)
- On branch: enqueue both taken and not-taken targets
- Stop at terminators: RTS, RTI, JMP abs, BRK, STP

Indirect JMPs (`JMP ($XXXX)`) — if the pointer address is in ROM (`>= $8000`), BFS reads consecutive word-sized entries from that address and enqueues each one that looks like a valid PRG address (`$8000–$FFFF`). Stops at the first non-PRG word. This catches dispatch tables of the form:

```asm
JMP (handler_table)
handler_table:
    .word handler_a, handler_b, handler_c
```

Switchable PRG regions (`is_switchable()`) are excluded from the fixed-bank BFS: MMC1/UNROM/MMC3 `$8000–$BFFF`, MMC5 `$8000–$DFFF`, AxROM the whole `$8000–$FFFF`. For **UNROM (2) and AxROM (7)** each bank is then disassembled separately (`discover_banked`, `_bfs_bank`) into `func_bN_XXXX`, dispatched by `mapper_get_prg_bank(0)`. For MMC1/MMC3/MMC5 switchable code always runs in the interpreter (dispatch miss → `cpu_interp_step()`). Plan for the rest: `docs/bank-aware-recompilation.md`.

### Orphan Phase

After BFS, a second pass finds **orphan code islands** — subroutines that BFS never reached because no JSR/JMP/branch points to them statically (e.g. called only via RTS-trick dispatch or data-driven jump tables).

For each discovered function, the pass looks at the bytes immediately after its terminator (RTS/RTI/JMP/BRK/STP). If those bytes decode as a valid instruction, they become new BFS seeds. The scan tries up to `--orphan-window` byte offsets past the terminator to skip small inline data gaps between subroutines.

**Why it's needed:** ca65/cc65 output often places subroutines back-to-back with zero padding between them. A tight BFS would stop at each RTS and never look at the next function. The orphan phase recovers these.

**Tuning:**
- Default window of 3 handles: 0-byte gaps (adjacent functions) and 1–2 byte alignment pads.
- Use `--orphan-window 16` to also skip over small inline data tables (e.g. a 3-entry jump table) between subroutines.
- Too large a window risks decoding data bytes as code — check `grep "UNHANDLED" generated/*_full.c` afterwards.

```bash
# More aggressive orphan discovery:
make GAME=NesGame ROM=rom/NesGame.nes ORPHAN=16
# or directly:
python tools/nesrecomp.py rom/NesGame.nes --game NesGame --orphan-window 16
```

### Config Seeds

`cfg/NesGame.cfg` supports four directives (see `cfg/game.cfg.example`):

| Directive | Purpose |
|-----------|---------|
| `extra_func = XXXX` | Force-add entry point — for state machine handlers, learning mode output. Bank-qualified form `extra_func = N:XXXX` seeds bank N (UNROM/AxROM) |
| `data_region = XXXX,YYYY` | Exclude address range from BFS — prevents phantom functions from inline data |
| `inline_data_func = XXXX` | Mark subroutine that consumes bytes after JSR as data (PLA/PLA pattern) — BFS skips `pc+3` fallthrough |
| `jump_table = XXXX,N` | Declare jump table of N entries at XXXX — BFS adds each valid word as a seed |

`extra_func` entries come from learning mode or manual analysis. The other three must be filled manually after inspecting a disassembly.

**TODO:** auto-detect `inline_data_func` and `jump_table` from the ASM parser.
- `inline_data_func`: subroutines whose first instructions are `PLA / STA $zp / PLA / STA $zp+1`
- `jump_table`: labels immediately followed by `.word` entries with valid PRG addresses

Requires refactoring `parse_asm_labels` to return structured data instead of `Set[int]`. Fix ASM parser edge cases (bug 3.2) first.

### Emitted Code Pattern

```c
// NesGame_full.c
void func_8000(void) {
    cpu.PC = 0x8000;
    /* $8000 LDA #$42 */
    g_cpu_cycles += 2;
    cpu.A = 0x42;
    SET_NZ(cpu.A);
    /* $8002 JMP $8100 */
    g_cpu_cycles += 3;
    cpu.PC = 0x8100;
    return;  // yield
}

// NesGame_dispatch.c
void call_by_address(uint16_t addr) {
    switch (addr) {
        case 0x8000: func_8000(); return;
        // ...
        default:
            runner_miss(addr);     // log in learning mode
            cpu_interp_step();     // single instruction fallback
            return;
    }
}
```

---

## Interpreter Fallback

| Function | Used by |
|----------|---------|
| `cpu_interp_step()` | Every dispatch miss in recompiled mode (all mappers, incl. switchable-bank code of MMC1/MMC3/MMC5 and unknown banks) and the whole `--interp` beam loop. Executes one instruction and returns to the main loop for PPU/APU sync. |
| `cpu_interp_run(addr)` | Only `src/stub_full.c` (`INTERP=1` builds). Runs until RTS/RTI returns to the original stack depth. |
| `cpu_interp_run_cycles(n)` | `--interp=fceux` chunked loop (FCEUX `X6502_Run` analogue). |

---

## Learning Mode (Incremental Discovery)

> **Full guide: [`docs/address-collection.md`](docs/address-collection.md)** — what
> demos are for (collecting addresses to the game's ending), the dispatch-mode
> requirement (collection does NOT work under `--interp` / `INTERP=1`), the
> iterative loop, cfg directives, and why demo sync drives coverage.

```bash
# Run headless and log missed addresses
RECOMP_LEARN=1 ./bin/NesGame --headless --seconds 30

# Recompile with discovered addresses written to cfg/NesGame.cfg
make ROM=rom/NesGame.nes GAME=NesGame
```

Repeat until no new misses. If an FM2 file exists for the game, always prefer it over a timed headless run — it covers far more code paths and terminates automatically when playback ends:

```bash
# Preferred: FM2 playback (terminates when done, covers all code paths in the recording)
# NOTE: always use --headless for FM2 playback — it runs at maximum speed (no SDL throttle).
./bin/NesGame --headless --playback fm2/NesGame.fm2

# Fallback: timed headless run (no FM2 available)
RECOMP_LEARN=1 ./bin/NesGame --headless --seconds 30
```

Learning mode is enabled automatically in headless mode. After the run, re-run `make GAME=NesGame` to rebuild with the new addresses.

---

## Temporary Directory

`./tmp/` — all temporary working files go here (not in git).

- **Screenshots**: `./tmp/screenshots/` — use for all screenshot comparisons
- **Frame dumps**: `./tmp/` — hash dump files from `--dump-frames`
- Tool: `tools/fceux_screenshot.sh GAME FRAME [OUT.png]` — capture FCEUX reference screenshot

---

## Local Asset Directories (not in git)

Three directories are not tracked by git — intentionally (ROM files are copyrighted; TAS and ASM files are working materials):

### `rom/`

NES ROM files. Pass the path via:
```bash
make ROM=rom/NesGame.nes GAME=NesGame
```

### `fm2/`

FCEUX TAS movie files. Used in learning mode — replay recorded inputs to cover more code than manual play.

```bash
./bin/NesGame --playback fm2/NesGame.fm2 --headless
```

Format: FCEUX FM2 (text). Each line `|skip|P1|P2|` describes one frame. Parser: `runner.c → fm2_load()`.

### `asm/`

ca65 assembly files with labels for specific games. Used during recompilation (`--asm`) to give `tools/nesrecomp.py` function names and extra entry points from manual disassembly.

```bash
make ROM=rom/NesGame.nes GAME=NesGame ASM=NesGame.asm
```

When passed via `ASM=`, the Makefile automatically prepends the `asm/` prefix — specify only the filename without path.

---

## Build System

```bash
# Full pipeline (ROM → embed → parse_asm → discover → compile → binary)
make ROM=rom/NesGame.nes GAME=NesGame

# With ca65 assembly labels (asm_parser.py runs as a separate step)
make ROM=rom/NesGame.nes GAME=NesGame ASM=NesGame.asm

# Individual pipeline steps:
make gen_embed  GAME=NesGame ROM=rom/NesGame.nes   # extract PRG/CHR to C header
make parse_asm  GAME=NesGame ASM=NesGame.asm       # ca65 labels → merged into cfg/NesGame.cfg
make discover   GAME=NesGame ROM=rom/NesGame.nes   # BFS + C emit
make compile    GAME=NesGame                       # C → binary (no re-disassembly)

# Interpreter-only build (no discover, links src/stub_full.c) — use for all demo-sync work
make GAME=NesGame INTERP=1

# Build every rom/*.nes; list targets/options
make roms
make help

# Cross-compile for Windows
make CROSS=1 GAME=NesGame
```

`ROM` defaults to `rom/$(GAME).nes`; `ASM` is auto-detected as `asm/$(GAME).asm`. Other options: `ORPHAN=N`, `DEFAULT_SCALE=N`.

Output: `bin/NesGame` (Linux) or `bin/NesGame.exe` (Windows).

Pipeline data flow:
```
ROM
 ├─ gen_embed  → generated/NesGame_embedded_data.h/.c
 ├─ parse_asm  → cfg/NesGame.cfg  (merges new extra_func entries in-place)
 └─ discover   ← cfg/NesGame.cfg
               → generated/NesGame_full.c + NesGame_dispatch.c
```

---

## Tests

```bash
make test        # = python3 -m unittest discover -s tests -v
```

Synthetic ROMs only — runs anywhere, including cloud sessions without `rom/`.

```bash
tools/ci_smoke.sh   # needs SDL2 dev: builds a synthetic ROM through the whole pipeline
                    # (normal + INTERP=1), runs recompiled / --interp / --interp=fceux headless
```

CI (`.github/workflows/ci.yml`) runs `make test` and `tools/ci_smoke.sh` on every push and PR.

| File | Covers |
|------|--------|
| `tests/test_discovery.py` | iNES parsing, BFS (JSR/branch/JMP/indirect tables), `inline_data_func`, `data_region`, orphan phase, cfg parsing |
| `tests/test_emit.py` | Cycle/page-cross/branch emission, `tick_ppu_apu()` placement, dispatch, UNROM/AxROM per-bank code, CLI end-to-end, OPTABLE cycles == `cpu_base_cycles[]` |
| `tests/test_cpu_diff.py` + `tests/c/cpu_diff.c` | **Differential test:** every opcode's emitted C vs `cpu_interp_step()` from the same random CPU/RAM state (registers, flags, RAM, cycles, PC). Needs gcc; links `src/` without `runner.c`/SDL. |

Known interpreter gaps are listed in `KNOWN` in `tests/test_cpu_diff.py` (with reasons) and in `docs/STATUS.md`; a known bug with a pending fix is an `@unittest.expectedFailure` test. When you fix one, remove its entry — an "unexpected success" means the entry is stale.

---

## PPU Timing (beam path, `ppu.c`)

- 341 dots/scanline, 262 scanlines/frame; VBlank at scanline 241; odd-frame dot skip on the pre-render line when rendering.
- Pixel output at dots 1–256 → `x = dot - 1`.
- BG tile fetch happens **before** the pixel block on reload dots (9, 17, … 257) to avoid 1-pixel gaps at tile boundaries.
- The `--interp=fceux` backend does not use this per-dot path: it renders lazily per line (`fceux_*` in `ppu.c`), driven by `LineUpdate`-style triggers on `$200x` accesses — see `docs/investigations/battletoads.md`.

---

## State Variables

```c
// src/include/cpu.h
CPU cpu;                      // A, X, Y, SP, PC + flags N V D I Z C (get_P/set_P)
uint32_t g_cpu_cycles;        // cycles of the current step; reset after PPU/APU step in main loop
uint64_t g_total_cpu_cycles;  // running total (excludes DMC stall cycles)

// src/include/interrupts.h
volatile int g_nmi_pending;
volatile int g_irq_pending;

// src/include/ppu.h
PPU ppu;                      // .scanline, .cycle (dot), .v_addr, .regs[], .framebuf[], .frame_odd

// src/include/memory.h
int g_lag_flag;               // 1 = no controller read this frame (lag metric)
```

---

## Mapper Support Summary

| # | Name | PRG Banks | CHR | Notes |
|---|------|-----------|-----|-------|
| 0 | NROM | Fixed 16/32KB | Fixed | Simplest — fully recompilable |
| 1 | MMC1 | 16KB switchable | 4/8KB switchable | Shift register writes; switchable code → interpreter |
| 2 | UNROM | 16KB switchable + fixed last | Fixed | Per-bank recompilation (`func_bN_XXXX`) |
| 3 | CNROM | Fixed | 8KB switchable | CHR only switching |
| 4 | MMC3 | 8KB granularity | 2/1KB granularity | Scanline IRQ; switchable code → interpreter |
| 5 | MMC5 | mode 2: 8KB×4 + fixed last | 1KB×8 sprites / 1KB×4 BG | PRG mode 3 (32KB switchable) not yet tested — TODO: find ROM (Just Breed, Uncharted Waters, Getsu Fuuma Den) |
| 7 | AxROM | 32KB switchable | — | One-screen nametable; per-bank recompilation (`func_bN_XXXX`) |

---

## Runtime Controls

| Key | Action |
|-----|--------|
| F5 | Save state |
| F8 | Load state |
| F11 | Toggle fullscreen |
| F12 | Screenshot (`screenshot_<ticks>.png`) |
| Arrows / Z / X / Enter / Right Shift | D-pad / A / B / Start / Select |
| Tab | Toggle widescreen |
| ESC | Quit |

## CLI Flags

`bin/GAME --help` prints the authoritative list. The ROM is embedded; a positional `rom.nes` argument is accepted and ignored.

| Flag | Description |
|------|-------------|
| `-h`, `--help` / `-v`, `--verbose` | Usage / print active configuration at startup |
| `--scale N` / `--speed N` | Window scale / fast-forward (N frames per shown frame) |
| `--headless` | Run without SDL window (for automated testing); enables learning mode automatically |
| `--interp` / `--interp=beam` | Use pure CPU interpreter (beam-accurate per-dot PPU) instead of recompiled code |
| `--interp=fceux` | FCEUX-faithful playback backend (lazy line rendering, FCEUX DoLine timing) — the demo-sync reference path |
| `--interp=fceux_vendor` | GPL FCEUX x6502 differential oracle — only when built with local `nogpl/` sources (`INTERP=1`) |
| `--playback fm2/X.fm2` | Replay FM2 input file |
| `--dump-frames out.txt` | Write per-frame CRC32 hashes of the **framebuffer** (cosmetic comparison with FCEUX). For demo-sync debugging prefer the lag+RAM metric — see [`docs/sync-methodology.md`](docs/sync-methodology.md). |
| `--dump-sync out.txt` | Write per-frame `frame lag lagcount djb2(RAM $0000-$07FF)` — the **lag-sequence metric** (primary demo-sync signal). lag and RAM are captured together at the VBL boundary. Use via `tools/verify_all.sh`. |
| `--frames N` | Stop after N frames |
| `--seconds N` | Run for N seconds (headless without playback) |
| `--screenshot path.png` | Save screenshot on exit |

`--interp` is useful for isolating recompiler bugs from PPU/mapper bugs: if a game diverges in recompiler mode but matches FCEUX in interpreter mode, the bug is in the recompiler (code generation). If both modes diverge equally, the bug is in PPU/mapper/timing emulation.

---

## Common Tasks for AI Agents

### Adding a new mapper
Edit `src/mapper.c` and `src/include/mapper.h`. Follow existing mapper pattern: implement `mapper_write()` bank switching and update `mapper_init()`.

### Fixing a PPU rendering bug
Work in `src/ppu.c`. Key timing: 341 dots/scanline, 262 scanlines/frame. Pixel output at dots 1–256. VBlank starts scanline 241. Decide first which backend is affected (beam per-dot path vs `fceux_*` lazy renderer) — see "PPU Timing" above.

### Adding a new 6502 opcode to the interpreter
Edit `src/cpu_interp.c`. All opcodes follow the same pattern: decode addressing mode, execute, update flags, increment PC, accumulate cycles.

### Adding a new opcode to the recompiler
Edit `tools/nesrecomp.py` in the instruction emission section. Match the C pattern used in `cpu_interp.c`.

### Debugging a dispatch miss
Enable `RECOMP_LEARN=1`, run headless, check the generated `cfg/NesGame.cfg` for new addresses. Re-run `make GAME=NesGame`.

### Investigating cycle accuracy
Every instruction must: (a) increment `g_cpu_cycles` by the correct cycle count including page-cross / taken-branch penalties (emitter: `Op.page_cross`, branch emission; interpreter: per-opcode `cycles=`), (b) return from the recompiled function (or step from interpreter) so the main loop can step PPU/APU. `cpu_base_cycles[256]` in `cpu_interp.c` is the canonical base table (verified identical to FCEUX `CycTable`).

---

## Battery Saves (SRAM)

`EMBEDDED_BATTERY` flag emitted by `extract_rom_data.py` (iNES header byte 6 bit 1).
On startup: `sram_load()` reads `<bin-dir>/sav/GAME_battery.sav` into `sram[]`.
On exit (`runner_quit`): `sram_save()` writes `sram[]` to the same path.
No-op at compile time when `EMBEDDED_BATTERY == 0`.
SIGTERM and SIGINT both trigger a clean exit so the save is not lost.
During `--playback` / `--dump-sync` / `--dump-frames` SRAM is **hermetic** (never loaded or saved) — see `docs/investigations/timing-fixes.md`.

---

## Demo Sync — Essentials

Full methodology (read before debugging any desync): [`docs/sync-methodology.md`](docs/sync-methodology.md). Step-by-step procedure: [`.claude/skills/debug-desync/SKILL.md`](.claude/skills/debug-desync/SKILL.md).

- **Metrics, in priority order:** (1) lag sequence / cumulative lag drift — the sync verdict; (2) RAM hash `$0000-$07FF` — tightening metric; (3) framebuffer hash — cosmetic only.
- **One command:** `tools/verify_all.sh [GAME]` (`REBUILD=0` skips make, `FCEUX=0` uses cached refs only).
- **Build with `make GAME=X INTERP=1`** for all sync work (skips the giant generated `_full.c`).
- **Verify the ROM first:** the FM2 `romChecksum` must match `rom/GAME.nes` (MD5 without the 16-byte iNES header).
- **FCEUX is not ground truth for hardware timing** — arbitrate timing disputes against Mesen / test ROMs.
- **The RAM hash includes the stack page** — a one-frame mismatch confined to `$01xx` that re-converges is not a bug.
- **A fidelity change that measures neutral may be unobservable in the current structure**, not wrong — re-test after structural changes.

---

## Documentation Map & Session Workflow

| Where | What goes there |
|-------|-----------------|
| `AGENTS.md` (this file) | Stable facts every session needs: conventions, architecture, build, CLI, gotchas. Short. |
| `CLAUDE.md` | Claude Code entry point: imports this file and `docs/STATUS.md`. Claude-specific notes only. |
| [`docs/STATUS.md`](docs/STATUS.md) | **Current state:** per-game accuracy, open problems, next steps, recently done. |
| [`docs/investigations/`](docs/investigations/) | Dated investigation logs: hypotheses, measurements, refutations, methods that worked. |
| [`docs/decisions/`](docs/decisions/) | ADRs — short "decided X because Y, rejected Z" records. |
| `docs/*.md` | Reference & design docs (methodology, FCEUX Lua, bug checklist, verification, designs). |
| `.claude/skills/` | Step-by-step procedures loaded on demand (e.g. `debug-desync`). |

**Session workflow:**
1. **Start:** read `docs/STATUS.md`; before touching a subsystem, read the matching investigation log (e.g. Battletoads → `docs/investigations/battletoads.md`) so you do not re-derive refuted hypotheses.
2. **During:** log non-trivial investigations in `docs/investigations/<topic>.md` with dated `##` sections. Never silently delete a refuted hypothesis — mark it ~~struck~~ / **REFUTED** with the date and the evidence.
3. **End (before finishing a task):** update `docs/STATUS.md` — accuracy numbers (from `tools/verify_all.sh`), open problems, next steps, and a one-line entry under "Recently done". Record any architectural choice as an ADR in `docs/decisions/`.
4. **Promote to this file** only facts that would cause mistakes in *any* future session.

---

## Reference Documents

| Doc | Contents |
|-----|----------|
| [`docs/STATUS.md`](docs/STATUS.md) | Current accuracy table, open problems, next steps. |
| [`docs/sync-methodology.md`](docs/sync-methodology.md) | Demo-sync metrics, mandatory diagnostic order, `verify_all.sh`, FM2/ROM check, reference-emulator caveat, controller profile. |
| [`docs/address-collection.md`](docs/address-collection.md) | How demos drive dynamic address discovery: dispatch-mode requirement, iterative learning loop, cfg directives, coverage strategy. |
| [`docs/nesrecomp-bugs.md`](docs/nesrecomp-bugs.md) | Recompiler/emitter bugs + fix checklist. Bugs 1.4/1.5 (page-cross/branch cycles) are prerequisites for timing accuracy. |
| [`docs/nesrecomp-verification.md`](docs/nesrecomp-verification.md) | Success criteria L0–L4, comparison methods (frame-hash, trace, sync-point trace B'), coverage strategy, seed trust levels. |
| [`docs/interp-fceux-design.md`](docs/interp-fceux-design.md) | Design of the `--interp=fceux` backend. |
| [`docs/bank-aware-recompilation.md`](docs/bank-aware-recompilation.md) | Plan: recompile switchable PRG banks (start with UNROM/Mermaid). |
| [`docs/fceux-lua.md`](docs/fceux-lua.md) | FCEUX 2.6.6 Lua API: speed modes, exiting, `gui.gdscreenshot()` format, hash-script template. |

External working material (not in the repo; cite section numbers in commits/issues):

| Doc | Contents |
|-----|----------|
| `next-features.md` | Block-boundary yield (perf), DMA/timing anomalies (OAMDMA, DMC steal, A12 filter, controller glitch), observable-equivalence invariant. |
| `fceux-fm2-playback.md` | FM2 semantics from FCEUX source: record=frame model, controller quirks, power-on, lag frames. |
| `mesen-reference.md` | Mesen 2.1.1 power-on (ppuOffset=1), hardware-accurate controller, MSM format, FCEUX↔Mesen 16-point comparison. |
| `fceux-lua-dump.md` | VERIFIED FCEUX Lua API for lag+RAM dumping (not framebuffer): ready script, djb2 hash, comparison protocol with interpretation table. |
| `ppu-and-fm2-playback.md` | Summary: Mesen vs FCEUX new/old PPU, demo replay by layers (semantics→power-on→controller→PPU timing), §6.0 the metric problem. |
