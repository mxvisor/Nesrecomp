# AGENTS.md — NESRecomp Project Guide for AI Agents

## Conventions

- **Commit messages**: always in English
- **Code comments**: always in English
- **AGENTS.md / README**: English
- **Commits**: never commit without explicit user confirmation
- **After every change to `tools/nesrecomp.py`**: run a full recompile + build on a real ROM before continuing. Do not proceed to the next task until the build is clean. Example:
  ```bash
  make GAME=Battle
  ```
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

Every recompiled function ends with `return` after each control-flow instruction (JMP, JSR, branch taken, RTS, RTI), yielding back to the main loop. This keeps PPU/APU in sync at instruction granularity.

> **Planned change:** instruction-granularity yield negates most of the recompilation speedup (the yield cost replaces the interpreter's dispatch cost). The target architecture is **block-boundary yield** with catch-up, breaking blocks only at control flow and timing-sensitive accesses. This is a large change with a strict correctness invariant (observable equivalence to the per-instruction mode). Do NOT start it before bugs 1.4/1.5 (exact cycle counts) are fixed — block cycle sums depend on them. Full design + checklist: `next-features.md`.

---

## Key Files

| File | Purpose |
|------|---------|
| `tools/nesrecomp.py` | Static recompiler — BFS discovery + C code emitter |
| `tools/asm_parser.py` | ca65 label parser — extracts labeled addresses ≥ $8000 as extra BFS seeds |
| `runner.c / runner.h` | Main loop, SDL2 window, input, save states, FM2 TAS, interrupts |
| `memory.c` | CPU address map ($0000–$FFFF): RAM, PPU regs, APU I/O, ROM |
| `cpu_interp.c` | Full 6502 interpreter fallback (step and run modes) |
| `ppu.c / ppu.h` | 2C02 PPU emulation: background, sprites, scanline timing, VBlank |
| `apu.c / apu.h` | APU: pulse ×2, triangle, noise, DMC, audio buffering |
| `mapper.c / mapper.h` | Bank switching: NROM(0), MMC1(1), UNROM(2), CNROM(3), MMC3(4), AxROM(7) |
| `include/interrupts.h` | NMI/IRQ pending flags and vector logic |
| `cfg/NesGame.cfg` | Manually/learning-discovered extra entry points |
| `rom/NesGame.nes` | NES ROM files (not in git — local copies) |
| `asm/NesGame.s` | Optional ca65 assembly for label-based discovery (not in git) |
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

MMC3 switchable banks ($8000–$BFFF) are skipped during discovery — handled by `cpu_interp_run()`.

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
| `extra_func = XXXX` | Force-add entry point — for state machine handlers, learning mode output |
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

Two strategies, selected per mapper:

| Strategy | Function | When Used |
|----------|----------|-----------|
| Single-step | `cpu_interp_step()` | Most mappers (0,1,2,3,7) — rare misses |
| Full-run | `cpu_interp_run(addr)` | MMC3 only — entire subroutines in switchable banks |

`cpu_interp_step()` executes one instruction and returns to main loop for PPU/APU sync.

`cpu_interp_run(addr)` runs until RTS/RTI returns to original stack depth.

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
make parse_asm  GAME=NesGame ASM=NesGame.asm       # ca65 labels → generated/NesGame_asm_labels.cfg
make discover   GAME=NesGame ROM=rom/NesGame.nes   # BFS + C emit
make compile    GAME=NesGame                       # C → binary (no re-disassembly)

# Cross-compile for Windows
make CROSS=1 GAME=NesGame
```

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

## PPU Timing (Current Branch: fix/ppu-pixel-timing)

The real 2C02 PPU renders the first visible pixel at **dot 12**, not dot 1.

Current fix:
- Shift registers shift at dots 1–256 (pipeline fill)
- Pixel output at dots 12–267 → `x = dot - 12`
- BG tile fetch reordered before pixel block on reload-dots to avoid 1-pixel gaps

If touching `ppu.c`, be aware of this dot offset. The scanline has 341 dots; visible pixels are dots 12–267 → screen columns 0–255.

---

## State Variables

```c
// cpu.h
cpu_t cpu;           // A, X, Y, S, P, PC registers + flags

// Global cycle counter (runner.c)
int g_cpu_cycles;    // reset to 0 after PPU/APU step in main loop

// Interrupt flags (interrupts.h)
int g_nmi_pending;
int g_irq_pending;

// PPU state (ppu.h)
ppu_t ppu;           // includes .scanline, .dot, .frame_buffer[]
```

---

## Mapper Support Summary

| # | Name | PRG Banks | CHR | Notes |
|---|------|-----------|-----|-------|
| 0 | NROM | Fixed 16/32KB | Fixed | Simplest — fully recompilable |
| 1 | MMC1 | 16KB switchable | 4/8KB switchable | Shift register writes |
| 2 | UNROM | 16KB switchable + fixed last | Fixed | |
| 3 | CNROM | Fixed | 8KB switchable | CHR only switching |
| 4 | MMC3 | 8KB granularity | 2/1KB granularity | Scanline IRQ; interpreter for $8000–$BFFF |
| 5 | MMC5 | mode 2: 8KB×4 + fixed last | 1KB×8 sprites / 1KB×4 BG | PRG mode 3 (32KB switchable) not yet tested — TODO: find ROM (Just Breed, Uncharted Waters, Getsu Fuuma Den) |
| 7 | AxROM | 32KB switchable | — | One-screen nametable |

---

## Runtime Controls

| Key | Action |
|-----|--------|
| F5 | Save state |
| F8 | Load state |
| F11 | Toggle fullscreen |
| Tab | Toggle widescreen |
| ESC | Quit |

## CLI Flags

| Flag | Description |
|------|-------------|
| `--headless` | Run without SDL window (for automated testing) |
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
Edit `mapper.c` and `mapper.h`. Follow existing mapper pattern: implement `mapper_write()` bank switching and update `mapper_init()`.

### Fixing a PPU rendering bug
Work in `ppu.c`. Key timing: 341 dots/scanline, 262 scanlines/frame. Pixel output at dots 12–267. VBlank starts scanline 241.

### Adding a new 6502 opcode to the interpreter
Edit `cpu_interp.c`. All opcodes follow the same pattern: decode addressing mode, execute, update flags, increment PC, accumulate cycles.

### Adding a new opcode to the recompiler
Edit `tools/nesrecomp.py` in the instruction emission section. Match the C pattern used in `cpu_interp.c`.

### Debugging a dispatch miss
Enable `RECOMP_LEARN=1`, run headless, check the generated `cfg/NesGame.cfg` for new addresses. Re-run `make GAME=NesGame`.

### Investigating cycle accuracy
Every instruction must: (a) increment `g_cpu_cycles` by the correct cycle count, (b) return from the recompiled function (or step from interpreter) so the main loop can step PPU/APU.

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

Full methodology (read before debugging any desync): [`docs/sync-methodology.md`](docs/sync-methodology.md).

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
