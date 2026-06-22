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
    uint8_t  frame_odd;    /* toggles each frame for odd-frame dot skip */
    uint8_t  in_vblank;    /* VBL period active; survives $2002 read which clears regs[2] bit7 */
    uint8_t  nmi_suppressed;
    uint32_t framebuf[SCREEN_W * SCREEN_H];
    uint8_t  indexbuf[SCREEN_W * SCREEN_H]; /* raw NES palette index (0-63) per pixel */
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

/* --interp=fceux chunk-driven backend (FCEUX old-PPU lazy render). See
 * docs/interp-fceux-design.md and runner_run_fceux(). */
void    fceux_line_begin(int sl);   /* per visible scanline: copy_hori, render BG
                                       opacity, evaluate sprite-0 for the line */
void    fceux_line_end(int sl);     /* EndRL: CheckSpriteHit(272), inc_vert */
void    fceux_prerender(void);      /* pre-render: copy_vert (set up line-0 v) */
void    fceux_on_2002_read(void);   /* lazy LineUpdate at a $2002 read */
void    fceux_render_line(int sl);  /* full-colour line render → framebuf (video) */

#endif /* PPU_H */
