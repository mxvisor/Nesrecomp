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

> **Full guide: [`docs/address-collection.md`](address-collection.md)** — what
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
| `--interp` | Use pure CPU interpreter instead of recompiled code |
| `--playback fm2/X.fm2` | Replay FM2 input file |
| `--dump-frames out.txt` | Write per-frame CRC32 hashes of the **framebuffer** (cosmetic comparison with FCEUX). For demo-sync debugging prefer the lag+RAM metric — see "Synchronization Methodology" below. |
| `--dump-sync out.txt` | Write per-frame `frame lag lagcount djb2(RAM $0000-$07FF)` — the **lag-sequence metric** (primary demo-sync signal). lag and RAM are captured together at the VBL boundary. Use via `tools/compare_lags.sh`. |
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

### AxROM (mapper 7) — Battletoads status

**Startup + copy protection: FIXED** by the unified FM2 timing (see
"Unified FM2 timing + ppudead=1" below). The new FM2 (US Battletoads,
MD5-verified, plays in FCEUX) used to desync at frame 4; now it stays in
lag-sync (drift +1) through the whole copy-protection relay and level-1
start — **~5592 frames**.

How it was found: a bank-switch write trace (ours vs an FCEUX
`memory.registerwrite` Lua trace) showed the copy-protection relay is
**bit-identical** to FCEUX (same $FFB3/$FFB9/$FFC9/$FFB4 writes/values/
order); the only difference was a one-frame shift from the FM2 frame-1
reset being applied a frame late. Disassembly: reset $FFF2 = `LDA #0;
STA $FFB3; JMP $82A9`, then a two-VBL wait (`$82BA: LDA $2002; BPL`).
The unified record-N-at-frame-start model removed the extra boot
iteration and the relay now lines up. (Ruled out along the way:
illegal-op cycles, soft-reset completeness, blanket pre-load — which
broke Zelda.)

**Remaining: gameplay desync at ~frame 5592.** At 5592 the game leaves
its main loop ($872A wait) for a heavy multi-bank routine (banks 0/1/2/6
— a level transition / data load) that lags for ~9 frames. FCEUX's run
of that routine is 9 lag frames; **ours is 10** (one extra), so we exit
one frame late and the drift becomes +1, then more such heavy routines
each add ~+1. This is the **same class** as Castle3/Mermaid: a 1-frame
slip in a heavy routine on a NewPPU-0 demo — a cycle/timing accuracy gap
(sprite-0 / OAM-DMA / sprite-eval), best arbitrated against Mesen, not a
unique Battletoads bug. RAM can't pinpoint it (phase-unreliable: 3/4800
matches) without the post-NMI RAM-snapshot TODO.

**Earlier title-screen fixes (historical):** the title was previously
stuck; two root fixes were applied:

**Fix 1 — STP dispatch (`tools/nesrecomp.py`):**
When a bank-switch write happens mid-function (e.g. SLO izx in `func_b5_D2B5` writes to ROM),
the real CPU continues execution at the same PC in the *new* bank's code. Our STP handler now
emits `cpu.PC = 0x{addr:04X};` before return so the dispatch loop fetches that address in the
new bank, rather than re-calling the original function entry.

**Fix 2 — Illegal opcodes (`src/cpu_interp.c`):**
ISB ($FF/$FB/$EF/$F7/$F3/$E7/$E3), SLO ($1F/$1B/$0F/$13/$17/$07/$03), and RRA ($7F/$7B/$6F/
$73/$77/$67/$63) were missing from the interpreter. They were treated as 1-byte unknowns,
causing stuck loops (especially ISB abs,X $FF $FF $FF — all-$FF open-bus regions).

**Copy-protection relay (for reference):**
Battletoads uses a multi-stage copy-protection relay: BRK → bank3 IRQ ($FF46) → 13× ISB abs,X
at $FFFF+X (each increments the current bank's IRQ-vector hi byte, triggering a bank switch to
bank0 or bank7) → bank5 ($D2B5) → bank4 ($D2CC) → bank1 ($D2D7) → RTI → BRK cascade in bank1
→ bank0. The relay depends on cpu.X loaded from RAM via LAX izy($FF).

**Remaining known issues (low priority — game runs):**
- Some debug `fprintf(stderr,...)` traces left in `generated/Battletoads_full.c`; strip before
  shipping (they are in the auto-generated file so will disappear on next `make discover`).
- Sprite-0 Y position: OAM[0]Y=$F8 (off-screen) on title — split-screen effects may be off.

### Dead frame / fm2 sync mismatch (TODO)

FCEUX emits **2** gray-screen hashes at startup (ppudead=2). Our emulator emits **1** gray hash
while consuming 2 fm2 inputs. This causes `first_mismatch=2` for **Contraf** (game renders
a non-gray frame where FCEUX still shows the second ppudead gray frame).

FCEUX ppudead loop (from `ppu.cpp`):
```c
if (ppudead) {
    memset(XBuf, 0x80, 256 * 240);
    X6502_Run(scanlines_per_frame * (256 + 85));
    ppudead--;
}
```
Input is applied before each frame including dead frames (`FCEU_UpdateInput()` → `FCEUPPU_Loop()`).

**Fix needed:** emit 2 gray hashes (one per ppudead frame) while consuming 2 fm2 inputs and
advancing the CPU for each. Affected games: **Contraf** (first_mismatch=2), possibly others.
Skip until other accuracy issues are resolved to avoid masking more important divergences.

---

### Battlecity frame-hash accuracy (Mapper 0, NROM-128)

Frame-hash comparison against FCEUX reference (`fm2/Battlecity.hashes`, 46898 frames):

| Cluster | Frames | Count | Description |
|---------|--------|-------|-------------|
| A | 2450 | 1 | Level-transition explosion frame — right half of sprite missing. Cosmetic. |
| B | 5171–5172, 7879–7883, 10541 | ~6 | Similar single/2-frame level-transition glitches. |
| C | 11874–11947 | 74 | Larger block, likely same class of issue. |
| **D** | **12604–46897** | **34293** | **Permanent divergence** — something breaks at frame ~12604 and never recovers. Root cause unknown; needs investigation. |

Screenshots of first diverging frame (cluster A) are in `tmp/screenshots/`.

**Root cause hypothesis for cluster A/B/C:** sprite clipping at level-transition boundary — PPU disables rendering 1 dot too early/late causing one scanline of a sprite to vanish. 1-frame granularity.

**Root cause hypothesis for cluster D:** a game-state variable or timing accumulates drift across multiple levels until it permanently diverges at level ~8-9. Possible causes: RNG seed drift, timer off-by-one, or a specific mapper/PPU edge case triggered only at that point in the FM2.

**Priority:** low for A/B/C (cosmetic, <10 frames total). Medium for D (breaks second half of TAS replay).

---

### Mermaid (UNROM/Mapper 2) — frame-hash accuracy

Frame-hash comparison against FCEUX reference (fm2/Mermaid.hashes, 129000 frames):

- Frames 1–1891: **match 100%** (after palette fix below)
- Frames 1892–end: **~1.6% match** — persistent divergence

**Root cause found:** At frame 1892, game sets RAM[$EE]=$FF to signal level completion and disable PPU rendering (NMI handler at $C000 checks $EE on entry: `LDA $EE; ORA $9A; BNE skip_reenable`). FCEUX sets $EE=$FF during the game loop of frame 1892; our emulator keeps $EE=$00.

Both emulators have identical RAM at frame 1891 end (only 3 stale stack bytes differ: $01F9, $01FA, $01FE). The game-loop code that should set $EE=$FF before VBlank 1892 doesn't trigger in our emulator.

**Hypothesis:** Some switchable-bank code (likely bank 5, address $B2D3–$B2DB: `INC $EE`) runs in FCEUX but not in our interpreter. This could be due to:
- A specific code path in bank 4/5 that evaluates a counter/flag differently
- A timing-sensitive loop that finishes before VBlank in FCEUX but not in ours (due to cycle-counting differences)

**Investigation state:** RAM[$EE] and RAM[$9A] are both $00 at every NMI delivery in our emulator (frames 1889–1895). FCEUX has $EE=$FF at frame 1892 VBlank. The write trace in our emulator only shows: `$EE: 00→01` (INC at $C6EF, bank4) then `01→00` (reset at $C6E5, bank4) — no write to $FF.

**Next steps to fix:**
1. Add instruction-level trace in FCEUX (Mesen trace log) for bank 4–5 code around frame 1892
2. Compare which INC/STA $EE instructions FCEUX executes vs ours
3. Find what flag/counter controls whether the "level complete" code path runs

**Palette implementation (runner.c):** FCEUX_PAL tables are 256-entry. `ppu.indexbuf[i]` stores
`emphasis_range | (color & 0x3F)` where emphasis_range is:
- `0x80`: no PPU emphasis bits (PPU[1] >> 5 == 0) → lookup uses unscaled P64 values
- `0xC0`: all 3 emphasis bits set → 0.75-scaled values (FCEUX XBuf 0xC0 range)
- `0x40`: partial emphasis → approximate 0.75-scaled values

---

### Frame-hash accuracy summary (all games)

Run with `--interp` flag to use pure interpreter (no recompiled code). Comparison vs `fm2/GAME.hashes` (FCEUX reference).

| Game | Mapper | Recompiler | Interpreter | First mismatch | Root cause |
|------|--------|-----------|-------------|----------------|------------|
| Mario | NROM-256 | **99.99%** | **99.99%** | frame 7817 (1), 12594 (1) | 2 isolated mismatches; same in both modes |
| Battlecity | NROM-128 | 26.70% | **99.98%** | recomp: 2450 / interp: 2449 | **Recompiler bug** (cluster D at 12604+) |
| Mermaid | UNROM | 1.61% | 1.61% | frame 1892 | PPU/mapper bug; interp identical → not recompiler |
| Zelda | MMC1 | 0.20% | 0.20% | frame 32 | PPU/mapper bug; 7-frame phase advance vs FCEUX |
| Adventure | CNROM | 4.06% | 3.59% | frame 8 | PPU/mapper bug; both diverge at first content frame |
| Felix | MMC3 | 0.44% | 0.44% | frame 116 | PPU/mapper bug; PRG bank switch timing mid-frame |
| Castle3 | MMC5 | 99.60% | **99.97%** | frame 998 | Slight recompiler issue; mostly PPU/mapper |
| Battletoads | AxROM | 1.08% | **68.03%** | recomp: 172 / interp: 1537 | **Recompiler bug** (copy protection code path) |

**Methodology:** `GAME=X bin/X --headless --interp --playback fm2/X.fm2 --dump-frames out.txt`
Battlecity/Battletoads/Adventure: full-movie run. Others: 3000-frame sample.

**Key insight:** Games where interpreter ≫ recompiler have **recompiler bugs** (wrong code generation).
Games where both modes match have **PPU/mapper/timing bugs** (emulation-level issues).

**Battlecity (NROM):** Interpreter fixes the permanent divergence at frame 12604+ → recompiler emits wrong code somewhere in the NROM-128 address space.

**Battletoads (AxROM):** Interpreter 68% vs recompiler 1% → copy protection ISB/SLO sequences probably have recompiler code generation errors.

**Zelda (MMC1):** Both modes diverge at frame 32 (7-frame phase advance vs FCEUX). PPU/CPU reset timing or MMC1 initial state.

**Adventure (CNROM):** Both modes diverge at frame 8 (first rendered frame). CNROM CHR bank initialization or PPU issue.

**Felix (MMC3):** Both modes diverge at frame 116. PRG bank switch pattern differs from FCEUX — animation advances 1 frame too early. Bug is in PPU timing or NMI handler interaction, not recompilation.

---

### Bank-aware recompilation (switchable PRG banks)

Currently, only the **fixed PRG bank** ($E000–$FFFF or equivalent) is statically recompiled. All switchable bank code falls back to the interpreter. This defeats the purpose of recompilation for most banked games.

**Goal:** fully recompile all PRG banks, including switchable ones.

**Start with UNROM (Mapper 2, game: Mermaid)** — simplest case:
- One switchable slot: $8000–$BFFF (16 KB, selectable from N banks)
- Fixed last bank: $C000–$FFFF
- No CHR switching complexity

**Required changes:**

1. **`tools/nesrecomp.py`**: disassemble each bank independently; emit functions namespaced by bank:
   ```c
   void func_b0_8000(void) { ... }  // bank 0 mapped at $8000
   void func_b1_8000(void) { ... }  // bank 1 mapped at $8000
   ```

2. **`generated/GAME_dispatch.c`**: nested dispatch — outer switch on address, inner switch on active bank:
   ```c
   case 0x8000:
       switch (mapper.prg_bank[0]) {
           case 0: func_b0_8000(); return;
           case 1: func_b1_8000(); return;
           default: cpu_interp_step(); return;
       }
   ```

3. **`mapper.h` / `mapper.c`**: expose `prg_bank[slot]` as a public field so `_dispatch.c` can read it.

4. **`cfg/GAME.cfg`**: extend format to specify per-bank seeds: `extra_func = B:XXXX` (bank B, address XXXX).

**Scope per mapper after UNROM:**
- MMC1 (Zelda): two switchable slots ($8000–$BFFF and $C000–$DFFF)
- MMC3 (Felix): six independently switchable 8 KB windows
- AxROM (Battletoads): single 32 KB switchable slot $8000–$FFFF
- MMC5 (Castle3): four 8 KB slots ($8000–$DFFF) + fixed $E000–$FFFF

---

## FCEUX 2.6.6 Lua API — Frame Capture & Exit (researched from source)

Source: `/home/VisoR/projects/HOME/BATTLE_CITY_ADW/fceux-2.6.6/src/`

### Speed rule

**Always run FCEUX at maximum speed** when capturing sync/frame data. Add `emu.speedmode("nothrottle")` at the top of every Lua script. This is already in `tools/fceux_dump.lua`.

### Speed modes (`emu.speedmode`)

| Mode | Rendering | `gui.gdscreenshot()` | Lua hooks |
|------|-----------|----------------------|-----------|
| `"normal"` | every frame, throttled to 60fps | ✅ valid | every frame |
| `"nothrottle"` | every frame, no throttle (~3200%) | ✅ valid | every frame |
| `"turbo"` | frame-skip | ⚠️ may be stale | every frame |
| `"maximum"` | **SKIPPED** (stub only) | ❌ stale buffer | every frame |

**Rule:** Use `"nothrottle"` for fast capture. Never use `"maximum"` with `gui.gdscreenshot()`.

### Exiting FCEUX from Lua

- `os.exit(0)` — **works immediately**, os library is NOT sandboxed (`luaL_openlibs` loads everything). Best for clean process kill.
- `emu.exit()` — sets `exitScheduled = TRUE`, processed at next frame boundary (not immediate). Prefer `os.exit(0)` when speed matters.
- `--movielength N` CLI flag — calls `exit(0)` directly in `fceu.cpp` after N frames. Zero Lua overhead. Use this when you don't need per-frame hashes.

### `gui.gdscreenshot()` — GD2 format

Returns a Lua string of `11 + 256×240×4 = 245,771` bytes:
- Bytes 0–10: GD2 header (`FF FE`, width(2BE), height(2BE), truecolor(1), bgcolor(4))
- Bytes 11+: pixels as `[A][R][G][B]` per pixel, A=0 means opaque (7-bit alpha)

**Fast CRC32 idiom** — hash entire pixel block in one Lua pass (NOT per-pixel sub()):
```lua
local pixels = gd:sub(12)   -- everything after header (Lua 1-indexed: byte 12 = first pixel A)
local hash = crc32(pixels)  -- one pass over 245,771 bytes
```

Performance note: per-pixel `gd:sub(off, off+2)` in a loop = ~61K string allocs → GC pressure → ~100ms/frame. One `gd:sub(12)` + byte-loop CRC32 = ~5ms/frame.

### Frame hash format compatibility with `--dump-frames`

FCEUX GD2 pixel layout: `[A=0, R, G, B]` big-endian per pixel.
Our framebuf layout: `0xFFRRGGBB` uint32_t (little-endian: bytes B, G, R, A in memory).

**To get matching hashes**, runner.c must output pixels in the same byte order as GD2.
Options:
1. Hash `[0, R, G, B]` in both (add zero alpha byte before each pixel in runner.c)
2. Hash only `[R, G, B]` in both (skip alpha in Lua with per-pixel indexing — slow)
3. **Best:** use FCEUX palette in our emulator so colors match, then hash `[R, G, B]`

**Correct approach for palette-invariant hashes:**
1. Add `uint8_t indexbuf[SCREEN_W * SCREEN_H]` to ppu struct (filled alongside framebuf with `color & 0x3F`)
2. In runner.c `--dump-frames`: convert indexbuf → [0, R, G, B] using FCEUX's palette table (static const in runner.c), hash that
3. In Lua: `gd:sub(12)` already uses FCEUX palette → identical bytes for same NES index

Do NOT change `PALETTE[]` in ppu.c — it is used for display rendering, not hash comparison.

### Correct minimal Lua script template

```lua
local outfile = os.getenv("FRAME_HASH_OUT") or "/tmp/fceux_frames.txt"
local limit   = tonumber(os.getenv("FRAME_LIMIT") or "0") or 0
local f = assert(io.open(outfile, "w"))

emu.speedmode("nothrottle")

-- CRC32 (Lua 5.1, bit library)
local bit = require("bit")
local band, bxor, rshift = bit.band, bit.bxor, bit.rshift
local crc_tab = {}
for i = 0, 255 do
    local c = i
    for _ = 1, 8 do
        c = band(c,1)==1 and bxor(rshift(c,1), 0xEDB88320) or rshift(c,1)
    end
    crc_tab[i] = c
end
local function crc32(s)
    local crc = 0xFFFFFFFF
    for i = 1, #s do
        crc = bxor(rshift(crc,8), crc_tab[band(bxor(crc, s:byte(i)), 0xFF)])
    end
    return band(bxor(crc, 0xFFFFFFFF), 0xFFFFFFFF)
end

local frame, done = 0, false
emu.registerafter(function()
    if done then return end
    frame = frame + 1
    local gd = gui.gdscreenshot()
    if gd and #gd > 11 then
        f:write(string.format("%06d %08X\n", frame, crc32(gd:sub(12))))
    else
        f:write(string.format("%06d NOFRAME\n", frame))
    end
    if (limit > 0 and frame >= limit) or movie.mode() == "finished" then
        done = true
        f:close()
        os.exit(0)   -- immediate, os not sandboxed
    end
end)
```

---

## Synchronization Methodology — READ BEFORE DEBUGGING DEMO DESYNC

This section supersedes ad-hoc frame-hash debugging. It encodes the
diagnostic order derived from the reference research (companion docs:
`ppu-and-fm2-playback.md`, `fceux-lua-dump.md`, `mesen-reference.md`,
`next-features.md`). Agents debugging desync MUST follow this order
instead of staring at framebuffer hashes.

### The metric problem (why current frame-hash debugging stalls)

The current `--dump-frames` hashes the **framebuffer** (PPU output).
This is the *worst* metric for demo sync, because the framebuffer is the
bottom of the dependency chain and absorbs every cosmetic PPU difference
that does NOT affect whether the demo desyncs:

```
CPU logic (what the game DECIDED)      ← "does the demo stay in sync" lives here
   ↓ RAM $0000-$07FF (positions, RNG)  ← direct consequence of logic
   ↓ PPU regs / VRAM (what was LOADED)
   ↓ Framebuffer (what was DRAWN)      ← we currently hash HERE (worst)
```

A sprite blinking one frame late, a palette emphasis shade, a 1-pixel
mid-frame scroll — all diverge the framebuffer while the game runs
perfectly. This is why most games in the accuracy table show "many PPU
differences": we measure the layer most polluted by cosmetic noise.

### Correct metrics, in priority order

1. **Lag sequence** (primary). One bit per frame: did the game read
   `$4016/$4017` this frame. Compute: `lag=true` at frame start;
   `lag=false` on controller read **by the game** (not by debug/Lua
   peeks); record bit at frame end. A lag bit divergence = the exact
   frame where input shifted = the demo's death point. Because one FM2
   record = one emulated frame, a single extra lag frame shifts all
   subsequent input by one and kills the run. This is the ONLY point
   worth debugging.
2. **RAM hash `$0000-$07FF`** (secondary). Whether game logic matches
   (positions, counters, RNG). Independent of how the PPU draws.
3. **Framebuffer** (last). Only for cosmetic verification once logic is
   provably in sync.

### Mandatory diagnostic order

1. Check the demo header `NewPPU` flag (see "Reference emulator
   caveat" below). If `0`/absent and the game is timing-sensitive,
   desync is EXPECTED on our cycle-accurate core — do not debug the
   core against this demo.
2. Compare **lag sequences** first. FCEUX side via `emu.lagged()`
   (see `fceux-lua-dump.md` for the verified Lua dump script). First
   lag divergence row = the frame to debug. Everything after it is
   downstream noise.
3. Only if lags match end-to-end: compare RAM hashes. A transient
   (diverge-one-frame-then-rejoin) RAM mismatch with matching lags is
   a hash-sample-phase artifact, NOT a desync — ignore it.
4. Framebuffer last.

### Tooling — implemented

Run the whole comparison with one command:

```bash
make GAME=Battlecity                 # build first
tools/compare_lags.sh Battlecity     # whole movie (auto-caps to FM2 length)
tools/compare_lags.sh Battlecity 300 # first 300 frames (fast iteration)
```

It dumps both sides into `lags/GAME.ours.txt` and `lags/GAME.fceux.txt`
(format `frame lag lagcount djb2(RAM $0000-$07FF)`), then compares
**frame-to-frame** (no offset fitting — fitting an offset hides real
transient divergences) and prints: lag match %, divergent-frame count,
cumulative lag drift, and the first lag divergence with surrounding
context. Exit 0 = lags match end-to-end.

- [x] `--dump-sync out.txt`: emits `frame lag lagcount djb2(RAM
  $0000-$07FF)`. lag and RAM are captured **together** at the VBL
  boundary so they share one timing phase (an earlier version captured
  them at different boundaries, which no constant offset could align).
- [x] Lag flag plumbing: `g_lag_flag` in `memory.c` cleared ONLY by
  game-side `ctrl_read()` (`$4016/$4017`); reset each frame in the FM2
  advance. Debug peeks do not touch it.
- [x] FCEUX-side: `tools/fceux_dump.lua` (`emu.lagged()`,
  `memory.readbyterange(0,0x800)`, djb2, `emu.speedmode("nothrottle")`,
  `os.exit(0)` to stop at movie end). `compare_lags.sh` bakes the OUT
  path and frame cap into a temp copy per run.

### Interpreting the result (cumulative lag drift is the verdict)

The lag sequence rarely matches 100% frame-to-frame even when a demo
plays perfectly, because borderline `wait-for-vblank` frames flip lag
one frame early/late then immediately re-sync (an NMI-moment phase
jitter). The **authoritative** "does the demo stay in sync" signal is
the **cumulative lag total**: if `ours ≈ fceux` (drift 0 or ±1), FM2
input stays aligned end-to-end and the demo plays correctly; only the
jittery frames differ. A *growing* drift = real desync — debug the
first frame where the running totals start to separate.

### Lag-sequence results — all games (interpreter, full movie)

Generated with `tools/compare_lags.sh GAME`. "drift" = cumulative lag
total (ours − fceux); near-zero = FM2 input stays aligned = demo plays
in sync. "f2f" = frame-to-frame lag match (jitter sensitive — low f2f
with near-zero drift just means many transient single-frame flips).

After the **hermetic-SRAM** + **unified FM2 timing** (record N applied at
the START of frame N, like FCEUX) + **ppudead=1** + **odd-frame dot-skip**
+ **DMC steal** fixes:

| Game | Mapper | f2f match | **drift** | verdict |
|------|--------|-----------|-----------|---------|
| Mario | NROM-256 | **100.00%** | **0** | perfect |
| Battlecity | NROM-128 | **100.00%** | **0** | perfect |
| Adventure¹ | CNROM | 99.96% | **−4** | in sync |
| Felix | MMC3 | **100.00%** | **0** | perfect |
| Castle3 | MMC5 | 99.70% | **−641** | in sync² (full 367k playthrough) |
| Mermaid | UNROM | 99.57% | **−368** | in sync² |
| Zelda | MMC1 | **100.00%** | **0** | perfect (was −16771!) |
| Battletoads | AxROM | 11.28% | +69200 | in sync to ~frame 5800³ |

Pre-unification these were 99.7–99.9% with drift ±1; the unified timing
took four of them to **exact** 100%/0 and fixed Battletoads' startup.

¹ Adventure measured over first 20000 frames (movie is 240k).
² Castle3/Mermaid: in sync with a bursty **slow drift** from heavy-scene
  slowdown FCEUX models and we don't (Castle3 −641 over 367k = 0.17%;
  Mermaid −368 over 129k). Both NewPPU 0 — needs a Mesen cross-check
  before treating as our bug. Not DMC (verified). Low priority.
³ Battletoads: now **in sync to ~frame 5800** (startup + copy-protection
  relay fixed by the unified FM2 timing — previously desynced at frame 4).
  A separate gameplay desync begins ~frame 5800 (cumulative drift jumps
  from +1 to +347 by frame 6000 and grows ~1/frame — our game lags every
  frame FCEUX doesn't, i.e. it entered a different state). The full-movie
  f2f (11%) is post-5800 noise. Next target: trace the ~5800 divergence.

**Key takeaways:**
- The framebuffer accuracy table (further below) badly under-reported
  sync — it measured cosmetic PPU noise, not whether the demo stays in
  sync. By the lag metric **7/8 games play in sync** (4 of them bit-exact
  100%/drift-0), and Battletoads' startup is now in sync too.
- The decisive fixes, in order of impact: (1) **hermetic SRAM** (don't
  load a stale battery save over power-on RAM); (2) **unified FM2 timing**
  — apply movie record N at the START of frame N (pre-load record 1, then
  tick record N+1 at each frame end) exactly like FCEUX, instead of the
  old VBL-end tick that applied record N one frame late; (3) **ppudead=1**
  warm-up, the correct value under the unified model. Together these took
  Mario/Battlecity/Felix/Zelda to exact 100%/0 and fixed Battletoads'
  copy-protection startup (was desyncing at frame 4).
- Remaining: **Battletoads** gameplay desync at ~frame 5800 (startup
  fixed); **Castle3/Mermaid** slow drift from old-PPU heavy-scene slowdown
  (needs a Mesen arbiter). Both are downstream of correct startup sync.

### Known limitation — RAM hash phase (TODO: post-NMI snapshot)

`--dump-sync` snapshots RAM at the VBL boundary, which is BEFORE the
current frame's NMI handler runs; FCEUX's `registerafter` snapshots
AFTER it. Most games rewrite page-0 RAM in the NMI handler, so the two
snapshots differ by one NMI handler's worth of writes EVERY frame — a
sub-frame phase difference that no integer frame offset can cancel
(Battlecity RAM matches only ~17% at its best offset despite lags being
in sync). Therefore **RAM% is currently informational only**; lag is
the trusted metric. To make RAM directly comparable, capture the sync
snapshot when the NMI handler returns (watch for `cpu.SP` returning to
its pre-NMI value), not at the VBL boundary. Until then do not treat a
low RAM% as logic drift when the cumulative lag drift is ~0.

### Unified FM2 timing + PPU warm-up (ppudead=1) — implemented

**The single biggest sync fix.** FCEUX applies each movie record at the
START of its frame: `FCEU_UpdateInput()` latches controllers + processes
reset/power commands, THEN `FCEUPPU_Loop()` emulates the frame. So record
N drives frame N.

Our loop ticks the record at the VBL that ENDS a frame, which made record
N drive frame N+1 — one frame late. Harmless for steady-state controller
input on most games (the poll happens in the next frame's NMI handler
anyway), but wrong for a frame-1 reset: the game booted once, then got
reset and booted again (an extra startup iteration → Battletoads desync).

**Fix (runner_run):** pre-load record 1 before the loop (apply its
controllers + reset, reset the lag flag), then the in-loop tick at each
frame end advances to record N+1. Net: frame N consumes record N, exactly
like FCEUX. Mid-movie resets are handled the same way (applied before the
frame they belong to).

**PPU warm-up:** `ppu.c` gates the scanline-241 VBL/NMI block on
`g_ppudead`; `runner.c` decrements it once per frame AFTER the dumps read
it (keep the decrement last — an earlier ordering bug made frame 2 not
gray → 36% framebuffer). Under the unified timing the correct value is
**`ppudead=1`** (not 2 — the old value compensated for the late-input
model). 

**Result (full-movie lag vs FCEUX):** Mario/Battlecity/Felix/Zelda go to
**exact 100% / drift 0**; Adventure 99.96%; Battletoads' startup +
copy-protection now in sync (was desyncing at frame 4 → now ~5800).
Castle3/Mermaid stay in sync with their own slow heavy-scene drift. This
replaced the previous hermetic-SRAM+ppudead=2 result (99.7–99.9%, ±1).

**Still TODO (optional, Mesen-style):** drop `$2000/$2001/$2005/$2006`
writes during the ~29658-cycle warm-up window. Not needed for lag sync.

### Odd-frame dot skip (NTSC) — implemented

`ppu.c` now skips the idle dot at (scanline 261, dot 340) on odd frames
when rendering is enabled, so frames alternate 89342/89341 dots
(29780.5 CPU cyc avg) instead of always 89342. Implemented in the dot
advance block via `ppu.frame_odd` (toggled at the 261→0 wrap).

**Result:** improved **Mermaid** (lag drift −137 → **−26**); neutral on
the 6 in-sync games. Battletoads' lag number swung (69%→11%) but it is
already desynced at frame 4 (copy protection) so its lag count is noise
— two different desynced trajectories, not a real regression. The skip
is hardware-correct and FCEUX old-PPU also models it, hence the Mermaid
gain toward the reference.

### DMC DMA cycle stealing — implemented

Models the ~4-cycle CPU stall per DMC sample-byte fetch: `apu_step()`
accumulates the stolen cycles in `g_dmc_stall` (file-scope in apu.c,
referenced via inline `extern` in runner.c so it does NOT touch a shared
header / trigger giant generated-file rebuilds), and `runner_run()`
drains them by advancing PPU/APU without running CPU.

First validated on the **full 367k Castle3 playthrough** which drives DMC
hard (1.35M byte fetches): drift −825 → **−793**. Marginal because DMC
steal is inherently small (~15 cyc/frame even at that fetch rate), so it
can't explain Castle3's heavy-scene slowdown — but it is hardware-correct
and **neutral on non-DMC games** (`g_dmc_stall` stays 0; verified
on/off-identical on Battletoads/Mermaid). Kept for correctness.

### FM2 compatibility — verify ROM before trusting a demo

An FM2 only plays correctly on the exact ROM it was recorded against.
The header's `romChecksum base64:...` is the **MD5 of the ROM minus the
16-byte iNES header**. Check it before any comparison:

```bash
grep '^romChecksum' fm2/GAME.fm2 | sed 's/.*base64://' | base64 -d | xxd -p
tail -c +17 rom/GAME.nes | md5sum            # must match
```

Two wrong Castle3 demos were hit this way: one for *Akumajo Densetsu (J)*
(VRC6 / mapper 24 — different game, unsupported mapper), and the old US
demo. The current `fm2/Castle3.fm2` (US Castlevania III, 367k frames)
matches `rom/Castle3.nes` (`bfc4d979…def4f`). Also confirm the demo
actually plays in FCEUX itself — a mismatch there means the FM2/ROM pair
is wrong, not our emulator.

### Hermetic SRAM during playback/dump (implemented)

Battery-backed games (e.g. Zelda) were loading `bin/sav/GAME_battery.sav`
(a save from a previous run) over the cleared power-on SRAM, booting the
game into a different state than the FCEUX movie (recorded from power-on)
→ instant desync. Fix: `g_hermetic` flag (set when `--playback` /
`--dump-sync` / `--dump-frames`) skips `sram_load()`/`sram_save()` so the
run starts from clean power-on SRAM and never persists it. Normal play
still loads/saves the battery. This cut Zelda's startup offset +8→+1.

### Interpreter-only build (`make GAME=X INTERP=1`)

For demo-sync work everything runs under `--interp`, so the recompiled
code is **never executed** (`runner.c`: `if (g_interp_mode) cpu_interp_step()
else call_by_address()`). `INTERP=1` links the tiny `src/stub_full.c`
(`call_by_address → cpu_interp_run`) instead of the generated
`_full.c`/`_dispatch.c`, and skips the `discover` step entirely (no cfg /
extra_func / bank-aware needed).

**Why it matters:** bank-aware games generate enormous `_full.c` files
(Mermaid UNROM ≈ 24500 functions); compiling that with `-O2` exhausts
RAM. `INTERP=1` drops Mermaid's build peak from OOM to ~53 MB and is
byte-identical at runtime under `--interp` (verified: Battlecity/Mermaid
lag results unchanged). **Use `INTERP=1` for all `compare_lags.sh`
testing.** Editing a shared header (e.g. `apu.h`) under a normal build
forces recompiling the giant `_full.c` — another reason to use INTERP.

TODO (separate): the giant generated `_full.c` is a recompiler
scalability problem (compile time/RAM). Options: compile generated files
at `-O1`, or split into multiple translation units.

### Reference emulator caveat (resolves many "PPU bugs")

The accuracy table's "PPU/mapper bug; interp identical" rows may not be
our bugs at all — they may be **FCEUX old-PPU inaccuracies** we are
chasing:

- FCEUX default PPU is **scanline-based, not cycle-accurate**. Its NMI
  moment, sprite-0 dot, and A12 edges differ from hardware. A demo
  recorded on it is correct *relative to old PPU*, and our
  cycle-accurate core is allowed to diverge — that is not our bug.
- FCEUX **new PPU** (`NewPPU 1` in the FM2 header) is dot-level but has
  empirical constants (notably NMI fires ~20 dots into scanline 241,
  tuned to make Marble Madness work, vs hardware cycle 1). Details in
  `ppu-and-fm2-playback.md` §3.
- **Mesen 2** is the cycle-accuracy reference. For timing disputes,
  arbitrate against Mesen or a blargg/nesdev test ROM — NOT against
  FCEUX. If our core matches Mesen but not FCEUX, we are right and the
  FM2 is old-PPU-incompatible at the timing layer.
- Action: when a "PPU bug" is suspected, FIRST check `NewPPU` flag,
  THEN cross-check the disputed frame in Mesen before touching `ppu.c`.

### Re-interpreting the accuracy table with this methodology

The table classifies by `recomp vs interp` (which isolates recompiler
bugs). Now add the lag/RAM axis to isolate the rest:

- **Battlecity cluster D, Battletoads** — recomp ≪ interp → recompiler
  code-gen bugs. Confirmed correct classification; debug
  `tools/nesrecomp.py` emission. (Battletoads largely fixed per AxROM
  notes; cluster D still open.)
- **Mermaid, Zelda, Adventure, Felix** — recomp == interp → NOT
  recompiler. Re-test these with the lag-sequence metric: many may be
  cosmetic framebuffer divergence with intact lag sequences (i.e. the
  demo actually stays in sync and only the picture differs). Mermaid's
  `$EE` finding (frame 1892) is a real logic divergence — that one will
  show as a RAM-hash divergence with a preceding lag divergence; trace
  the first lag divergence, expect NMI-moment or cycle-count (page-cross
  / branch, bugs 1.4/1.5) as root cause.

### Most likely root cause of lag divergence (debug here first)

When lag sequences diverge, the game did a different amount of work per
frame. Ranked causes:

1. **NMI moment.** If VBlank/NMI is set on a different dot than the
   reference, the game's `wait-for-vblank` loop exits after a different
   instruction count → different per-frame budget → borderline frames
   lag differently. Causes #1. Consider a configurable `nmi_delay_dots`
   compat knob (hardware/Mesen = cycle 1; FCEUX-compat = tune to match
   lag sequence on a known demo).
2. **CPU cycle accuracy.** Unaccounted page-cross and taken-branch
   cycles (bugs 1.4/1.5 in `nesrecomp-bugs.md`) accumulate per-frame
   budget error. Cheapest to fix — do FIRST.
3. **DMC cycle stealing** if DPCM active (see `next-features.md` §6.2).

### Format strategy (FM2 vs MSM) — discovery vs verification

- **Discovery (collect addresses for recompilation): use FM2.** Reason:
  full playthroughs exist only in FM2/bk2 (TASVideos). Desync tolerance
  is high — even a desynced tail still executes real game code and
  yields valid addresses (a dynamic miss is almost never false: if the
  CPU jumped there, it is code). Tag discovery-log addresses with a
  sync marker (lag-match up to frame N) so post-desync addresses are
  known to be off-route but still valid as code.
- **Verification (prove core correctness): use Mesen.** Record short
  MSM in Mesen (cycle-accurate, hermetic settings) OR feed the same
  input table to both via Lua `setInput` in the `inputPolled` callback
  (scanline 241 — verified point). MSM playback is GUI-only; parse the
  MSM yourself (ZIP + Input.txt, button order `UDLRSsBA` — differs from
  FM2 `RLDUTSBA`). Details: `mesen-reference.md`.
- **Do NOT** rely on converting FM2→full playthrough on Mesen: timing
  layer (old PPU) makes long runs desync. Conversion is fine as an
  *input source* for short differential core checks, not as a way to
  replay a whole demo.

### Controller compatibility profile (FM2 playback)

For bit-exact FM2 replay, the controller must mimic **FCEUX, not
hardware** (verified in FCEUX source, see `fceux-fm2-playback.md` §4):
read while strobe high SHIFTS the register (hardware reloads); open bus
is `DB & 0xC0`; DPCM controller glitch is NOT emulated by FCEUX. Keep a
`controller_profile` switch: FCEUX-FM2 profile for replay, Hardware
profile (reload on strobe, console-model open-bus mask, glitch on) for
Mesen/hardware verification. Mixing profiles causes subtle desync on
games with non-standard polling.

---

## Companion Reference Documents

Detailed research backing the methodology above (kept outside the repo
as working material; cite section numbers in commits/issues):

| Doc | Contents |
|-----|----------|
| [`address-collection.md`](address-collection.md) | **(in-repo)** How demos drive dynamic address discovery: dispatch-mode requirement, iterative learning loop, cfg directives, coverage strategy. |
| `nesrecomp-bugs.md` | Recompiler/emitter bugs + fix checklist. Bugs 1.4/1.5 (page-cross/branch cycles) are prerequisites for timing accuracy. |
| `next-features.md` | Block-boundary yield (perf), DMA/timing anomalies (OAMDMA, DMC steal, A12 filter, controller glitch), observable-equivalence invariant. |
| `nesrecomp-verification.md` | Success criteria L0–L4, comparison methods (frame-hash, trace, sync-point trace B'), coverage strategy, seed trust levels. |
| `fceux-fm2-playback.md` | FM2 semantics from FCEUX source: record=frame model, controller quirks, power-on, lag frames. |
| `mesen-reference.md` | Mesen 2.1.1 power-on (ppuOffset=1), hardware-accurate controller, MSM format, FCEUX↔Mesen 16-point comparison. |
| `fceux-lua-dump.md` | VERIFIED FCEUX Lua API for lag+RAM dumping (not framebuffer): ready script, djb2 hash, comparison protocol with interpretation table. |
| `ppu-and-fm2-playback.md` | Summary: Mesen vs FCEUX new/old PPU, demo replay by layers (semantics→power-on→controller→PPU timing), §6.0 the metric problem. |

**Priority entry point for current desync work:** `ppu-and-fm2-playback.md`
§6.0 (switch metric to lags+RAM) → `fceux-lua-dump.md` (dump lag log) →
find first lag divergence → almost certainly NMI moment or cycle counts
→ `nesrecomp-bugs.md` bugs 1.4/1.5.
