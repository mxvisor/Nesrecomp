/* ppu: PPU 2C02 — rendering, CHR banking, interrupts */
#include "runner.h"
#include <stdio.h>

PPU ppu;

/* --interp=fceux backend selector (defined in runner.c). 0 = beam-accurate
 * default, 1 = FCEUX-faithful (lazy sprite-0 visibility in ppu_step). */
extern int g_ppu_backend;

/* =========================================================================
   Palette — accurate 2C02 NTSC palette (ARGB8888)
   ========================================================================= */
static const uint32_t PALETTE[64] = {
    0xFF626262, 0xFF002CA8, 0xFF1212C8, 0xFF5200B0,
    0xFF7C007C, 0xFF880020, 0xFF780000, 0xFF5C1000,
    0xFF302800, 0xFF004400, 0xFF005000, 0xFF004C14,
    0xFF003C5C, 0xFF000000, 0xFF000000, 0xFF000000,

    0xFFABABAB, 0xFF1060FC, 0xFF4040FC, 0xFF8000FC,
    0xFFBC00CC, 0xFFD40060, 0xFFD00010, 0xFFA42000,
    0xFF6C4400, 0xFF226400, 0xFF007400, 0xFF006C30,
    0xFF006090, 0xFF000000, 0xFF000000, 0xFF000000,

    0xFFFFFFFF, 0xFF60B4FC, 0xFF8490FC, 0xFFCC78FC,
    0xFFF460FC, 0xFFFC60B0, 0xFFFC6054, 0xFFF07828,
    0xFFCC9C00, 0xFF78BC00, 0xFF40CC00, 0xFF2CC840,
    0xFF2CC8A4, 0xFF444444, 0xFF000000, 0xFF000000,

    0xFFFFFFFF, 0xFFB8E4FC, 0xFFCCD8FC, 0xFFE8C8FC,
    0xFFFCC4FC, 0xFFFCC4D8, 0xFFFCBCB0, 0xFFF0D0A0,
    0xFFE4E090, 0xFFC8F08C, 0xFFAEF4AC, 0xFFA8F0CC,
    0xFFA8EEF0, 0xFFB8B8B8, 0xFF000000, 0xFF000000,
};

/* =========================================================================
   VRAM / nametable mirroring
   ========================================================================= */
static uint16_t mirror_nt(uint16_t addr) {
    addr &= 0x0FFF;
    switch (mapper.mirroring) {
    case 0: return (addr & 0x03FF) | ((addr >= 0x0800) ? 0x0400 : 0);
    case 1: return addr & 0x07FF;
    case 2: return addr & 0x0FFF;
    case 3: return addr & 0x03FF;
    case 4: return 0x0400|(addr&0x03FF);
    }
    return addr & 0x03FF;
}

/* MMC5 nametable read: routes to CIRAM, ExRAM, or fill tile */
static uint8_t mmc5_nt_read(uint16_t addr) {
    uint16_t rel  = addr & 0x0FFF;
    uint8_t  slot = (rel >> 10) & 3;   /* which of the 4 NT slots */
    uint16_t off  = rel & 0x03FF;
    switch (mapper.m5_nt_map[slot]) {
    case 0: return ppu.vram[off];           /* CIRAM page 0 */
    case 1: return ppu.vram[0x0400 | off];  /* CIRAM page 1 */
    case 2: return (mapper.m5_exram_mode <= 1) ? mapper.m5_exram[off & 0x3FF] : 0;
    case 3: /* fill mode */
        if (off >= 0x3C0) {
            /* attribute byte — replicate fill attr in all groups */
            uint8_t a = mapper.m5_fill_attr & 3;
            return (uint8_t)((a << 6) | (a << 4) | (a << 2) | a);
        }
        return mapper.m5_fill_tile;
    }
    return 0;
}

static void mmc5_nt_write(uint16_t addr, uint8_t val) {
    uint16_t rel  = addr & 0x0FFF;
    uint8_t  slot = (rel >> 10) & 3;
    uint16_t off  = rel & 0x03FF;
    switch (mapper.m5_nt_map[slot]) {
    case 0: ppu.vram[off]          = val; break;
    case 1: ppu.vram[0x0400 | off] = val; break;
    case 2: if (mapper.m5_exram_mode <= 1) mapper.m5_exram[off & 0x3FF] = val; break;
    case 3: break; /* fill mode: writes ignored */
    }
}

static uint8_t vram_read(uint16_t addr) {
    addr &= 0x3FFF;
    if (addr < 0x2000) return mapper_chr_read(addr);
    if (addr < 0x3F00) {
        if (mapper.id == 5) return mmc5_nt_read(addr);
        return ppu.vram[mirror_nt(addr - 0x2000)];
    }
    addr &= 0x1F;
    if (addr == 0x10 || addr == 0x14 || addr == 0x18 || addr == 0x1C)
        addr &= 0x0F;
    return ppu.vram[0x3F00 | addr] & 0x3F;
}

static void vram_write(uint16_t addr, uint8_t val) {
    addr &= 0x3FFF;
    if (addr < 0x2000) { mapper_chr_write(addr, val); return; }
    if (addr < 0x3F00) {
        if (mapper.id == 5) { mmc5_nt_write(addr, val); return; }
        uint16_t offs = mirror_nt(addr - 0x2000);
        ppu.vram[offs] = val; return;
    }
    addr &= 0x1F;
    if (addr == 0x10 || addr == 0x14 || addr == 0x18 || addr == 0x1C)
        addr &= 0x0F;
    ppu.vram[0x3F00 | addr] = val;
}

/* =========================================================================
   PPU register read/write
   ========================================================================= */
uint8_t ppu_read(uint8_t reg) {
    if (g_ppu_backend) {
        /* fceux chunk-driven backend: no per-dot beam. EVERY $200x read triggers
         * FCEUX's lazy LineUpdate — render/sprite-0-check only up to this read's
         * exact dot (lastpixel). FCEUX hooks all four read cases (2, 4, 7 and the
         * open-bus default). See fceux_line_update() and runner_run_fceux. */
        fceux_line_update();
    } else {
        /* beam: FCEUX-style catch-up to this read's CPU cycle (interp mode arms
         * g_ppu_catchup_dots = base_cycles*3) so a $2002 poll observes the PPU
         * at the read, not a whole instruction behind. See runner_run(). */
        extern int g_ppu_catchup_dots, g_ppu_caught_up;
        if (g_ppu_catchup_dots && !g_ppu_caught_up) {
            g_ppu_caught_up = 1;
            for (int i = 0; i < g_ppu_catchup_dots; i++) ppu_step();
        }
    }
    switch (reg & 7) {
    case 2: {
        uint8_t s = (ppu.regs[2] & 0xE0) | (ppu.open_bus & 0x1F);
        /* NMI/$2002 race: reading the status register at (or within a CPU
         * cycle of) the VBL-set dot (241,1; ppu.cycle is post-incremented to 2)
         * suppresses this frame's NMI — and a read one dot early sees VBL still
         * clear. Matches hardware/FCEUX; only observable now that the read is
         * cycle-accurate via PPU catch-up (else the read lands at instr start). */
        if (!g_ppu_backend && ppu.scanline == 241 && ppu.cycle >= 1 && ppu.cycle <= 3) {
            extern volatile int g_nmi_pending;
            g_nmi_pending = 0;
            if (ppu.cycle == 1) s &= ~0x80;   /* read just before VBL set */
        }
        ppu.nmi_suppressed = 1;
        ppu.regs[2] &= ~0x80;
        ppu.write_toggle = 0;
        return s;
    }
    case 4: return ppu.oam[ppu.regs[3]];
    case 7: {
        uint16_t a = ppu.v_addr & 0x3FFF;
        uint8_t val;
        if (a < 0x3F00) {
            val = ppu.data_buf;
            ppu.data_buf = vram_read(a);
        } else {
            ppu.data_buf = vram_read(a & 0x2FFF);
            val = vram_read(a);
        }
        ppu.v_addr = (ppu.v_addr + ((ppu.regs[0] & 4) ? 32 : 1)) & 0x7FFF;
        return val;
    }
    }
    return ppu.open_bus;
}

void ppu_write(uint8_t reg, uint8_t val) {
    /* FCEUX renders the line so far BEFORE the write lands, for every register
     * whose new value changes the rest of the line ($2000/$2001/$2005/$2006/$2007
     * — not $2002/$2003/$2004). This is what turns a mid-line write into a raster
     * split instead of a retroactive repaint of the whole scanline. */
    if (g_ppu_backend) {
        switch (reg & 7) {
        case 0: case 1: case 5: case 6: case 7: fceux_line_update(); break;
        default: break;
        }
    }
    uint8_t old_ctrl = ppu.regs[0];
    ppu.open_bus = val;
    ppu.regs[reg & 7] = val;
    switch (reg & 7) {
    case 0:
        ppu.t_addr = (ppu.t_addr & ~0x0C00) | ((uint16_t)(val & 3) << 10);
        /* NMI fires only on the 0->1 EDGE of the NMI-enable bit while the VBL flag
         * is set — FCEUX B2000: `!(old&0x80) && (V&0x80) && (PPU_status&0x80)`.
         * Without the edge test, a $2000 write with bit7 already 1 spuriously
         * re-fires the NMI (Adventure frame 7: an extra NMI mid-vblank shifted all
         * sub-frame timing → RAM-hash divergence from the FCEUX oracle). */
        if (!(old_ctrl & 0x80) && (val & 0x80) && (ppu.regs[2] & 0x80))
            nes_nmi();
        break;
    case 1:
        break;
    case 3: break;
    case 4:
        ppu.oam[ppu.regs[3]++] = val;
        break;
    case 5:
        if (!ppu.write_toggle) {
            ppu.t_addr = (ppu.t_addr & ~0x001F) | (val >> 3);
            ppu.fine_x = val & 7;
        } else {
            ppu.t_addr = (ppu.t_addr & ~0x73E0)
                       | ((uint16_t)(val & 0x07) << 12)
                       | ((uint16_t)(val & 0xF8) << 2);
        }
        ppu.write_toggle ^= 1;
        break;
    case 6:
        if (!ppu.write_toggle) {
            ppu.t_addr = (ppu.t_addr & 0x00FF) | ((uint16_t)(val & 0x3F) << 8);
        } else {
            ppu.t_addr = (ppu.t_addr & 0xFF00) | val;
            ppu.v_addr = ppu.t_addr;
        }
        ppu.write_toggle ^= 1;
        break;
    case 7:
        vram_write(ppu.v_addr, val);
        ppu.v_addr = (ppu.v_addr + ((ppu.regs[0] & 4) ? 32 : 1)) & 0x7FFF;
        break;
    }
}

/* =========================================================================
   Loopy scroll helpers
   ========================================================================= */
static inline void inc_hori_v(void) {
    if ((ppu.v_addr & 0x001F) == 31) {
        ppu.v_addr &= ~0x001F;
        ppu.v_addr ^= 0x0400;
    } else {
        ppu.v_addr++;
    }
}

static inline void inc_vert_v(void) {
    if ((ppu.v_addr & 0x7000) != 0x7000) {
        ppu.v_addr += 0x1000;
    } else {
        ppu.v_addr &= ~0x7000;
        int y = (ppu.v_addr >> 5) & 31;
        if (y == 29) { y = 0; ppu.v_addr ^= 0x0800; }
        else if (y == 31) y = 0;
        else y++;
        ppu.v_addr = (ppu.v_addr & ~0x03E0) | (y << 5);
    }
}

static inline void copy_hori_v(void) {
    ppu.v_addr = (ppu.v_addr & ~0x041F) | (ppu.t_addr & 0x041F);
}

static inline void copy_vert_v(void) {
    ppu.v_addr = (ppu.v_addr & ~0x7BE0) | (ppu.t_addr & 0x7BE0);
}

/* =========================================================================
   Background tile fetch
   ========================================================================= */
static void fetch_bg_tile(void) {
    mapper.m5_bg_chr = 1;
    uint16_t nt_addr = 0x2000 | (ppu.v_addr & 0x0FFF);
    uint8_t  tile    = vram_read(nt_addr);

    uint16_t at_addr = 0x23C0
                     | (ppu.v_addr & 0x0C00)
                     | ((ppu.v_addr >> 4) & 0x38)
                     | ((ppu.v_addr >> 2) & 0x07);
    uint8_t  attr  = vram_read(at_addr);
    uint8_t  shift = ((ppu.v_addr >> 4) & 4) | (ppu.v_addr & 2);
    uint8_t  pal   = (attr >> shift) & 3;

    uint16_t pt_base = (ppu.regs[0] & 0x10) ? 0x1000 : 0x0000;
    uint8_t  fine_y  = (ppu.v_addr >> 12) & 7;
    uint8_t  lo      = vram_read(pt_base + (uint16_t)tile * 16 + fine_y);
    uint8_t  hi      = vram_read(pt_base + (uint16_t)tile * 16 + fine_y + 8);

    ppu.bg_lo     = (ppu.bg_lo     & 0xFF00) | lo;
    ppu.bg_hi     = (ppu.bg_hi     & 0xFF00) | hi;
    ppu.bg_pal_lo = (ppu.bg_pal_lo & 0xFF00) | ((pal & 1) ? 0xFF : 0x00);
    ppu.bg_pal_hi = (ppu.bg_pal_hi & 0xFF00) | ((pal & 2) ? 0xFF : 0x00);
}

static void fetch_bg_tile_high(void) {
    mapper.m5_bg_chr = 1;
    uint16_t nt_addr = 0x2000 | (ppu.v_addr & 0x0FFF);
    uint8_t  tile    = vram_read(nt_addr);

    uint16_t at_addr = 0x23C0
                     | (ppu.v_addr & 0x0C00)
                     | ((ppu.v_addr >> 4) & 0x38)
                     | ((ppu.v_addr >> 2) & 0x07);
    uint8_t  attr  = vram_read(at_addr);
    uint8_t  shift = ((ppu.v_addr >> 4) & 4) | (ppu.v_addr & 2);
    uint8_t  pal   = (attr >> shift) & 3;

    uint16_t pt_base = (ppu.regs[0] & 0x10) ? 0x1000 : 0x0000;
    uint8_t  fine_y  = (ppu.v_addr >> 12) & 7;
    uint8_t  lo      = vram_read(pt_base + (uint16_t)tile * 16 + fine_y);
    uint8_t  hi      = vram_read(pt_base + (uint16_t)tile * 16 + fine_y + 8);

    ppu.bg_lo     = (ppu.bg_lo     & 0x00FF) | ((uint16_t)lo << 8);
    ppu.bg_hi     = (ppu.bg_hi     & 0x00FF) | ((uint16_t)hi << 8);
    ppu.bg_pal_lo = (ppu.bg_pal_lo & 0x00FF) | (((pal & 1) ? 0xFF : 0x00) << 8);
    ppu.bg_pal_hi = (ppu.bg_pal_hi & 0x00FF) | (((pal & 2) ? 0xFF : 0x00) << 8);
}

/* =========================================================================
   Sprite evaluation
   ========================================================================= */
static void eval_sprites(int scanline) {
    ppu.sprite_count    = 0;
    ppu.sp_zero_on_line = 0;
    uint8_t sprite_h = (ppu.regs[0] & 0x20) ? 16 : 8;

    for (int i = 0; i < 64; i++) {
        int y = (int)ppu.oam[i * 4] + 1;
        if (scanline < y || scanline >= y + sprite_h) continue;
        if (ppu.sprite_count == 8) { ppu.regs[2] |= 0x20; break; }
        if (i == 0) ppu.sp_zero_on_line = 1;

        uint8_t tile = ppu.oam[i*4+1];
        uint8_t attr = ppu.oam[i*4+2];
        uint8_t spx  = ppu.oam[i*4+3];
        int     row  = scanline - y;
        if (attr & 0x80) row = (sprite_h - 1) - row;

        uint16_t pt_base;
        uint8_t  t;
        if (sprite_h == 8) {
            pt_base = (ppu.regs[0] & 0x08) ? 0x1000 : 0x0000;
            t = tile;
        } else {
            pt_base = (tile & 1) ? 0x1000 : 0x0000;
            t = tile & 0xFE;
            if (row >= 8) { t++; row -= 8; }
        }

        mapper.m5_bg_chr = 0;
        uint8_t lo = vram_read(pt_base + (uint16_t)t * 16 + row);
        uint8_t hi = vram_read(pt_base + (uint16_t)t * 16 + row + 8);

        if (attr & 0x40) {
            lo = (uint8_t)(((lo * 0x80200802ULL) & 0x0884422110ULL) * 0x0101010101ULL >> 32);
            hi = (uint8_t)(((hi * 0x80200802ULL) & 0x0884422110ULL) * 0x0101010101ULL >> 32);
        }

        int idx = ppu.sprite_count++;
        ppu.sp_pattern_lo[idx] = lo;
        ppu.sp_pattern_hi[idx] = hi;
        ppu.sp_attr[idx]       = attr;
        ppu.sp_x[idx]          = spx;
    }
}

/* =========================================================================
   PPU step — one dot (pixel clock)
   ========================================================================= */
#define BG_EN   (ppu.regs[1] & 0x08)
#define SP_EN   (ppu.regs[1] & 0x10)
#define RENDER  (BG_EN || SP_EN)
#define NMI_EN  (ppu.regs[0] & 0x80)

void ppu_step(void) {
    int dot      = ppu.cycle;
    int scanline = ppu.scanline;

    /* ---- Visible scanlines 0–239 AND pre-render scanline 261 ---- */
    if (scanline < 240 || (scanline == 261 && RENDER)) {

#define RENDER_PIXELS (scanline < 240)

        /* BG tile fetch BEFORE pixel output — prevents gap at tile boundaries */
        if (RENDER) {
            if (dot >= 9 && dot <= 257 && (dot & 7) == 1) {
                fetch_bg_tile();
                if (dot < 257) inc_hori_v();
            }
        }

        /* Pixel output: dots 1–256 */
        if (dot >= 1 && dot <= 256) {
            int x = dot - 1;
            int out_x = x;

            uint8_t bg_pixel = 0, bg_pal = 0;
            if (BG_EN && (x >= 8 || (ppu.regs[1] & 0x02))) {
                uint16_t mux = 0x8000 >> ppu.fine_x;
                bg_pixel = ((ppu.bg_lo     & mux) ? 1 : 0)
                         | ((ppu.bg_hi     & mux) ? 2 : 0);
                bg_pal   = ((ppu.bg_pal_lo & mux) ? 1 : 0)
                         | ((ppu.bg_pal_hi & mux) ? 2 : 0);
            }

            if (RENDER_PIXELS) {
                uint8_t sp_pixel = 0, sp_pal = 0, sp_priority = 0;
                if (SP_EN && (x >= 8 || (ppu.regs[1] & 0x04))) {
                    for (int i = 0; i < ppu.sprite_count; i++) {
                        int sx = x - (int)ppu.sp_x[i];
                        if (sx < 0 || sx > 7) continue;
                        uint8_t lo = (ppu.sp_pattern_lo[i] >> (7 - sx)) & 1;
                        uint8_t hi = (ppu.sp_pattern_hi[i] >> (7 - sx)) & 1;
                        uint8_t p  = lo | (hi << 1);
                        if (!p) continue;
                        if (i == 0 && ppu.sp_zero_on_line && x < 255) {
                            if (bg_pixel)
                                ppu.regs[2] |= 0x40;
                        }
                        sp_pixel    = p;
                        sp_pal      = (ppu.sp_attr[i] & 3) + 4;
                        sp_priority = (ppu.sp_attr[i] >> 5) & 1;
                        break;
                    }
                }

                uint8_t pal_addr;
                if      (!bg_pixel && !sp_pixel) pal_addr = 0;
                else if (!bg_pixel &&  sp_pixel) pal_addr = sp_pal  * 4 + sp_pixel;
                else if ( bg_pixel && !sp_pixel) pal_addr = bg_pal  * 4 + bg_pixel;
                else {
                    pal_addr = sp_priority ? (bg_pal * 4 + bg_pixel)
                                           : (sp_pal * 4 + sp_pixel);
                }
                uint8_t color = vram_read(0x3F00 + pal_addr);
                ppu.framebuf[scanline * SCREEN_W + out_x] = PALETTE[color & 0x3F];
                ppu.indexbuf[scanline * SCREEN_W + out_x] = color & 0x3F;
            }

            ppu.bg_lo     <<= 1; ppu.bg_hi     <<= 1;
            ppu.bg_pal_lo <<= 1; ppu.bg_pal_hi <<= 1;
        }

        /* Scroll increment / copy / tile prefetch */
        if (RENDER) {
            if (dot == 256 && scanline < 240) inc_vert_v();
            if (dot == 257) copy_hori_v();
            if (scanline == 261 && dot >= 280 && dot <= 304) copy_vert_v();
            if (dot == 321) { fetch_bg_tile_high(); inc_hori_v(); }
            if (dot == 329) { fetch_bg_tile(); inc_hori_v(); }
        }

        if (dot == 257)
            eval_sprites(scanline + 1 < 240 ? scanline + 1 : 0);

#undef RENDER_PIXELS
    }

    /* ---- Pre-render scanline 261: special ops at dot 1 ---- */
    if (scanline == 261 && dot == 1) {
        ppu.regs[2] &= ~0xE0;
        ppu.in_vblank = 0;
        ppu.nmi_suppressed = 0;
    }

    /* ---- VBlank: scanline 241, dot 1 ---- */
    if (scanline == 241 && dot == 1) {
        ppu.frame_ready = 1;          /* internal frame boundary — always */
        /* PPU power-up warm-up: real hardware sets no reliable VBL flag for the
         * first frames after reset, so the game spins in its reset wait-loop.
         * FCEUX/Mesen model this; without it our game starts early and its FM2
         * input lands a frame off (Zelda desync). g_ppudead counts the frames. */
        if (g_ppudead == 0) {
            ppu.regs[2] |= 0x80;
            ppu.in_vblank = 1;
            ppu.nmi_suppressed = 0;
            if (NMI_EN) nes_nmi();
            /* MMC5: end of frame */
            mapper.m5_in_frame = 0;
            mapper.m5_scanline = 0;
        }
    }

    /* MMC5: track in-frame state — starts at pre-render scanline dot 1 */
    if (mapper.id == 5 && scanline == 261 && dot == 1)
        mapper.m5_in_frame = 1;

    /* MMC3 Scanline IRQ: fire at dot 260 (hblank) of visible scanlines 0-239.
       Firing during hblank gives the CPU time to switch CHR banks before the next scanline renders. */
    if (RENDER && dot == 260 && scanline < 240) {
        mapper_scanline();
    }

    /* ---- Advance dot / scanline ---- */
    ppu.cycle++;
    /* NTSC odd-frame dot skip: on odd frames with rendering enabled, the idle
     * dot at (261,340) is skipped — jump straight to (0,0). This makes frames
     * alternate 89342/89341 dots (29780.5 CPU cyc avg) instead of always
     * 89342, matching hardware so the per-frame CPU budget is exact. */
    if (ppu.scanline == 261 && ppu.cycle == 340 && ppu.frame_odd && RENDER) {
        ppu.cycle = 0;
        ppu.scanline = 0;
        ppu.frame_odd ^= 1;
    } else if (ppu.cycle > 340) {
        ppu.cycle = 0;
        ppu.scanline++;
        if (ppu.scanline > 261) {
            ppu.scanline = 0;
            ppu.frame_odd ^= 1;
        }
    }
}

void ppu_run(int clocks) {
    for (int i = 0; i < clocks; i++) ppu_step();
}

/* =========================================================================
   FCEUX-faithful chunk-driven backend (--interp=fceux)
   -------------------------------------------------------------------------
   Ports FCEUX's old-PPU lazy render for sprite-0-hit timing (the residual that
   the beam+catch-up could not match — see AGENTS.md AxROM section). No per-dot
   beam: BG opacity is computed once per visible line (loopy v at line start) and
   the sprite-0 hit is checked LAZILY only at $2002 reads (lastpixel) and at line
   end (EndRL CheckSpriteHit(272)) — exactly FCEUX. This reproduces the tight
   poll's exact exit cycle that beam-bulk rendering shifts by one.
   ========================================================================= */
static uint8_t  fc_bgopac[256];  /* 1 = opaque BG pixel on the current line */
static int      fc_cur_line = -1;
static int32_t  fc_sphitx;       /* sprite-0 left X on the line, 0x100 = none */
static uint8_t  fc_sphitdata;    /* non-transparent pixel mask, MSB=leftmost,
                                    already horizontally flipped if needed */
/* Incremental render cursor (FCEUX firsttile / Pline / pshift[]). fc_pshift* hold
 * the last four fetched pattern bytes and deliberately persist across lines. */
static int      fc_firsttile;    /* next tile column to fetch */
static int      fc_px;           /* screen x of the next emitted pixel */
static uint32_t fc_pshift0, fc_pshift1;
static int      fc_tofix;        /* FCEUX tofix: inc_vert still owed for this line */

#define FC_TOFIXNUM (272 - 4)

static void fceux_check_sprite0(int lastpixel);

/* FCEUX Fixit1 = inc_vert, gated on rendering being enabled. */
static void fceux_fixit1(void) {
    if (ppu.regs[1] & 0x18) inc_vert_v();
}

static uint8_t fc_reverse8(uint8_t b) {
    return (uint8_t)(((b * 0x80200802ULL) & 0x0884422110ULL) * 0x0101010101ULL >> 32);
}

/* FCEUX RefreshLine(lastpixel): render the BG opacity for the tile columns that
 * have become visible since the last call, advancing ppu.v_addr (FCEUX's
 * RefreshAddr, written back at the end) and the two-tile pattern pipeline as it
 * goes.  This runs LAZILY — at every $2002 read and once more at EndRL(272) — so
 * a mid-line write to mirroring / $2000 / $2001 changes only the tiles rendered
 * after it.  Rendering the whole line up front instead makes such a write apply
 * retroactively to the entire line (Battletoads flips AxROM single-screen
 * mirroring mid-frame), and leaves v_addr un-advanced for a mid-line $2007.
 *
 * Pixels are emitted only for X1 >= 2: that is the two-tile fetch delay, so at
 * lastpixel only pixels below (lasttile-2)*8 are ready — which is exactly where
 * fceux_check_sprite0's "-16" comes from. */

static void fceux_refresh_line(int lastpixel) {
    if (fc_cur_line < 0) return;

    int lasttile = lastpixel >> 3;
    /* Render one extra tile while a sprite-0 hit is still pending, so the hit
     * test never runs ahead of the BG it is compared against. */
    if (fc_sphitx != 0x100 && !(ppu.regs[2] & 0x40)) {
        if (fc_sphitx < lastpixel - 16 && !(fc_sphitx < (lasttile - 2) * 8))
            lasttile++;
    }
    if (lasttile > 34) lasttile = 34;
    int numtiles = lasttile - fc_firsttile;
    if (numtiles <= 0) return;

    int bg  = ppu.regs[1] & 0x08;               /* ScreenON */
    int spr = ppu.regs[1] & 0x10;               /* SpriteON */

    if (!bg && !spr) {
        /* Rendering fully off: backdrop, and no fetches — v_addr does NOT move.
         * FCEUX advances its pixel cursor by numtiles*8 here, ignoring the
         * X1 >= 2 rule; mirror that. */
        for (int i = 0; i < numtiles * 8 && fc_px < 256; i++) fc_bgopac[fc_px++] = 0;
        fc_firsttile = lasttile;
        if (lastpixel >= FC_TOFIXNUM && fc_tofix) { fceux_fixit1(); fc_tofix = 0; }
        return;
    }

    mapper.m5_bg_chr = 1;                       /* MMC5: BG fetches use BG CHR banks */
    uint16_t ra   = ppu.v_addr;
    uint16_t ptb  = (ppu.regs[0] & 0x10) ? 0x1000 : 0x0000;
    int      xoff = ppu.fine_x;                 /* FCEUX XOffset, sampled per call */

    for (int X1 = fc_firsttile; X1 < lasttile; X1++) {
        if (X1 >= 2) {
            uint8_t opac = (uint8_t)(((fc_pshift0 | fc_pshift1) >> (8 - xoff)) & 0xFF);
            for (int i = 0; i < 8 && fc_px < 256; i++)
                fc_bgopac[fc_px++] = (opac >> (7 - i)) & 1;
        }
        uint16_t vadr = ptb + (uint16_t)(vram_read(0x2000 | (ra & 0x0FFF)) * 16)
                      + ((ra >> 12) & 7);
        fc_pshift0 = (fc_pshift0 << 8) | vram_read(vadr);
        fc_pshift1 = (fc_pshift1 << 8) | vram_read(vadr + 8);
        if ((ra & 0x1F) == 0x1F) ra ^= 0x041F; else ra++;
    }
    ppu.v_addr = ra;                            /* RefreshAddr = smorkus */

    /* BG off but sprites on: the fetches still happened (v_addr moved), but the
     * pixels they produced are backdrop. */
    if (!bg)
        for (int i = (fc_firsttile - 2) * 8; i < (lasttile - 2) * 8; i++)
            if (i >= 0 && i < 256) fc_bgopac[i] = 0;

    /* BG left-column clip (PPUMASK bit1 = 0 → leftmost 8 px hidden), applied on
     * the call that renders the tile carrying x = 0..7. */
    if (fc_firsttile <= 2 && 2 < lasttile && !(ppu.regs[1] & 0x02))
        for (int i = 0; i < 8; i++) fc_bgopac[i] = 0;

    /* FCEUX fires inc_vert here — on the FIRST line update at/after lastpixel 268,
     * not at EndRL — so the line's last tile (screen x 248..255) is fetched with
     * fine_y already advanced. Only observable because the render is incremental. */
    if (lastpixel >= FC_TOFIXNUM && fc_tofix) { fceux_fixit1(); fc_tofix = 0; }

    fceux_check_sprite0(lastpixel);
    fc_firsttile = lasttile;
}

/* FCEUX FCEUPPU_LineUpdate(): resolve the CPU's current dot into a lastpixel for
 * the line in progress and render up to it.  FCEUX calls this from every PPU
 * register access that can change how the rest of the line looks — reads of
 * $2002/$2004/$2007 (and open-bus $200x), writes of $2000/$2001/$2005/$2006/$2007
 * — which is what makes a mid-line write a raster split rather than a retroactive
 * repaint of the whole line. */
void fceux_line_update(void) {
    extern int g_fceux_dot, g_ppu_catchup_dots, g_fceux_vbase;
    if (fc_cur_line < 0 || fc_cur_line >= 240) return;
    /* g_fceux_dot counts from the top of the frame, which is the POST-RENDER line
     * — visible line 0 starts at g_fceux_vbase, so rebase before splitting into
     * (scanline, pixel). Without this the sl check never matches and sprite-0
     * hits are only seen at EndRL, one poll iteration late (Battletoads @1708). */
    int d = g_fceux_dot + g_ppu_catchup_dots - g_fceux_vbase;
    /* FCEUX starts a line's pixel clock at ResetRL, which DoLine calls 16 dots
     * BEFORE the nominal scanline boundary (`... ResetRL(); X6502_Run(16);`), so
     * GETLASTPIXEL runs 16 ahead of our nominal dot. */
    int px = d - fc_cur_line * 341 + 16;
    if (px > 0) fceux_refresh_line(px);
}

/* Compute sprite-0 sphitx/sphitdata for visible line sl (equivalent to FCEUX
 * FetchSpriteData+RefreshSprites for the next line). */
static void fceux_eval_sprite0(int sl) {
    fc_sphitx = 0x100;
    if (!(ppu.regs[1] & 0x10)) return;                 /* sprites disabled */
    if (!(ppu.regs[1] & 0x08)) return;                 /* BG off → no hit */

    int H   = (ppu.regs[0] & 0x20) ? 16 : 8;
    int row = sl - ((int)ppu.oam[0] + 1);
    if (row < 0 || row >= H) return;                   /* sprite 0 not on line */

    uint8_t tile = ppu.oam[1];
    uint8_t attr = ppu.oam[2];
    uint8_t spx  = ppu.oam[3];
    if (attr & 0x80) row = (H - 1) - row;              /* v-flip */

    uint16_t pt_base; uint8_t t;
    if (H == 8) { pt_base = (ppu.regs[0] & 0x08) ? 0x1000 : 0x0000; t = tile; }
    else { pt_base = (tile & 1) ? 0x1000 : 0x0000; t = tile & 0xFE;
           if (row >= 8) { t++; row -= 8; } }

    mapper.m5_bg_chr = 0;
    uint8_t lo = vram_read(pt_base + (uint16_t)t * 16 + row);
    uint8_t hi = vram_read(pt_base + (uint16_t)t * 16 + row + 8);
    uint8_t J  = lo | hi;                              /* non-transparent mask */
    if (attr & 0x40) J = fc_reverse8(J);               /* h-flip */

    fc_sphitx    = spx;
    fc_sphitdata = J;
}

/* FCEUX CheckSpriteHit(p): set bit6 when the first opaque sprite-0/BG overlap
 * pixel x in [sphitx,sphitx+8) satisfies x < lastpixel-16. */
static void fceux_check_sprite0(int lastpixel) {
    if (fc_sphitx == 0x100) return;
    if (ppu.regs[2] & 0x40) return;
    int l = lastpixel - 16;
    for (int x = fc_sphitx; x < fc_sphitx + 8 && x < l; x++) {
        if ((fc_sphitdata & (0x80 >> (x - fc_sphitx))) && fc_bgopac[x] && x < 255) {
            ppu.regs[2] |= 0x40;
            fc_sphitx = 0x100;
            break;
        }
    }
}


void fceux_prerender(void) {
    if (ppu.regs[1] & 0x18) copy_vert_v();   /* restore vertical v for line 0 */
}

void fceux_line_begin(int sl) {
    if (ppu.regs[1] & 0x18) copy_hori_v();   /* horizontal scroll for this line */
    fc_cur_line = sl;
    /* ResetRL: blank the line and rewind the render cursor. Nothing is drawn
     * here — the line is rendered lazily by fceux_refresh_line(). */
    for (int i = 0; i < 256; i++) fc_bgopac[i] = 0;
    fc_firsttile = 0;
    fc_px        = 0;
    fc_tofix     = 1;                        /* ResetRL arms the inc_vert latch */
    fceux_eval_sprite0(sl);
}

void fceux_line_end(int sl) {
    (void)sl;
    fceux_refresh_line(272);                  /* EndRL: finish the line */
    if (fc_tofix) { fceux_fixit1(); fc_tofix = 0; }   /* not latched mid-line */
    fceux_check_sprite0(272);
    fc_cur_line = -1;
}

/* Full-colour render of visible line sl into ppu.framebuf — for --interp=fceux
 * video/screenshots only (the sync path needs only opacity). Self-contained:
 * walks the line-start loopy v for BG (colour + palette), evaluates this line's
 * sprites (reusing eval_sprites), combines with priority, and looks up the NES
 * palette. Call AFTER fceux_line_begin (v is set up for the line); raster splits
 * are honoured because it runs per scanline with that line's CHR/scroll. */
void fceux_render_line(int sl) {
    uint32_t *outp = &ppu.framebuf[sl * SCREEN_W];
    uint8_t  *outi = &ppu.indexbuf[sl * SCREEN_W];

    uint8_t bgpix[256], bgpal[256];
    for (int i = 0; i < 256; i++) { bgpix[i] = 0; bgpal[i] = 0; }

    if (ppu.regs[1] & 0x08) {                       /* BG enabled */
        mapper.m5_bg_chr = 1;
        uint16_t v       = ppu.v_addr;
        uint16_t pt_base = (ppu.regs[0] & 0x10) ? 0x1000 : 0x0000;
        int      x       = 0;
        int      startbit = ppu.fine_x;
        while (x < 256) {
            uint16_t nt_addr = 0x2000 | (v & 0x0FFF);
            uint8_t  tile    = vram_read(nt_addr);
            uint16_t at_addr = 0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) | ((v >> 2) & 0x07);
            uint8_t  attr    = vram_read(at_addr);
            uint8_t  pal     = (attr >> (((v >> 4) & 4) | (v & 2))) & 3;
            uint8_t  fine_y  = (v >> 12) & 7;
            uint8_t  lo      = vram_read(pt_base + (uint16_t)tile * 16 + fine_y);
            uint8_t  hi      = vram_read(pt_base + (uint16_t)tile * 16 + fine_y + 8);
            for (int bit = startbit; bit < 8 && x < 256; bit++) {
                bgpix[x] = ((lo >> (7 - bit)) & 1) | (((hi >> (7 - bit)) & 1) << 1);
                bgpal[x] = pal;
                x++;
            }
            startbit = 0;
            if ((v & 0x001F) == 31) { v &= ~0x001F; v ^= 0x0400; } else v++;
        }
        if (!(ppu.regs[1] & 0x02))                  /* BG left-column clip */
            for (int i = 0; i < 8; i++) bgpix[i] = 0;
    }

    eval_sprites(sl);                               /* fills ppu.sp_* for this line */

    for (int x = 0; x < 256; x++) {
        uint8_t bgp = bgpix[x], bgpl = bgpal[x];
        uint8_t spp = 0, sppl = 0, sppri = 0;
        if ((ppu.regs[1] & 0x10) && (x >= 8 || (ppu.regs[1] & 0x04))) {
            for (int i = 0; i < ppu.sprite_count; i++) {
                int sx = x - (int)ppu.sp_x[i];
                if (sx < 0 || sx > 7) continue;
                uint8_t p = ((ppu.sp_pattern_lo[i] >> (7 - sx)) & 1)
                          | (((ppu.sp_pattern_hi[i] >> (7 - sx)) & 1) << 1);
                if (!p) continue;
                spp = p; sppl = (ppu.sp_attr[i] & 3) + 4; sppri = (ppu.sp_attr[i] >> 5) & 1;
                break;
            }
        }
        uint8_t pal_addr;
        if      (!bgp && !spp) pal_addr = 0;
        else if (!bgp &&  spp) pal_addr = sppl * 4 + spp;
        else if ( bgp && !spp) pal_addr = bgpl * 4 + bgp;
        else pal_addr = sppri ? (bgpl * 4 + bgp) : (sppl * 4 + spp);
        uint8_t color = vram_read(0x3F00 + pal_addr) & 0x3F;
        outp[x] = PALETTE[color];
        outi[x] = color;
    }
}