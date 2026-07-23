#include <stdio.h>
#include "runner.h"

/* =========================================================================
   Global state
   ========================================================================= */
CPU      cpu;
uint8_t  ram[RAM_SIZE];
uint8_t  sram[SRAM_SIZE];
uint8_t  prg_rom[PRG_ROM_MAX];
uint32_t prg_rom_size = 0;
uint32_t g_cpu_cycles = 0;

uint8_t controller[2]  = {0, 0};
uint8_t ctrl_shift[2]  = {0, 0};
static uint8_t ctrl_strobe = 0;
int g_lag_flag = 1;  /* 1 = no controller read this frame (lag); cleared by ctrl_read */

/* =========================================================================
   Controller — NES standard shift-register protocol
   
   Write $4016 bit0=1: strobe ON  → shift always mirrors live buttons
   Write $4016 bit0=0: strobe OFF → latch current state, begin serial read
   Read  $4016/$4017:  return next bit (A B Sel St Up Dn L R), MSB first
   ========================================================================= */
void ctrl_write(uint8_t val) {
    uint8_t new_strobe = val & 1;
    /* Falling edge (1→0): latch current button state */
    if (ctrl_strobe && !new_strobe) {
        ctrl_shift[0] = controller[0];
        ctrl_shift[1] = controller[1];
    }
    ctrl_strobe = new_strobe;
    /* While strobe is high, shift mirrors live state continuously */
    if (ctrl_strobe) {
        ctrl_shift[0] = controller[0];
        ctrl_shift[1] = controller[1];
    }
}

uint8_t ctrl_read(int port) {
    g_lag_flag = 0;  /* game read controller → not a lag frame */
    /* $4016/$4017 upper bits read back as open bus = $40 (bit 6), the high byte
     * of the $40xx address left on the data bus. Hardware/FCEUX behaviour; games
     * that store the raw read (e.g. Contra Force $04/$05) desync without it. */
    if (ctrl_strobe) {
        /* Strobe high: always return A button (bit 7) */
        return ((controller[port] >> 7) & 1) | 0x40;
    }
    /* Shift out next bit, MSB first */
    uint8_t bit = (ctrl_shift[port] >> 7) & 1;
    ctrl_shift[port] <<= 1;
    ctrl_shift[port] |= 1; /* bus returns 1 after all 8 bits shifted out */
    return bit | 0x40;
}

/* =========================================================================
   Memory read
   ========================================================================= */
uint8_t mem_read(uint16_t addr) {
    /* RAM $0000-$1FFF (mirrored every 2KB) */
    if (addr < 0x2000) return ram[addr & 0x07FF];
    /* PPU registers $2000-$3FFF (mirrored every 8 bytes) */
    if (addr < 0x4000) return ppu_read((uint8_t)(addr & 7));
    /* APU / IO $4000-$4017 */
    if (addr == 0x4015) {
        /* fceux backend: DMC/frame timing is owned by apu_fceux (cycle-identical
         * to the vendor). Take bits 4 (DMC active), 6 (frame IRQ), 7 (DMC IRQ)
         * from it; keep apu.c's length-counter bits 0-3. */
        extern int g_ppu_backend;
        if (g_ppu_backend) {
            extern uint8_t apu_fceux_status(void);
            uint8_t base = apu_read_status();
            return (uint8_t)((base & 0x0F) | apu_fceux_status());
        }
        return apu_read_status();
    }
    if (addr == 0x4016) return ctrl_read(0);
    if (addr == 0x4017) return ctrl_read(1);
    if (addr < 0x4020) return 0xFF; /* open bus */
    /* MMC5 registers / ExRAM $5000-$5FFF */
    if (addr >= 0x5000 && addr < 0x6000) return mapper5_read(addr);
    /* AxROM (mapper 7) has NO PRG-RAM: $6000-$7FFF reads return open bus (the
     * data-bus latch ≈ address high byte), not SRAM. Battletoads reads data
     * tables through pointers that land here; FCEUX (ANROM, no WRAM) returns
     * $7F for $7Fxx while our SRAM gave 00 → desync. */
    if (mapper.id == 7 && addr >= 0x6000 && addr < 0x8000) return (uint8_t)(addr >> 8);
    /* SRAM $6000-$7FFF */
    if (addr >= 0x6000 && addr < 0x8000) return sram[addr - 0x6000];
    /* PRG-ROM $8000-$FFFF */
    if (addr >= 0x8000) return mapper_prg_read(addr);
    return 0xFF;
}

/* =========================================================================
   Memory write
   ========================================================================= */
void mem_write(uint16_t addr, uint8_t val) {
    if (addr < 0x2000) {
        ram[addr & 0x07FF] = val;
        return;
    }
    if (addr < 0x4000) { ppu_write((uint8_t)(addr & 7), val); return; }

    /* OAM DMA $4014 */
    if (addr == 0x4014) {
        //fprintf(stderr, "[oamdma] page=$%02X at PC=$%04X bank=%d OAM[0]Y=%02X\n",
        //        val, cpu.PC, mapper.m1_prg_bank, ppu.oam[0]);
        uint16_t base = (uint16_t)val << 8;
        for (int i = 0; i < 256; i++)
            ppu.oam[i] = mem_read(base + i);
        //fprintf(stderr, "[oamdma] done, OAM[0]Y=%02X X=%02X tile=%02X\n",
        //        ppu.oam[0], ppu.oam[3], ppu.oam[1]);
        /* OAM DMA halts the CPU. FCEUX charges exactly 512 (B4014: 256×
         * (X6502_DMR+X6502_DMW), each ADDCYC(1)) on top of the 4-cycle STA store.
         * We were charging 513 → +1 cyc per DMA vs FCEUX, which the timing-churned
         * RNG (Contraf $0029) accumulates into a borderline lag flip. Use 512. */
        g_cpu_cycles += 512;
        return;
    }
    if (addr == 0x4016) { ctrl_write(val); return; }
    if (addr >= 0x4000 && addr <= 0x4017) {
        apu_write(addr, val);                    /* apu.c: audio channels + DAC */
        /* fceux backend: mirror the timing regs into apu_fceux (DMC/frame IRQ). */
        { extern int g_ppu_backend; extern void apu_fceux_write(uint16_t, uint8_t);
          if (g_ppu_backend) apu_fceux_write(addr, val); }
        return;
    }
    /* AxROM (mapper 7) has no PRG-RAM: the bank latch responds to the whole
     * $4020-$FFFF range (FCEUX ANROM Latch_Init 0x4020-0xFFFF), so writes below
     * $8000 (e.g. Battletoads writes the bank via $6000-$7FFF) MUST switch the
     * bank, not land in SRAM. Without this the active PRG bank silently diverges. */
    if (mapper.id == 7 && addr >= 0x4020) { mapper_prg_write(addr, val); return; }
    /* MMC5 registers / ExRAM $5000-$5FFF */
    if (addr >= 0x5000 && addr < 0x6000) { mapper5_write(addr, val); return; }
    if (addr >= 0x6000 && addr < 0x8000) { sram[addr - 0x6000] = val; return; }
    if (addr >= 0x8000) { mapper_prg_write(addr, val); return; }
}

/* =========================================================================
   BRK
   ========================================================================= */
void cpu_brk(void) {
    uint16_t ret = cpu.PC + 2;
    stack_push((ret >> 8) & 0xFF);
    stack_push(ret & 0xFF);
    stack_push(get_P() | 0x30);
    cpu.I = 1;
    cpu.PC = mem_read(0xFFFE) | ((uint16_t)mem_read(0xFFFF) << 8);
}
