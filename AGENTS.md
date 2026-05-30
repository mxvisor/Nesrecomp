# AGENTS.md — NESRecomp Project Guide for AI Agents

## Conventions

- **Commit messages**: always in English
- **Code comments**: always in English
- **AGENTS.md / README**: English
- **Commits**: never commit without explicit user confirmation

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

Indirect JMPs (`JMP ($XXXX)`) are **not traceable statically** — handled at runtime by interpreter.

MMC3 switchable banks ($8000–$BFFF) are skipped during discovery — handled by `cpu_interp_run()`.

After BFS, a second pass scans for orphan code islands (up to 3-byte gaps after terminators).

### Config Seeds

`cfg/NesGame.cfg` can add extra seeds:
```
extra_func = 8120
extra_func = 81A5
```

These come from learning mode or manual analysis.

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

Repeat until no new misses. FM2 TAS files help cover more code paths:

```bash
./bin/NesGame --playback fm2/NesGame.fm2 --headless
```

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
# Full recompile (ROM → generated C → binary)
make ROM=rom/NesGame.nes GAME=NesGame

# Recompile C only (after editing source, no ROM re-disassembly)
make compile GAME=NesGame

# Cross-compile for Windows
make CROSS=1 GAME=NesGame

# With ca65 assembly labels
make ROM=rom/NesGame.nes GAME=NesGame ASM=NesGame.asm
```

Output: `bin/NesGame` (Linux) or `bin/NesGame.exe` (Windows).

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
