/* apu_fceux.c — NON-GPL reimplementation of FCEUX's sync-critical APU timing
 * (DMC prefetch-DMA cycle-steal + DMC IRQ + frame-counter IRQ) for the
 * --interp=fceux backend, so the GPL apu_vendor.c can be dropped.
 *
 * WHY THIS EXISTS (see memory apu-two-tasks-fceux-vs-beam / fceux-backend-contraf):
 * FCEUX charges the DMC sample-fetch DMA on a PER-INSTRUCTION hook (FCEU_SoundCPUHook),
 * so the 4-cycle CPU stall lands on the exact instruction FCEUX picks. Our shared
 * apu.c models the DMC per-CPU-cycle, which jitters the stall onto a neighbouring
 * instruction → Contraf's music-RNG churn ($0029) window flips ±1..4 iterations →
 * lag desync. This file reproduces the FCEUX per-instruction batching so the fceux
 * backend is cycle-identical to the vendor oracle. It owns ONLY DMC/frame timing +
 * the $4015 status bits 4/6/7 + the $4010-$4017 timing registers; audio waveform
 * generation stays in apu.c (which reads $4011 for the DMC DAC level).
 *
 * The NTSC DMC period table and DMA semantics are public hardware behaviour; the
 * per-instruction batching is the FCEUX modelling choice we deliberately match.
 * Used only on the --interp=fceux (cpu_interp) path (guarded by g_ppu_backend);
 * the beam and recompiled backends keep apu.c's per-cycle DMC untouched.
 */
#include "runner.h"
#include "interrupts.h"

extern uint32_t g_dmc_stall;

/* Cycles deferred to the NEXT per-instruction hook, replicating FCEUX X6502_Run:
 * an instruction's page-cross/branch extras, its DMC-stall cycles, and interrupt
 * delivery (7) are ADDCYC'd AFTER the SoundCPUHook, so they land in the following
 * hook. The runner (fceux_run_to) accumulates them here and passes base+deferred. */
int g_apuf_deferred = 0;

/* NTSC DMC period table — CPU cycles per output-bit clock (hardware). */
static const int32_t DMC_PERIOD_TBL[16] = {
    428,380,340,320,286,254,226,214,190,160,142,128,106,84,72,54
};

/* Frame counter — FCEUX runs fhcnt in 1/48-CPU-cycle units (sound.cpp). */
static int32_t fhcnt = 0, fhinc = 0;
static uint8_t fcnt = 0;
static uint8_t frame_mode = 0;   /* bit0 = IRQ inhibit, bit1 = 5-step (from $4017 bits 6/7) */
static uint8_t sirq = 0;         /* status latch: bit6 = frame IRQ, bit7 = DMC IRQ */

/* DMC */
static int32_t  dmc_acc = 1;
static int32_t  dmc_period = 428;
static uint32_t dmc_addr = 0;
static int32_t  dmc_size = 0;
static uint8_t  dmc_format = 0;      /* $4010: bit7 irq_en, bit6 loop, bits0-3 rate */
static uint8_t  dmc_addr_latch = 0, dmc_size_latch = 0;
static uint8_t  dmc_bitcount = 0;
static uint8_t  dmc_have_dma = 0;

static void prep_dpcm(void) {
    dmc_addr = 0x4000 + ((uint32_t)dmc_addr_latch << 6);
    dmc_size = ((int32_t)dmc_size_latch << 4) + 1;
}

/* Frame-counter IRQ (FCEUX FrameSoundUpdate). Only the IRQ matters for sync;
 * channel length/sweep/envelope clocking lives in apu.c (audio). */
static void frame_update(void) {
    if (!fcnt && !(frame_mode & 0x3)) { sirq |= 0x40; nes_irq(); }  /* 4-step, not inhibited */
    if (fcnt == 3 && (frame_mode & 0x2)) fhcnt += fhinc;            /* 5-step extra step */
    fcnt = (fcnt + 1) & 3;
}

/* DMC sample-byte prefetch DMA — charges the 4-cycle CPU stall and reads the byte
 * (real bus read, like hardware/FCEUX; affects open bus). Guarded by dmc_have_dma
 * so it happens once per consumed byte. */
static void dmc_dma(void) {
    if (dmc_size && !dmc_have_dma) {
        g_dmc_stall += 4;
        (void)mem_read((uint16_t)(0x8000 + dmc_addr));   /* FCEUX: 4x X6502_DMR; last read latched (audio only) */
        dmc_have_dma = 1;
        dmc_addr = (dmc_addr + 1) & 0x7FFF;
        dmc_size--;
        if (!dmc_size) {
            if (dmc_format & 0x40)      prep_dpcm();                 /* loop */
            else if (dmc_format & 0x80) { sirq |= 0x80; nes_irq(); } /* DMC IRQ */
        }
    }
}

/* Byte consumed every 8 bit-clocks → free the DMA slot for the next prefetch. */
static void dmc_tester(void) {
    if (dmc_bitcount == 0 && dmc_have_dma) dmc_have_dma = 0;
}

/* FCEU_SoundCPUHook — called once per CPU instruction with that instruction's
 * cycle count (incl. FCEUX's deferred extras/stalls/interrupt cycles). Mirrors
 * apu_vendor.c apuv_hook exactly. */
void apu_fceux_hook(int cycles) {
    fhcnt -= cycles * 48;
    if (fhcnt <= 0) { frame_update(); fhcnt += fhinc; }

    dmc_dma();
    dmc_acc -= cycles;
    while (dmc_acc <= 0) {
        dmc_acc += dmc_period;            /* always > 0 (table 54..428) */
        dmc_bitcount = (dmc_bitcount + 1) & 7;
        dmc_tester();
    }
}

/* Timing-relevant register writes ($4010-$4013, $4015, $4017). Audio-only regs
 * ($4000-$400F, $4011 DAC) are handled by apu.c apu_write in parallel. */
void apu_fceux_write(uint16_t a, uint8_t v) {
    switch (a) {
    case 0x4010:
        dmc_period = DMC_PERIOD_TBL[v & 0xF];
        if (sirq & 0x80) {                 /* DMC IRQ pending */
            if (!(v & 0x80)) { sirq &= ~0x80; g_irq_pending = 0; }  /* clearing irq_en acks it */
        }
        dmc_format = v;
        break;
    case 0x4012: dmc_addr_latch = v; break;
    case 0x4013: dmc_size_latch = v; break;
    case 0x4015:
        if (v & 0x10) { if (!dmc_size) prep_dpcm(); }
        else           dmc_size = 0;
        sirq &= ~0x80; g_irq_pending = 0;  /* $4015 write acks DMC IRQ */
        break;
    case 0x4017:
        v = (uint8_t)((v & 0xC0) >> 6);    /* bit0 = inhibit, bit1 = 5-step */
        fcnt = 0;
        if (v & 0x2) frame_update();       /* 5-step: immediate clock */
        fcnt = 1;
        fhcnt = fhinc;
        sirq &= ~0x40; g_irq_pending = 0;  /* $4017 write acks frame IRQ */
        frame_mode = v;
        break;
    default: break;
    }
}

/* $4015 read: bit4 = DMC active, bit6 = frame IRQ, bit7 = DMC IRQ. Reading clears
 * the frame IRQ flag only (not DMC), per hardware/FCEUX apuv_status_read. */
uint8_t apu_fceux_status(void) {
    uint8_t ret = (uint8_t)((dmc_size ? 0x10 : 0) | (sirq & 0xC0));
    sirq &= ~0x40; g_irq_pending = 0;      /* reading $4015 acks the frame IRQ */
    return ret;
}

/* FCEUX power-on (FCEUSND_Power). */
void apu_fceux_power(void) {
    fhinc = 14915 * 24;
    fhcnt = fhinc;
    fcnt = 0;
    frame_mode = 0;
    sirq = 0;
    dmc_acc = 1; dmc_period = DMC_PERIOD_TBL[0];
    dmc_addr = 0; dmc_size = 0; dmc_format = 0;
    dmc_addr_latch = dmc_size_latch = 0;
    dmc_bitcount = 0; dmc_have_dma = 0;
    g_dmc_stall = 0;
    g_apuf_deferred = 0;
}

/* FCEUX soft reset (FCEUSND_Reset): DMCacc=1, DMCBitCount=0, frame counter reset;
 * DMCPeriod intentionally left as-is. */
void apu_fceux_reset(void) {
    frame_mode = 0;
    fhcnt = fhinc;
    fcnt = 0;
    sirq = 0;
    dmc_have_dma = 0;
    dmc_addr_latch = dmc_size_latch = dmc_format = 0;
    dmc_addr = 0; dmc_size = 0;
    dmc_acc = 1; dmc_bitcount = 0;
    g_dmc_stall = 0;
    g_apuf_deferred = 0;
}
