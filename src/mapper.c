/* mapper: cartridge mapper — PRG/CHR banking, interrupts */
#include <stdio.h>
#include "mapper.h"
#include "ppu.h"
#include "interrupts.h"
#include "memory.h"
#include <string.h>

Mapper mapper;

/* Debug: frame counter for MMC3/CHR tracing (set by runner) */

void mapper_init(int id, int prg_banks, int chr_banks, int mirroring) {
    memset(&mapper, 0, sizeof(mapper));
    mapper.id        = id;
    mapper.prg_banks = prg_banks;
    mapper.chr_banks = chr_banks;
    
    /* Castlevania requires vertical mirroring */
    if (id == 2) {
        mapper.mirroring = 1;
    } else {
        mapper.mirroring = mirroring;
    }
    
    /* MMC1 defaults */
    if (id == 1) {
        mapper.m1_shift       = 0x10;
        mapper.m1_shift_count = 0;
        mapper.m1_ctrl        = 0x0C;
    }
    
    /* MMC5 defaults */
    if (id == 5) {
        mapper.m5_prg_mode = 3;   /* 8KB banks */
        mapper.m5_chr_mode = 3;   /* 1KB banks */
        /* last PRG bank always at $E000 */
        mapper.m5_prg[3] = (prg_banks * 2 - 1) | 0x80;
        /* default nametable: all CIRAM page 0 */
        mapper.m5_nt_map[0] = mapper.m5_nt_map[1] = 0;
        mapper.m5_nt_map[2] = mapper.m5_nt_map[3] = 1;
    }

    /* MMC3 defaults */
    if (id == 4) {
        mapper.m4_banks[6] = prg_banks * 2 - 2;
        mapper.m4_banks[7] = prg_banks * 2 - 1;
        mapper.m4_irq_counter = 0;
        mapper.m4_irq_latch   = 0;
        mapper.m4_irq_enable  = 0;
        mapper.m4_irq_reload  = 0;
    }
    
    /* AxROM (Mapper 7) defaults: power-on selects bank 0, one-screen lower */
    if (id == 7) {
        mapper.m1_prg_bank = 0;
        mapper.mirroring = 3; /* one-screen lower */
    }
}

/* =========================================================================
   PRG-ROM read
   ========================================================================= */
uint8_t mapper_prg_read(uint16_t addr) {
    if (addr < 0x8000) return 0xFF;
    
    uint32_t offset;
    
    switch (mapper.id) {
    case 0: /* NROM */
        offset = addr - 0x8000;
        if (mapper.prg_banks == 1) offset &= 0x3FFF;
        return prg_rom[offset % prg_rom_size];
        
    case 1: { /* MMC1 */
        int prg_mode = (mapper.m1_ctrl >> 2) & 3;
        uint32_t bank = mapper.m1_prg_bank & 0x0F;
        if (prg_mode <= 1) {
            offset = (bank & ~1u) * 0x4000 + (addr - 0x8000);
        } else if (prg_mode == 2) {
            if (addr < 0xC000) offset = addr - 0x8000;
            else offset = bank * 0x4000 + (addr - 0xC000);
        } else {
            if (addr < 0xC000) offset = bank * 0x4000 + (addr - 0x8000);
            else offset = (mapper.prg_banks - 1) * 0x4000 + (addr - 0xC000);
        }
        return prg_rom[offset % prg_rom_size];
    }
        
    case 2: /* UNROM */
        if (addr < 0xC000) {
            offset = (uint32_t)mapper.m1_prg_bank * 0x4000 + (addr - 0x8000);
        } else {
            offset = (uint32_t)(mapper.prg_banks - 1) * 0x4000 + (addr - 0xC000);
        }
        return prg_rom[offset % prg_rom_size];
        
    case 3: /* CNROM */
        offset = addr - 0x8000;
        if (mapper.prg_banks == 1) offset &= 0x3FFF;
        return prg_rom[offset % prg_rom_size];
        
    case 4: { /* MMC3 */
        int prg_mode = (mapper.m4_bank_select >> 6) & 1;
        if (addr < 0xA000) {
            uint8_t b = prg_mode ? (mapper.prg_banks * 2 - 2) : mapper.m4_banks[6];
            offset = (uint32_t)b * 0x2000 + (addr - 0x8000);
        } else if (addr < 0xC000) {
            offset = (uint32_t)mapper.m4_banks[7] * 0x2000 + (addr - 0xA000);
        } else if (addr < 0xE000) {
            uint8_t b = prg_mode ? mapper.m4_banks[6] : (mapper.prg_banks * 2 - 2);
            offset = (uint32_t)b * 0x2000 + (addr - 0xC000);
        } else {
            offset = (uint32_t)(mapper.prg_banks * 2 - 1) * 0x2000 + (addr - 0xE000);
        }
        return prg_rom[offset % prg_rom_size];
    }
        
    case 5: { /* MMC5 */
        /* $E000-$FFFF: always last ROM bank (bit7=ROM in m5_prg[3]) */
        uint8_t bank;
        if (addr >= 0xE000) {
            bank = mapper.m5_prg[3] & 0x7F;
            offset = (uint32_t)bank * 0x2000 + (addr - 0xE000);
        } else if (mapper.m5_prg_mode == 3) {
            /* 8KB mode: m5_prg[0-3] → $8000/$A000/$C000/$E000 */
            if      (addr < 0xA000) { bank = mapper.m5_prg[0] & 0x7F; offset = (uint32_t)bank * 0x2000 + (addr - 0x8000); }
            else if (addr < 0xC000) { bank = mapper.m5_prg[1] & 0x7F; offset = (uint32_t)bank * 0x2000 + (addr - 0xA000); }
            else                    { bank = mapper.m5_prg[2] & 0x7F; offset = (uint32_t)bank * 0x2000 + (addr - 0xC000); }
        } else if (mapper.m5_prg_mode == 2) {
            /* 16+8+8: m5_prg[1] 16KB at $8000, m5_prg[2] 8KB at $C000 */
            if (addr < 0xC000) { bank = (mapper.m5_prg[1] & 0x7E); offset = (uint32_t)bank * 0x2000 + (addr - 0x8000); }
            else               { bank = mapper.m5_prg[2] & 0x7F;   offset = (uint32_t)bank * 0x2000 + (addr - 0xC000); }
        } else if (mapper.m5_prg_mode == 1) {
            /* 16KB: m5_prg[1] at $8000, m5_prg[3] at $C000 */
            if (addr < 0xC000) { bank = (mapper.m5_prg[1] & 0x7E); offset = (uint32_t)bank * 0x2000 + (addr - 0x8000); }
            else               { bank = (mapper.m5_prg[3] & 0x7E); offset = (uint32_t)bank * 0x2000 + (addr - 0xC000); }
        } else {
            /* 32KB: m5_prg[3] at $8000 */
            bank = (mapper.m5_prg[3] & 0x7C);
            offset = (uint32_t)bank * 0x2000 + (addr - 0x8000);
        }
        return prg_rom[offset % prg_rom_size];
    }

    case 7: /* AxROM — 32KB switchable bank at $8000-$FFFF */
        offset = (uint32_t)mapper.m1_prg_bank * 0x8000 + (addr - 0x8000);
        return prg_rom[offset % prg_rom_size];

    default:
        offset = addr - 0x8000;
        return prg_rom[offset % prg_rom_size];
    }
}

/* =========================================================================
   PRG-ROM write (mapper registers)
   ========================================================================= */
void mapper_prg_write(uint16_t addr, uint8_t val) {
    switch (mapper.id) {
    case 0: /* NROM */
        break;
        
    case 1: /* MMC1 */
        if (val & 0x80) {
            mapper.m1_shift       = 0x10;
            mapper.m1_shift_count = 0;
            mapper.m1_ctrl       |= 0x0C;
        } else {
            mapper.m1_shift = (mapper.m1_shift >> 1) | ((val & 1) << 4);
            mapper.m1_shift_count++;
            if (mapper.m1_shift_count == 5) {
                uint8_t v = mapper.m1_shift & 0x1F;
                mapper.m1_shift       = 0x10;
                mapper.m1_shift_count = 0;
                if      (addr < 0xA000) {
                    mapper.m1_ctrl = v;
                    /* MMC1 mirroring bits → mirror_nt case:
                       0=one-screen lower→3, 1=one-screen upper→4,
                       2=vertical→1, 3=horizontal→0 */
                    static const uint8_t m1_mirror[4] = {3, 4, 1, 0};
                    mapper.mirroring = m1_mirror[v & 3];
                }
                else if (addr < 0xC000)   mapper.m1_chr_bank0 = v;
                else if (addr < 0xE000)   mapper.m1_chr_bank1 = v;
                else                      mapper.m1_prg_bank = v & 0x0F;
            }
        }
        break;
        
    case 2: /* UNROM */
        mapper.m1_prg_bank = val & 0x07;
        break;
        
    case 3: /* CNROM */
        mapper.m1_chr_bank0 = val & 3;
        break;
        
    case 4: /* MMC3 */
        if (addr < 0xA000) {
            if (addr & 1) {
                int reg = mapper.m4_bank_select & 7;
                mapper.m4_banks[reg] = val;
                if (reg == 0 || reg == 1) mapper.m4_banks[reg] &= 0xFE;
                if (reg >= 6) mapper.m4_banks[reg] &= 0x3F;
            } else {
                mapper.m4_bank_select = val;
            }
        } else if (addr < 0xC000) {
            /* MMC3: val=0=vertical, val=1=horizontal
               Our constants: 0=H, 1=V — invert */
            if (!(addr & 1)) mapper.mirroring = (val & 1) ? 0 : 1;
        } else if (addr < 0xE000) {
            if (!(addr & 1)) mapper.m4_irq_latch = val;
            else mapper.m4_irq_reload = 1;  /* $C001: reload at next scanline, do NOT zero counter now */
        } else {
            if (addr & 1) mapper.m4_irq_enable = 1;
            else { mapper.m4_irq_enable = 0; g_irq_pending = 0; } /* acknowledge */
        }
        break;
        
    case 5: /* MMC5 — PRG writes go through mapper5_write */
        mapper5_write(addr, val);
        break;

    case 7: /* AxROM */
        {
            uint8_t bank = val & 0x07;
            uint8_t mir  = (val & 0x10) ? 4 : 3;
            mapper.m1_prg_bank = bank;
            mapper.mirroring   = mir;
        }
        break;
    }
}

/* =========================================================================
   MMC5 register read/write ($5000-$5FFF)
   ========================================================================= */
uint8_t mapper5_read(uint16_t addr) {
    if (addr >= 0x5C00 && addr <= 0x5FFF)
        return mapper.m5_exram[addr - 0x5C00];
    if (addr == 0x5204) {
        uint8_t v = (mapper.m5_in_frame ? 0x40 : 0) |
                    (mapper.m5_irq_enable && mapper.m5_in_frame &&
                     mapper.m5_scanline == mapper.m5_irq_line ? 0x80 : 0);
        return v;
    }
    if (addr == 0x5205) return (uint8_t)((mapper.m5_mul[0] * mapper.m5_mul[1]) & 0xFF);
    if (addr == 0x5206) return (uint8_t)((mapper.m5_mul[0] * mapper.m5_mul[1]) >> 8);
    return 0xFF;
}

void mapper5_write(uint16_t addr, uint8_t val) {
    if (addr >= 0x5C00 && addr <= 0x5FFF) {
        if (mapper.m5_exram_mode <= 1) mapper.m5_exram[addr - 0x5C00] = val;
        else if (mapper.m5_exram_mode == 2) { /* read-only, ignore */ }
        else mapper.m5_exram[addr - 0x5C00] = val;
        return;
    }
    switch (addr) {
    case 0x5100: mapper.m5_prg_mode = val & 3; break;
    case 0x5101: mapper.m5_chr_mode = val & 3; break;
    case 0x5104: mapper.m5_exram_mode = val & 3; break;
    case 0x5105:
        /* nametable mapping: 2 bits per screen (0=CIRAM0,1=CIRAM1,2=ExRAM,3=fill) */
        mapper.m5_nt_map[0] = (val >> 0) & 3;
        mapper.m5_nt_map[1] = (val >> 2) & 3;
        mapper.m5_nt_map[2] = (val >> 4) & 3;
        mapper.m5_nt_map[3] = (val >> 6) & 3;
        break;
    case 0x5106: mapper.m5_fill_tile = val; break;
    case 0x5107: mapper.m5_fill_attr = val & 3; break;
    case 0x5114: mapper.m5_prg[0] = val; break;
    case 0x5115: mapper.m5_prg[1] = val; break;
    case 0x5116: mapper.m5_prg[2] = val; break;
    case 0x5117: mapper.m5_prg[3] = val | 0x80; break;
    case 0x5120: mapper.m5_chr[0] = val; break;
    case 0x5121: mapper.m5_chr[1] = val; break;
    case 0x5122: mapper.m5_chr[2] = val; break;
    case 0x5123: mapper.m5_chr[3] = val; break;
    case 0x5124: mapper.m5_chr[4] = val; break;
    case 0x5125: mapper.m5_chr[5] = val; break;
    case 0x5126: mapper.m5_chr[6] = val; break;
    case 0x5127: mapper.m5_chr[7] = val; break;
    case 0x5128: mapper.m5_chr_hi[0] = val; break;
    case 0x5129: mapper.m5_chr_hi[1] = val; break;
    case 0x512A: mapper.m5_chr_hi[2] = val; break;
    case 0x512B: mapper.m5_chr_hi[3] = val; break;
    case 0x5130: mapper.m5_chr_upper = val & 3; break;
    case 0x5203: mapper.m5_irq_line = val; break;
    case 0x5204: mapper.m5_irq_enable = (val >> 7) & 1; break;
    case 0x5205: mapper.m5_mul[0] = val; break;
    case 0x5206: mapper.m5_mul[1] = val; break;
    /* PRG writes in range $8000-$FFFF also handled here for completeness */
    default: break;
    }
}

/* =========================================================================
   CHR read/write
   ========================================================================= */
uint8_t mapper_chr_read(uint16_t addr) {
    addr &= 0x1FFF;
    
    /* CHR-RAM */
    if (mapper.chr_banks == 0) {
        return ppu.chr[addr];
    }
    
    uint32_t chr_size = (uint32_t)mapper.chr_banks * 8192;
    uint32_t off;
    
    switch (mapper.id) {
    case 0: /* NROM */
    case 2: /* UNROM */
    case 7: /* AxROM */
        return ppu.chr[addr % chr_size];
        
    case 1: { /* MMC1 */
        int chr_mode = (mapper.m1_ctrl >> 4) & 1;
        if (chr_mode == 0) {
            off = (uint32_t)(mapper.m1_chr_bank0 & 0x1E) * 0x1000 + addr;
        } else {
            if (addr < 0x1000)
                off = (uint32_t)mapper.m1_chr_bank0 * 0x1000 + addr;
            else
                off = (uint32_t)mapper.m1_chr_bank1 * 0x1000 + (addr - 0x1000);
        }
        return ppu.chr[off % chr_size];
    }
        
    case 3: /* CNROM */
        off = (uint32_t)mapper.m1_chr_bank0 * 0x2000 + addr;
        return ppu.chr[off % chr_size];
        
    case 4: { /* MMC3 */
        int inv = (mapper.m4_bank_select >> 7) & 1;
        uint16_t a = inv ? (addr ^ 0x1000) : addr;
        uint8_t bank;
        
        if (a < 0x0800) {
            bank = mapper.m4_banks[0] & 0xFE;
            off = (uint32_t)bank * 0x400 + (a & 0x7FF);
        } else if (a < 0x1000) {
            bank = mapper.m4_banks[1] & 0xFE;
            off = (uint32_t)bank * 0x400 + (a & 0x7FF);
        } else if (a < 0x1400) {
            bank = mapper.m4_banks[2];
            off = (uint32_t)bank * 0x400 + (a & 0x3FF);
        } else if (a < 0x1800) {
            bank = mapper.m4_banks[3];
            off = (uint32_t)bank * 0x400 + (a & 0x3FF);
        } else if (a < 0x1C00) {
            bank = mapper.m4_banks[4];
            off = (uint32_t)bank * 0x400 + (a & 0x3FF);
        } else {
            bank = mapper.m4_banks[5];
            off = (uint32_t)bank * 0x400 + (a & 0x3FF);
        }
        return ppu.chr[off % chr_size];
    }
        
    case 5: { /* MMC5 */
        /* CHR bank selection: m5_chr_hi ($5128-$512B) only for BG in 8x16 sprite mode */
        int use_hi = mapper.m5_bg_chr && (ppu.regs[0] & 0x20);
        uint8_t *regs = use_hi ? mapper.m5_chr_hi : mapper.m5_chr;
        uint16_t upper = (uint16_t)mapper.m5_chr_upper << 8;
        uint8_t bank;
        switch (mapper.m5_chr_mode) {
        case 0: /* 8KB */
            bank = regs[7];
            off = (uint32_t)((upper | bank) & 0x1FF) * 0x2000 + addr;
            break;
        case 1: /* 4KB */
            if (use_hi) {
                /* chr_hi covers 4KB: indices 0 and 1 */
                bank = regs[(addr < 0x1000) ? 0 : 1];
                addr &= 0x0FFF;
            } else {
                if (addr < 0x1000) bank = regs[3];
                else { bank = regs[7]; addr -= 0x1000; }
            }
            off = (uint32_t)((upper | bank) & 0x1FF) * 0x1000 + addr;
            break;
        case 2: /* 2KB */
            if (use_hi) {
                /* chr_hi covers 4KB in 2KB banks: indices 0-3 */
                int idx2 = (addr >> 11) & 3;
                bank = regs[idx2]; addr &= 0x07FF;
            } else {
                if      (addr < 0x0800) { bank = regs[1]; }
                else if (addr < 0x1000) { bank = regs[3]; addr -= 0x0800; }
                else if (addr < 0x1800) { bank = regs[5]; addr -= 0x1000; }
                else                    { bank = regs[7]; addr -= 0x1800; }
            }
            off = (uint32_t)((upper | bank) & 0x1FF) * 0x0800 + addr;
            break;
        default: /* 1KB */
            { int idx = use_hi ? ((addr >> 10) & 3) : (addr >> 10);
              bank = regs[idx & 7]; addr &= 0x3FF; }
            off = (uint32_t)((upper | bank) & 0x1FF) * 0x0400 + addr;
            break;
        }
        return ppu.chr[off % chr_size];
    }

    default:
        return ppu.chr[addr % chr_size];
    }
}

uint8_t mapper_get_prg_bank(int slot) {
    (void)slot;
    /* UNROM (2), MMC1 (1), AxROM (7): m1_prg_bank is the active switchable bank */
    return mapper.m1_prg_bank;
}

void mapper_chr_write(uint16_t addr, uint8_t val) {
    /* CHR-RAM */
    if (mapper.chr_banks == 0) {
        uint16_t eff = addr & 0x1FFF;
        /* Trace all CHR writes during bank 2 */
        //if (mapper.m1_prg_bank == 2)
        //    fprintf(stderr, "[chr2] vram$%04X=%02X PC=$%04X\n", eff, val, cpu.PC);
        /* Trace CHR writes to BG tile $00 ($1000-$100F) */
        //if (eff >= 0x1000 && eff < 0x1010)
        //    fprintf(stderr, "[chrT0] vram$%04X=%02X scan=%d\n", eff, val, ppu.scanline);
        ppu.chr[eff] = val;
    }
}

/* =========================================================================
   MMC3 Scanline IRQ
   ========================================================================= */
void mapper_scanline(void) {
    /* MMC5 in-frame scanline IRQ */
    if (mapper.id == 5) {
        mapper.m5_scanline++;
        if (mapper.m5_in_frame && mapper.m5_scanline == mapper.m5_irq_line && mapper.m5_irq_enable)
            nes_irq();
        return;
    }

    if (mapper.id != 4) return;

    /* Called from ppu.c only when RENDER is active — no extra checks needed */

    int old_count = mapper.m4_irq_counter;
    if (!old_count || mapper.m4_irq_reload) {
        mapper.m4_irq_counter = mapper.m4_irq_latch;
        mapper.m4_irq_reload = 0;
    } else {
        mapper.m4_irq_counter--;
    }

    /* Fire IRQ when counter transitions to 0 from a non-zero value (standard MMC3 rev A).
     * Do NOT fire when reloading with old_count==0 (that would be rev B behavior). */
    if (old_count && !mapper.m4_irq_counter && mapper.m4_irq_enable) {
        nes_irq();
    }
}