# NESRecomp

Static recompilation of NES games to native C code. No interpreter hot loop — every 6502 instruction becomes a C function that yields back to the main loop after each control-flow instruction (JMP, JSR, RTS, RTI, BRK, branches). The interpreter remains only as a fallback for addresses that can't be statically discovered (e.g., indirect JMP targets).

## How it works

1. **Discoverer** (`tools/nesrecomp.py`) — BFS from RESET/NMI/IRQ vectors, follows JSR/JMP abs/branches, groups sequential instructions into functions, emits C code
2. **Emitter** — each function becomes a `void func_XXXX(void)` that sets `cpu.PC`, adds `g_cpu_cycles`, executes the instruction, and `return`s on any control-flow instruction
3. **Dispatch table** — `call_by_address(addr)` switch over all discovered function entries; the default case runs one instruction via the interpreter
4. **Main loop** (`runner.c`) — dispatches one instruction per iteration, then steps PPU (×3) and APU for the accumulated cycles before dispatching the next instruction
5. **ROM data** is embedded at compile time by `tools/extract_rom_data.py` (ROM → C header/source) — no external ROM file needed at runtime

## Features

- **No interpreter hot loop** — every instruction yields to the main loop; PPU/APU stay in sync
- **No runtime ROM dependency** — PRG/CHR data compiled into the binary
- **Yield-based control flow** — JMP, JSR, RTS, RTI, BRK, and all branches set `cpu.PC` and `return`
- **Learning mode** — `RECOMP_LEARN=1` collects dispatch misses into a `.cfg` file for the next recompilation
- **Universal Makefile** — same Makefile works on Linux and Windows (MinGW), with cross-compile support
- **Per-game binary** — `GAME=BattleCity` → `bin/BattleCity`
- **Mapper support** — MMC1, UNROM, CNROM, MMC3 (partial)
- **Save states** — F5 save, F8 load
- **Fullscreen** — F11 toggle
- **Widescreen** — Tab toggle

## Quick Start

### Linux

```bash
# Install dependencies
sudo apt install build-essential libsdl2-dev python3 make git

# Clone and build
git clone <url> nesrecomp
cd nesrecomp

# Place your ROM in rom/MyGame.nes, then:
make GAME=MyGame
./bin/MyGame
```

`ROM` defaults to `rom/$(GAME).nes`. If your ROM is elsewhere, pass it explicitly:

```bash
make GAME=MyGame ROM=/path/to/game.nes
```

If `asm/MyGame.asm` exists it is picked up automatically. The config `cfg/MyGame.cfg` is always used if present.

The `.asm` file is a **ca65 assembly source** (e.g. from a manual disassembly session in Ghidra, IDA, or da65). `tools/nesrecomp.py` extracts every labeled address ≥ `$8000` and adds them as extra BFS seeds — useful for entry points that static analysis can't reach on its own, such as indirect jump targets and data-driven dispatch tables.

You can also pass it explicitly:

```bash
make GAME=MyGame ASM=MyGame.asm
```

### Windows (MinGW)

```bash
# Install MSYS2 with mingw-w64-x86_64-gcc, SDL2, make, python
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 make python

make GAME=MyGame
./bin/MyGame.exe
```

### Cross-compile from Linux to Windows

```bash
sudo apt install gcc-mingw-w64-i686
make CROSS=1 GAME=MyGame
# produces bin/MyGame.exe (Windows PE)
```

## Learning Mode

The static discoverer can't follow indirect jumps (`JMP ($XXXX)`). To find the missing addresses:

```bash
RECOMP_LEARN=1 GAME=MyGame ./bin/MyGame
# Play through the game, then exit with ESC.
# Dispatch misses are saved automatically to cfg/MyGame.cfg
```

Then recompile — the config is picked up automatically:

```bash
make GAME=MyGame
```

Run repeatedly — each session builds on the previous `cfg/MyGame.cfg`. Eventually all reachable code is in the dispatch table.

### Headless Mode

Run without video or audio. The game emulates at full speed, collecting dispatch misses:

```bash
RECOMP_LEARN=1 GAME=MyGame ./bin/MyGame --headless --seconds 30
```

`RECOMP_LEARN=1` is set automatically in headless mode.

### TAS Playback

Replay an FM2 (FCEUX movie) file to exercise code paths from a full playthrough:

```bash
RECOMP_LEARN=1 GAME=MyGame ./bin/MyGame --playback fm2/MyGame.fm2
```

Combine with `--headless` for fully automated discovery:

```bash
GAME=MyGame ./bin/MyGame --headless --playback fm2/MyGame.fm2
```

On each frame (`NMI`) the controller state is loaded from the next FM2 line. The keyboard is ignored during playback. The program exits when all frames are consumed.

## Project Structure

```
runner.c / runner.h   — SDL loop, input, audio, save states
cpu_interp.c          — 6502 interpreter (fallback)
ppu.c / ppu.h         — PPU 2C02 emulation
apu.c / apu.h         — APU emulation (pulse, triangle, noise, DMC)
mapper.c / mapper.h   — mapper logic (MMC1, UNROM, CNROM, MMC3)
memory.c              — CPU address map, controller I/O
include/              — shared headers (cpu, ppu, apu, mapper, interrupts)
generated/            — per-game recompiled C files + embedded ROM data (auto-generated)
tools/nesrecomp.py    — static recompiler / discoverer / C emitter
tools/extract_rom_data.py — ROM parser → embedded C header/source

rom/                  — NES ROM files (.nes) — not tracked by git
cfg/                  — per-game extra entry point config (learning mode output)
asm/                  — ca65 assembly sources for label-based BFS seeding — not tracked by git
fm2/                  — FCEUX TAS movie files for automated discovery — not tracked by git
```

## Supported Mappers

| ID  | Name  | Status     |
|-----|-------|------------|
| 0   | NROM  | Complete   |
| 1   | MMC1  | Complete   |
| 2   | UNROM | Complete   |
| 3   | CNROM | Complete   |
| 4   | MMC3  | Partial    |

## License

MIT
