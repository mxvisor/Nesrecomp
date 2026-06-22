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
        /* Trace all page-1 NT writes (offsets $400-$7FF) when mirroring=4 */
        if (mapper.mirroring == 4 && offs >= 0x400 && offs < 0x800)
            fprintf(stderr, "[nt1] write $%02X offs=$%03X scan=%d mir=%d\n",
                    val, offs, ppu.scanline, mapper.mirroring);
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
        /* fceux chunk-driven backend: no per-dot beam. A $2002 read triggers
         * FCEUX's lazy LineUpdate — render/sprite-0-check only up to this read's
         * exact dot (lastpixel). See fceux_on_2002_read() and runner_run_fceux. */
        if ((reg & 7) == 2) fceux_on_2002_read();
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
    ppu.open_bus = val;
    ppu.regs[reg & 7] = val;
    switch (reg & 7) {
    case 0:
        ppu.t_addr = (ppu.t_addr & ~0x0C00) | ((uint16_t)(val & 3) << 10);
        if ((val & 0x80) && (ppu.regs[2] & 0x80))
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

static uint8_t fc_reverse8(uint8_t b) {
    return (uint8_t)(((b * 0x80200802ULL) & 0x0884422110ULL) * 0x0101010101ULL >> 32);
}

/* Render the BG opacity (pattern != 0) for visible line sl into fc_bgopac, using
 * the loopy v at line start (mirrors RefreshLine's smorkus local walk). */
static void fceux_render_bg_opacity(void) {
    for (int i = 0; i < 256; i++) fc_bgopac[i] = 0;
    if (!(ppu.regs[1] & 0x08)) return;                 /* BG disabled */

    mapper.m5_bg_chr = 1;                               /* MMC5: select BG CHR banks */
    uint16_t v       = ppu.v_addr;
    uint16_t pt_base = (ppu.regs[0] & 0x10) ? 0x1000 : 0x0000;
    int      x       = 0;
    int      startbit = ppu.fine_x;

    while (x < 256) {
        uint16_t nt_addr = 0x2000 | (v & 0x0FFF);
        uint8_t  tile    = vram_read(nt_addr);
        uint8_t  fine_y  = (v >> 12) & 7;
        uint8_t  lo      = vram_read(pt_base + (uint16_t)tile * 16 + fine_y);
        uint8_t  hi      = vram_read(pt_base + (uint16_t)tile * 16 + fine_y + 8);
        for (int bit = startbit; bit < 8 && x < 256; bit++) {
            uint8_t p = ((lo >> (7 - bit)) & 1) | (((hi >> (7 - bit)) & 1) << 1);
            fc_bgopac[x++] = p ? 1 : 0;
        }
        startbit = 0;
        /* inc hori v */
        if ((v & 0x001F) == 31) { v &= ~0x001F; v ^= 0x0400; } else v++;
    }

    /* BG left-column clip (PPUMASK bit1 = 0 → leftmost 8 px hidden) */
    if (!(ppu.regs[1] & 0x02))
        for (int i = 0; i < 8; i++) fc_bgopac[i] = 0;
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

void fceux_on_2002_read(void) {
    extern int g_fceux_dot, g_ppu_catchup_dots;
    int d  = g_fceux_dot + g_ppu_catchup_dots;
    int sl = d / 341, px = d % 341;
    if (sl == fc_cur_line && sl < 240)
        fceux_check_sprite0(px);
}

void fceux_prerender(void) {
    if (ppu.regs[1] & 0x18) copy_vert_v();   /* restore vertical v for line 0 */
}

void fceux_line_begin(int sl) {
    if (ppu.regs[1] & 0x18) copy_hori_v();   /* horizontal scroll for this line */
    fc_cur_line = sl;
    fceux_render_bg_opacity();
    fceux_eval_sprite0(sl);
}

void fceux_line_end(int sl) {
    (void)sl;
    fceux_check_sprite0(272);                 /* EndRL */
    if (ppu.regs[1] & 0x18) inc_vert_v();     /* advance to next line's row */
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