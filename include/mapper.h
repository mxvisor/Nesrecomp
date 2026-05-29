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

#endif /* MAPPER_H */
