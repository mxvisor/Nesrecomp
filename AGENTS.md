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

```bash
# Run headless and log missed addresses
RECOMP_LEARN=1 ./bin/NesGame --headless --seconds 30

# Recompile with discovered addresses written to cfg/NesGame.cfg
make ROM=rom/NesGame.nes GAME=NesGame
```

Repeat until no new misses. If an FM2 file exists for the game, always prefer it over a timed headless run — it covers far more code paths and terminates automatically when playback ends:

```bash
# Preferred: FM2 playback (terminates when done, covers all code paths in the recording)
./bin/NesGame --headless --playback fm2/NesGame.fm2

# Fallback: timed headless run (no FM2 available)
RECOMP_LEARN=1 ./bin/NesGame --headless --seconds 30
```

Learning mode is enabled automatically in headless mode. After the run, re-run `make GAME=NesGame` to rebuild with the new addresses.

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

## TODO / Planned Features

### ~~Battery-backed SRAM persistence~~ ✅ implemented
`EMBEDDED_BATTERY` flag emitted by `extract_rom_data.py` (iNES header byte 6 bit 1).
On startup: `sram_load()` reads `<bin-dir>/sav/GAME_battery.sav` into `sram[]`.
On exit (`runner_quit`): `sram_save()` writes `sram[]` to the same path.
No-op at compile time when `EMBEDDED_BATTERY == 0`.
SIGTERM and SIGINT both trigger a clean exit so the save is not lost.

### MMC5 PRG mode 3
PRG mode 3 (single switchable 32 KB bank) is not yet tested. Need a ROM that uses it.
Candidate games: **Just Breed**, **Getsu Fuuma Den**, **Uncharted Waters** (all Japan, Koei).

### AxROM (mapper 7)
AxROM is listed in `mapper.c` / `mapper.h` but has never been tested against a real game.

**Implementation plan:**
- Add mapper 7 case to `mapper_write()`: bits 0–3 select 32 KB PRG bank, bit 4 selects one-screen nametable (lower or upper)
- Add nametable mirroring to `ppu.c` / `memory.c`: single-screen mode using `$2000` or `$2400` depending on mapper bit
- Test with a known AxROM game (e.g. **Battletoads**, **Jeopardy!**, **Time Lord**)
- Add to Tested Games table in README once verified
