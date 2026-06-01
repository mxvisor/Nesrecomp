#ifndef APU_H
#define APU_H

/*
 * apu.h — APU (Audio Processing Unit): pulse, triangle, noise, DMC
 *
 * Scope: audio synthesis only.
 * If audio is wrong see apu.c.
 * APU has no knowledge of PPU or mapper.
 *
 * Architecture:
 *   apu_step()  called once per CPU cycle (~1.79 MHz)
 *   Samples accumulate in sample_buf[]
 *   Runner drains them into ring buffer -> SDL audio callback
 *
 * Channels:
 *   pulse[0], pulse[1]  — square waves with sweep unit
 *   tri                 — triangle wave
 *   noise               — noise generator with LFSR
 *   dmc                 — delta modulation (PCM samples from ROM)
 */

#include <stdint.h>

typedef struct {
    /* Pulse 1 & 2 */
    struct {
        uint8_t  duty;           /* waveform duty (0-3) */
        uint8_t  length_halt;    /* halt length counter / loop envelope */
        uint8_t  constant_vol;   /* constant volume flag */
        uint8_t  volume;         /* volume / envelope period */
        uint16_t timer;          /* current timer */
        uint16_t timer_reload;   /* timer period */
        uint8_t  length;         /* length counter */
        uint8_t  enabled;        /* channel enabled ($4015) */
        /* Sweep unit */
        uint8_t  sweep_en;
        uint8_t  sweep_period;
        uint8_t  sweep_negate;
        uint8_t  sweep_shift;
        uint8_t  sweep_reload;
        uint8_t  sweep_divider;
        /* Envelope */
        uint8_t  envelope_start;
        uint8_t  envelope_divider;
        uint8_t  envelope_decay;
        /* Sequencer */
        uint32_t seq_pos;
    } pulse[2];

    /* Triangle */
    struct {
        uint8_t  control;           /* linear counter halt */
        uint8_t  linear_reload_val;
        uint16_t timer, timer_reload;
        uint8_t  length, enabled;
        uint8_t  linear_counter, linear_reload;
        uint32_t seq_pos;
    } tri;

    /* Noise */
    struct {
        uint8_t  length_halt, constant_vol, volume;
        uint16_t timer, timer_reload;
        uint8_t  length, enabled, mode;
        uint16_t shift_reg;         /* LFSR, init=1 */
        uint8_t  envelope_start, envelope_divider, envelope_decay;
    } noise;

    /* DMC (Delta Modulation Channel) */
    struct {
        uint8_t  irq_en, loop, rate_idx, enabled;
        uint8_t  output;            /* current output level (0-127) */
        uint16_t sample_addr, sample_len;
        uint16_t cur_addr, bytes_remaining;
        uint8_t  sample_buf, bits_remaining, silence;
    } dmc;

    /* Frame sequencer */
    uint8_t  frame_counter_mode;  /* 0=4-step, 1=5-step */
    uint8_t  frame_irq_inhibit;
    uint32_t frame_clock;

    /* Output samples (accumulated in apu_step, flushed by runner) */
    float    sample_buf[4096];
    int      sample_count;
} APU;

extern APU apu;

/* Write APU register (called from memory.c) */
void    apu_write(uint16_t addr, uint8_t val);

/* Read $4015 status */
uint8_t apu_read_status(void);

/* One CPU cycle */
void    apu_step(void);

/* Drain accumulated samples into buf (for audio callback) */
void    apu_fill_buffer(float *buf, int samples);

#endif /* APU_H */
