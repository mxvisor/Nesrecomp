# Design: `--interp=fceux` — FCEUX-faithful playback backend

Status: **design** (not implemented). Chosen approach: **A — port FCEUX's
old PPU + IRQ timing** (vendored `fceux/src` is the source of truth), rather
than re-deriving FCEUX's behaviour piecemeal (that repeatedly produced subtle
mismatches: see the catch-up work and the failed sprite-0 deferral).

## 1. Goal / success criteria

- A selectable mode `--interp=fceux` that replays an FM2 **exactly like FCEUX**,
  so frame-perfect demos reach the end.
- **Success:** `tools/verify_all.sh` shows `1stSustDiv=None` for **Battletoads**
  and **Contraf** (the two current desyncs), ideally bit-exact lag for all 11.
- **Non-goals:** speed; the recompiler/dispatch path; the default beam-accurate
  `--interp` and SDL play — all stay untouched. `fceux` mode is a separate code
  path used for demo verification and (later) address collection.

## 2. Why (the gap, proven this session)

Our default PPU is **beam-accurate** (per-dot, lock-step with the CPU). FCEUX
old-PPU (NewPPU 0 — all our demos) is **lazy/timestamp-driven**:
- rendering is advanced only on a PPU-register access or at frame end
  (`FCEUPPU_LineUpdate`), via `lastpixel = (timestamp*48 - linestartts) >> 4`;
- **sprite-0 hit** becomes visible when `lastpixel > sphitx + 16`
  (`CheckSpriteHit`), not at the beam pixel — this is the **Battletoads @5580**
  residual;
- the **MMC3 IRQ** counter clocks on A12 (only while rendering is on), via the
  per-scanline `GameHBIRQHook` — the **Contraf @10565** suspect (mid-frame
  rendering toggles).

Both remaining desyncs are this lazy model. Bolting deferral onto the beam model
fought it; the reliable fix is to run FCEUX's actual algorithm.

## 3. Approach A — two sub-strategies

The PPU is the problem, not the CPU (our CPU is already bit-exact for 4 games).

- **A2 (recommended): our CPU/memory/mapper-banking + FCEUX old-PPU + FCEUX
  MMC3-A12 IRQ.** Keep our cycle-accurate interp; replace only the PPU
  rendering/timing and the scanline-IRQ model with FCEUX's. Smallest new
  surface, reuses our strengths, produces **our** PC stream (good for the
  recompiler later). Risk: the seam `lastpixel = f(our timestamp)` must match
  FCEUX — but our per-instruction cycle counts already match (bit-exact games),
  and unlike the failed experiment we now **fully adopt** the lazy model (no
  beam/deferral hybrid to fight).
- **A1 (fallback): embed FCEUX's old-PPU core driven by FCEUX's x6502.** If the
  A2 seam proves intractable, vendor FCEUX's x6502 + old-PPU loop + the boards
  we need as a self-contained `fceux` core, fed by our ROM/FM2/IO shell.
  Maximally faithful, larger, second CPU in the binary. PC stream is identical
  to A2 (same ROM/execution), so address collection is unaffected.

Decision point for implementation start: **A2 first**, fall back to A1 only if a
specific observable can't be matched without FCEUX's scanline loop.

## 4. FCEUX old-PPU dependency map (what A2 must provide)

From `fceux/src/ppu.cpp` (old path). Left = FCEUX symbol, right = our binding.

| FCEUX needs | Our binding (A2) |
|-------------|------------------|
| `timestamp`, `X.count`, `linestartts` (CPU cycle clock) | a cycle counter advanced by our interp; `linestartts` reset at each scanline start |
| `FCEUPPU_LineUpdate()` on every $2000-$2007 access | call from `memory.c` PPU-reg read/write (we already have a catch-up hook) |
| `RefreshLine(lastpixel)` / `Plinef[]` (BG line buffer + opacity bit 0x40) | new `ppu_fceux.c` line buffer; BG fetch from our `mapper_chr_read` + nametable mirroring |
| `CheckSpriteHit(p)`, `sphitx`, `sphitdata` (sprite-0 hit) | port verbatim; sprite-0 X/pattern from our OAM |
| CHR reads (`VPage`/`vnapage`) | `mapper_chr_read()` (already bank-aware) |
| `GameHBIRQHook()` per scanline (MMC3 A12) | drive our `mapper_scanline()` on the FCEUX schedule (A12-gated by rendering), replacing the dot-260 call |
| nametable/palette | our `ppu.vram` + mirroring |
| `PPU[0..3]`, scroll latches, `$2002` flags, NMI | our `ppu.regs`, `t_addr/v_addr/write_toggle`, VBL/NMI logic (FCEUX's exact dot for VBL set/clear + the suppression we added) |

## 4a. FCEUX old-PPU control structure (key implementation insight)

FCEUX old-PPU is **chunk-driven**, not per-dot. `FCEUPPU_Loop` → `DoLine()`
per visible scanline runs the CPU in timed chunks via `X6502_Run(N)` and fires
PPU events *between* chunks (`fceux/src/ppu.cpp:1314`):

```
DoLine():
  X6502_Run(256)                  # visible part of the scanline
  ... deemph ...
  if (GameHBIRQHook && rendering && (PPU[0]&0x38)!=0x18):
      X6502_Run(6); X6502_Run(4); GameHBIRQHook();   # MMC3 IRQ at ~cyc 266
      X6502_Run(85-16-10)
  else: X6502_Run(6); X6502_Run(85-6-16); [late GameHBIRQHook]
  X6502_Run(16)
```

The lazy `FCEUPPU_LineUpdate` runs *during* an `X6502_Run` chunk when the CPU
touches a PPU register (so `$2002`/sprite-0 reflect the exact mid-scanline
position). VBL set/clear and NMI happen at chunk boundaries in `FCEUPPU_Loop`.

**Implication for us:** the `fceux` backend is a **separate main loop** that
drives OUR `cpu_interp_step` in the same chunk structure (a
`cpu_interp_run_cycles(N)` wrapper that steps until N CPU cycles are consumed),
firing RefreshLine / sprite-0 / `mapper_scanline` (MMC3 A12) / VBL+NMI at the
same cycle boundaries. This — not per-dot `ppu_step` — is how A2 stays faithful;
it naturally yields FCEUX's sprite-0 visibility and IRQ timing.

## 5. Integration seam

- **Ours:** ROM load (embedded), FM2 player, controller IO, `--dump-sync`,
  screenshots, mapper banking ($8000+ writes), CPU interp (`cpu_interp.c`).
- **FCEUX (new `src/ppu_fceux.c`, port):** scanline render, lazy `LineUpdate`,
  sprite-0 hit, the per-scanline IRQ clock.
- **Switch:** `g_ppu_backend` (0 = beam `ppu.c`, 1 = fceux). `--interp=fceux`
  sets interp mode **and** backend=1. `ppu_step()`/register hooks dispatch on it,
  or (cleaner) the runner main loop calls a backend vtable. Default path emits
  identical code (backend stays 0) — zero risk to the 4 bit-exact games and the
  recompiler.

## 6. Phasing (each phase gated by verify_all in fceux mode)

1. **Harness + NROM:** wire backend switch; port BG render + VBL/NMI; bit-match
   **Mario/Battlecity** in fceux mode (no IRQ, no sprite-0 splits).
2. **Sprite-0:** port `CheckSpriteHit`/`sphitx`/lastpixel; **Battletoads** →
   `1stSustDiv=None` (the headline win). Re-verify Mario stays bit-exact.
3. **MMC3 A12 IRQ:** port the per-scanline hook + A12-while-rendering clocking;
   **Felix** stays in sync, **Contraf** → `1stSustDiv=None`.
4. **Rest:** Zelda/Mermaid/Adventure/Castle3/Captain/Superc — confirm ≥ current.

## 7. Gate / verification

- **fceux mode:** `verify_all.sh` should bit-match FCEUX for every game (it is
  FCEUX's algorithm); any divergence is a port bug.
- **default mode:** unchanged — the 4 bit-exact games must stay 100%/0 and the
  recompiler build must be byte-identical. CI = run `verify_all.sh` in both modes.

## 8. Risks / open questions

- **Seam precision (A2):** `lastpixel` depends on sub-instruction cycle position
  (`X.count`). Our interp tracks whole-instruction cycles; may need a per-access
  cycle offset (we already approximate this with `cpu_base_cycles[]` for the
  catch-up — reuse/extend it). If insufficient → A1.
- **NewPPU 0 only:** all our demos are old-PPU; do NOT port the new PPU.
- **GPL:** FCEUX is GPL; `fceux/src` is already vendored — keep the ported file
  clearly attributed; check repo licensing before shipping.
- **Build size / second render path:** acceptable (gated, off by default).

## 9. Address collection (deferred, but how it plugs in)

When address collection resumes: run demos in `--interp=fceux` and log every
executed PC (+ bank) → recompiler seeds (`extra_func`). Same PCs as A1/A2.
(Equivalent external shortcut exists — FCEUX's Code/Data Logger — if the
emulator path is ever not worth it.)
