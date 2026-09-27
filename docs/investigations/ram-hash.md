# RAM-hash accuracy (fceux backend, 2026-07-22)

> Moved from `AGENTS.md`.

## RAM hash — $2000-write NMI-enable EDGE fix (2026-07-22): 4→7/11 exact

**`--interp=fceux` RAM-exact went 4→7/11, zero lag regressions.** Fixed
Battletoads (11→None), Mermaid (10260→None), Captain (10→None); Adventure 9→**64950**
(lag drift even improved +389→+210). Mario/Battlecity/Felix stay None.

**Root cause (ppu.c `ppu_write` case 0 = $2000):** we fired `nes_nmi()` on
`(val&0x80) && (regs[2]&0x80)` with **no edge test** (and `regs[0]` already
overwritten). FCEUX `B2000` (`ppu_vendor.c:500`) fires only on the **0→1 edge** of
NMI-enable while VBL is set: `!(vPPU[0]&0x80) && (V&0x80) && (PPU_status&0x80)`. A
`$2000` write with bit7 **already 1** during vblank spuriously **re-fired the NMI →
2 NMIs/frame** → a delay loop caught one iteration off at the VBL snapshot → RAM
diverged. Fix: capture `old_ctrl = ppu.regs[0]` before the store, gate on
`!(old_ctrl&0x80)`. Shared ppu.c ⇒ beam + recompiler also change.

**verify_all CONFIRMED (2026-07-22) — no lag regression, net improvement.**
our fceux 11/11 ok, and the fix *improved* two: **Captain −1634→+0 (bit-perfect)**,
Adventure +389→+210; rest still +0. Beam: the 4 bit-exact games (Mario/Battlecity/
Zelda/Felix) held drift 0; Mermaid/Captain beam improved; the only 2 beam DESYNCs
(Battletoads, Contraf) were already desyncing pre-fix and still are (drift numbers
shifted, verdict unchanged — not new). Vendor 11/11 ok. Not committed (GPL files
still present; never commit without explicit user confirmation).

**The method (reuse for RAM-hash work): three-way A/B/C isolation.** Dump raw RAM
$000-$7FF at the first divergent frame from A=`--interp=fceux` (cpu_interp+our
ppu+loop), B=hybrid (vendor CPU + our ppu + our loop; force `g_cpu_vendor=1` while
still calling `runner_run_fceux`), C=`--interp=fceux_vendor` (oracle). **A==B on
every game ⇒ the CPU is NOT the cause** — the RAM divergence is in ppu.c/loop, not
cpu_interp. (This overturns the "RAM = cpu_interp bug" framing.) Then per-frame
instruction-count trace → origin frame (first with OAM DMA); per-frame NMI-delivery
log (interrupted PC) → 2-vs-1 NMI count.

## RAM hash — MMC5 scanline IRQ was never clocked in the fceux backend (2026-07-22)

`runner_run_fceux` clocked MMC5 via `mapper_scanline()` — which is **MMC3-only**
(`if (mapper.id != 4) return;`). The real MMC5 hook `mapper5_hb()` was called **only
by the vendored ppu_vendor.c**, never by our fceux loop (or our beam ppu.c). So the
MMC5 split-screen IRQ never fired on `--interp=fceux`. Found by a per-frame **NMI/IRQ
delivery count** (B=our loop vs C=vendor): from frame 10 the vendor fired 1 IRQ/frame,
we fired 0; RAM diverged one frame later (@11). Fix: call
`mapper5_hb(sl, ppu.regs[1]&0x18)` at each visible line's dot 0 (like FCEUX DoLine),
`m5_in_frame=0` before the loop so line 0 enters the frame. **Castle3 lag
99.7%/−643 → 100.0%/+0 (bit-perfect — the missing IRQ skewed lag too); RAM
first-mismatch 11 → 6442.** fceux-backend `id==5` only (Castle3 is the sole MMC5 game).

**The RAM hash includes the STACK page $0100-$01FF.** Bytes below SP are dead but
hashed, so a tiny stack-depth phase difference shows as a **one-frame mismatch that
re-converges next frame with zero-page never diverging** — NOT a logic bug. Confirmed:
Castle3 @6442 (8 stack bytes, gone by 6500) and Zelda @3724 (1 stack byte). When
triaging a first_ram_mismatch, split diffs by page (zp/$01xx/other) and check
re-convergence before calling it real.

**Remaining after both fixes:** Adventure @64950 (categorization pending);
**Superc @4** (A==B==C: vendored port ≠ real FCEUX — out of reach); Contraf @8013
(RNG `$0029` churn, known-hard). **Next lead** for any true NMI-jitter residual: our
$2000-write NMI is immediate (`nes_nmi()`); FCEUX `TriggerNMI2()` delays one
instruction (IQNMI2).

## RAM hash phase — the old "informational only" caveat is OBSOLETE (2026-07-22)

The earlier worry — that `--dump-sync` snapshots RAM at the VBL boundary (before the
frame's NMI handler) while FCEUX's `registerafter` snapshots after, giving an
uncancellable phase offset — turned out to be **wrong in practice**: our snapshot
phase IS comparable to the reference (both the vendored oracle and the FCEUX ref
match `--interp=fceux` **bit-for-bit end-to-end on 7/11 games**, Battlecity included
— the old "~17%" figure predated the unified FM2 timing). So **RAM hash is a valid
frame-to-frame metric**, not merely informational, and a first-divergence frame is a
real, localisable defect (see the $2000 NMI-edge fix above). The one caveat that
survives: the **stack page $0100-$01FF is hashed**, so dead bytes below SP can differ
without meaning anything (Zelda @3724 is exactly this). Lag is still the trusted
demo-sync verdict; RAM hash is the tightening metric.

