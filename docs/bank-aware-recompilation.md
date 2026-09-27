# Bank-aware recompilation (switchable PRG banks)

> Status: **partially implemented** — UNROM (2) and AxROM (7) are done; MMC1, MMC3
> and MMC5 still run switchable-bank code in the interpreter. Moved from the
> `AGENTS.md` TODO section; the original plan is kept below.
>
> What exists in `tools/nesrecomp.py` (checked 2026-09-27):
> - `discover_banked()` / `_bfs_bank()` disassemble each switchable bank for mapper 2 and 7
>   and emit `func_bN_XXXX` (step 1 below).
> - `_emit_dispatch_c()` emits the nested per-bank dispatch using
>   `mapper_get_prg_bank(0)`; unknown bank → `runner_miss()` + `cpu_interp_step()` (step 2).
> - `mapper_get_prg_bank(slot)` in `src/include/mapper.h` instead of a public field (step 3).
> - cfg `extra_func = N:XXXX` (bank-qualified seed) is parsed (step 4).
>
> Remaining: MMC1 (two slots, mode-dependent), MMC3 (multiple 8 KB windows),
> MMC5 (four 8 KB slots) — dispatch must key on more than `prg_bank(0)`.
> Also note the build-scale problem: Mermaid ≈ 24500 functions (see `STATUS.md`).

Originally (before implementation), only the **fixed PRG bank** ($E000–$FFFF or equivalent) is statically recompiled. All switchable bank code falls back to the interpreter. This defeats the purpose of recompilation for most banked games.

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
