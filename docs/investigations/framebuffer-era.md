# Framebuffer-hash era (pre lag-metric) — legacy notes

> Moved from `AGENTS.md`. **Historical:** these results were measured with `--dump-frames` (framebuffer CRC), which the [sync methodology](../sync-methodology.md) now ranks as the least reliable metric. Several items may be obsolete; re-check with `tools/verify_all.sh` before acting on them.

## Dead frame / fm2 sync mismatch (TODO)

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

## Battlecity frame-hash accuracy (Mapper 0, NROM-128)

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

## Mermaid (UNROM/Mapper 2) — frame-hash accuracy

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

## Frame-hash accuracy summary (all games)

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
## Re-interpreting the accuracy table with this methodology

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

