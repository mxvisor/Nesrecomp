#ifndef MAPPER_H
#define MAPPER_H

/*
 * mapper.h — cartridge mapper: PRG/CHR banking, IRQ
 *
 * Scope: bank switching and cartridge IRQ only.
 * If the game shows wrong tiles or crashes during scroll see mapper.c.
 *
 * Supported mappers:
 *   0  NROM       — Donkey Kong, Super Mario Bros
 *   1  MMC1       — Metroid, Mega Man 2
 *   2  UNROM      — Castlevania, Mega Man
 *   3  CNROM      — Gradius
 *   4  MMC3       — Mega Man 4, Battletoads, Aa Yakyuu
 *   5  MMC5       — Castlevania 3
 *   7  AxROM      — Battletoads (US)
 */

#include <stdint.h>

typedef struct {
    int     id;           /* mapper number */
    uint8_t prg_banks;    /* number of 16KB PRG banks */
    uint8_t chr_banks;    /* number of 8KB CHR banks (0 = CHR-RAM) */
    uint8_t mirroring;    /* 0=H, 1=V, 2=4screen, 3=single0, 4=single1 */

    /* MMC1 (mapper 1) */
    uint8_t m1_shift;         /* shift register (init=0x10) */
    uint8_t m1_shift_count;
    uint8_t m1_ctrl;          /* control register */
    uint8_t m1_prg_bank;
    uint8_t m1_chr_bank0;
    uint8_t m1_chr_bank1;

    /* MMC3 (mapper 4) */
    uint8_t m4_bank_select;   /* bank select register ($8000) */
    uint8_t m4_banks[8];      /* R0-R7: CHR[0-5], PRG[6-7] */
    uint8_t m4_irq_latch;
    uint8_t m4_irq_counter;
    uint8_t m4_irq_enable;
    uint8_t m4_irq_reload;

    /* MMC5 (mapper 5) */
    uint8_t m5_prg_mode;      /* $5100 bits 0-1: 0=32KB,1=16KB,2=16+8+8,3=8KB */
    uint8_t m5_chr_mode;      /* $5101 bits 0-1: 0=8KB,1=4KB,2=2KB,3=1KB */
    uint8_t m5_prg[4];        /* $5114-$5117: PRG bank regs */
    uint8_t m5_chr[8];        /* $5120-$5127: CHR bank regs */
    uint8_t m5_chr_hi[4];     /* $5128-$512B: CHR bg-only high banks */
    uint8_t m5_chr_upper;     /* $5130: upper 2 bits of CHR bank */
    uint8_t m5_irq_line;      /* $5203: IRQ scanline target */
    uint8_t m5_irq_enable;    /* $5204 bit 7 */
    uint8_t m5_in_frame;      /* set by PPU rendering, cleared at VBlank */
    int     m5_scanline;      /* current in-frame scanline counter */
    uint8_t m5_mul[2];        /* $5205-$5206: hardware multiplier inputs */
    uint8_t m5_exram[1024];   /* $5C00-$5FFF: extra 1KB RAM */
    uint8_t m5_exram_mode;    /* $5104 bits 0-1 */
    uint8_t m5_nt_map[4];     /* $5105: nametable mapping (2 bits each) */
    uint8_t m5_fill_tile;     /* $5106: fill-mode tile */
    uint8_t m5_fill_attr;     /* $5107: fill-mode attribute */
    uint8_t m5_bg_chr;        /* 0=use m5_chr, 1=use m5_chr_hi (set during BG fetch) */
} Mapper;

extern Mapper mapper;

/* Initialise on ROM load */
void    mapper_init(int id, int prg_banks, int chr_banks, int mirroring);

/* PRG-ROM read/write ($8000-$FFFF) */
uint8_t mapper_prg_read(uint16_t addr);
void    mapper_prg_write(uint16_t addr, uint8_t val);

/* CHR read/write ($0000-$1FFF via PPU) */
uint8_t mapper_chr_read(uint16_t addr);
void    mapper_chr_write(uint16_t addr, uint8_t val);

/*
 * mapper_scanline() — called from ppu.c at dot 260 of each visible scanline.
 * Used for MMC3 IRQ scanline counter (split-screen effects).
 */
void    mapper_scanline(void);

/* MMC5: read mapper registers/ExRAM ($5000-$5FFF) */
uint8_t mapper5_read(uint16_t addr);
void    mapper5_write(uint16_t addr, uint8_t val);

#endif /* MAPPER_H */
