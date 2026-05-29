# AGENTS.md — NESRecomp Project Guide for AI Agents

## Project Overview

NESRecomp is a **static recompiler for NES ROM files**. It converts 6502 machine code into native C functions, then compiles them into a standalone executable. Unknown code paths fall back to a cycle-accurate 6502 interpreter. The result is a per-game binary with the ROM data embedded — no external ROM file needed at runtime.

Supported games: **Battle City, Captain America, Contra Force, Felix the Cat, Super Mario Bros, Super C** (и другие — добавляются через `make recomp`).

---

## Architecture

```
ROM file
   ↓
nesrecomp.py          — BFS static disassembler → emits C
   ↓
generated/
  GAME_full.c         — one void func_XXXX(void) per discovered block
  GAME_dispatch.c     — call_by_address(addr): switch → func or interpreter
  GAME_embedded_data.h/.c — PRG/CHR ROM compiled into binary
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
| `nesrecomp.py` | Static recompiler — BFS discovery + C code emitter |
| `runner.c / runner.h` | Main loop, SDL2 window, input, save states, FM2 TAS, interrupts |
| `memory.c` | CPU address map ($0000–$FFFF): RAM, PPU regs, APU I/O, ROM |
| `cpu_interp.c` | Full 6502 interpreter fallback (step and run modes) |
| `ppu.c / ppu.h` | 2C02 PPU emulation: background, sprites, scanline timing, VBlank |
| `apu.c / apu.h` | APU: pulse ×2, triangle, noise, DMC, audio buffering |
| `mapper.c / mapper.h` | Bank switching: NROM(0), MMC1(1), UNROM(2), CNROM(3), MMC3(4), AxROM(7) |
| `include/interrupts.h` | NMI/IRQ pending flags and vector logic |
| `cfg/GAME.cfg` | Manually/learning-discovered extra entry points |
| `roms/GAME.nes` | NES ROM файлы (не в git — локальные копии) |
| `asm/GAME.s` | Optional ca65 assembly for label-based discovery (не в git) |
| `fm2/GAME.fm2` | Optional FCEUX TAS file for playback-based discovery (не в git) |
| `tools/extract_nes_data.py` | ROM parser → embedded data header/source |
| `generated/` | Auto-generated files — do not edit manually |

---

## Static Recompiler (nesrecomp.py)

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

`cfg/GAME.cfg` can add extra seeds:
```
extra_func = 8120
extra_func = 81A5
```

These come from learning mode or manual analysis.

### Emitted Code Pattern

```c
// GAME_full.c
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

// GAME_dispatch.c
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
RECOMP_LEARN=1 ./bin/BattleCity --headless --seconds 30

# Recompile with discovered addresses written to cfg/BattleCity.cfg
make recomp ROM=/path/to/BattleCity.nes GAME=BattleCity
```

Repeat until no new misses. FM2 TAS files help cover more code paths:

```bash
./bin/BattleCity --playback fm2/BattleCity.fm2 --headless
```

---

## Local Asset Directories (не в git)

Три папки не отслеживаются git — это намеренно (ROM файлы защищены авторским правом, TAS и ASM файлы — рабочие материалы):

### `roms/`

NES ROM файлы для каждой игры. Текущий набор:

| Файл | Игра | Маппер |
|------|------|--------|
| `Battle.nes` | Battle City | MMC1 (1) |
| `Captain.nes` | Captain America and The Avengers | — |
| `Contraf.nes` | Contra Force | — |
| `Felix.nes` | Felix the Cat | MMC3 (4) |
| `Mario.nes` | Super Mario Bros | NROM (0) |
| `Superc.nes` | Super C | — |

Путь к ROM передаётся в `make recomp ROM=roms/GAME.nes GAME=GAME`.

### `fm2/`

FCEUX TAS movie файлы для каждой игры. Используются для learning mode — запускают игру по записанным входам, покрывая больше кода чем ручная игра. Текущий набор: `Battle.fm2`, `Captain.fm2`, `Contraf.fm2`, `Felix.fm2`, `Mario.fm2`, `Superc.fm2`.

```bash
./bin/Felix --playback fm2/Felix.fm2 --headless
```

Формат: FCEUX FM2 (текстовый). Каждая строка `|skip|P1|P2|` описывает один кадр. Парсер в `runner.c → fm2_load()`.

### `asm/`

ca65 assembly файлы с метками для конкретных игр. Используются при рекомпиляции (`--asm`) чтобы дать несреkomp.py имена функций и дополнительные точки входа из ручного дизассемблирования. Текущий набор: `Battle.asm` (только для Battle City).

```bash
make recomp ROM=roms/Battle.nes GAME=Battle ASM=Battle.asm
```

При передаче через `ASM=` Makefile автоматически добавляет префикс `asm/`, так что указывать нужно только имя файла без пути.

---

## Build System

```bash
# Full recompile (ROM → generated C → binary)
make recomp ROM=/path/to/BattleCity.nes GAME=BattleCity

# Recompile C only (after editing source, no ROM re-disassembly)
make GAME=BattleCity

# Cross-compile for Windows
make CROSS=1 GAME=BattleCity

# With ca65 assembly labels
make recomp ROM=... GAME=BattleCity ASM=asm/BattleCity.s
```

Output: `bin/BattleCity` (Linux) or `bin/BattleCity.exe` (Windows).

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
Edit `nesrecomp.py` in the instruction emission section. Match the C pattern used in `cpu_interp.c`.

### Debugging a dispatch miss
Enable `RECOMP_LEARN=1`, run headless, check the generated `cfg/GAME.cfg` for new addresses. Re-run `make recomp`.

### Investigating cycle accuracy
Every instruction must: (a) increment `g_cpu_cycles` by the correct cycle count, (b) return from the recompiled function (or step from interpreter) so the main loop can step PPU/APU.
