#ifndef PPU_H
#define PPU_H

#include <stdint.h>

#define SCREEN_W  256
#define SCREEN_H  240
#define OAM_SIZE  256

typedef struct {
    uint8_t  regs[8];
    uint8_t  oam[OAM_SIZE];
    uint8_t  vram[0x4000];
    uint8_t  chr[0x80000];
    uint16_t vaddr;
    uint8_t  fine_x;
    uint8_t  write_toggle;
    uint8_t  data_buf;
    uint32_t cycle;
    int      scanline;
    uint8_t  frame_ready;
    uint8_t  nmi_suppressed;
    uint32_t framebuf[SCREEN_W * SCREEN_H];
    uint16_t t_addr;
    uint16_t v_addr;
    uint8_t  fine_x_latch;
    uint16_t bg_lo,     bg_hi;
    uint16_t bg_pal_lo, bg_pal_hi;
    uint8_t  sprite_count;
    uint8_t  sp_pattern_lo[8];
    uint8_t  sp_pattern_hi[8];
    uint8_t  sp_attr[8];
    uint8_t  sp_x[8];
    uint8_t  sp_zero_on_line;
    uint8_t  open_bus;
} PPU;

extern PPU ppu;

uint8_t ppu_read(uint8_t reg);
void    ppu_write(uint8_t reg, uint8_t val);
void    ppu_step(void);
void    ppu_run(int clocks);

#endif /* PPU_H */
