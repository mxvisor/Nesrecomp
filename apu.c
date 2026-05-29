#include "runner.h"
#include <math.h>

APU apu;

/* =========================================================================
   Tables
   ========================================================================= */
static const uint8_t LENGTH_TABLE[32] = {
    10,254,20, 2,40, 4,80, 6,160, 8,60,10,14,12,26,14,
    12, 16,24,18,48,20,96,22,192,24,72,26,16,28,32,30
};

static const uint8_t DUTY_SEQ[4][8] = {
    {0,1,0,0,0,0,0,0},
    {0,1,1,0,0,0,0,0},
    {0,1,1,1,1,0,0,0},
    {1,0,0,1,1,1,1,1},
};

static const uint8_t TRI_SEQ[32] = {
    15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,
     0, 1, 2, 3, 4, 5,6,7,8,9,10,11,12,13,14,15
};

static const uint16_t NOISE_PERIOD[16] = {
    4,8,16,32,64,96,128,160,202,254,380,508,762,1016,2034,4068
};

/* =========================================================================
   Forward declarations
   ========================================================================= */
static void clock_quarter_frame(void);
static void clock_half_frame(void);

/* =========================================================================
   Register writes
   ========================================================================= */
void apu_write(uint16_t addr, uint8_t val) {
    switch (addr) {
    /* Pulse 1 */
    case 0x4000:
        apu.pulse[0].duty         = val >> 6;
        apu.pulse[0].length_halt  = (val >> 5) & 1;
        apu.pulse[0].constant_vol = (val >> 4) & 1;
        apu.pulse[0].volume       = val & 0xF;
        break;
    case 0x4001:
        apu.pulse[0].sweep_en     = (val >> 7) & 1;
        apu.pulse[0].sweep_period = (val >> 4) & 7;
        apu.pulse[0].sweep_negate = (val >> 3) & 1;
        apu.pulse[0].sweep_shift  = val & 7;
        apu.pulse[0].sweep_reload = 1;
        break;
    case 0x4002:
        apu.pulse[0].timer_reload = (apu.pulse[0].timer_reload & 0x700) | val;
        break;
    case 0x4003:
        apu.pulse[0].timer_reload = (apu.pulse[0].timer_reload & 0xFF) | ((uint16_t)(val&7)<<8);
        if (apu.pulse[0].enabled) apu.pulse[0].length = LENGTH_TABLE[val >> 3];
        apu.pulse[0].envelope_start = 1;
        apu.pulse[0].seq_pos = 0;
        break;
    /* Pulse 2 */
    case 0x4004:
        apu.pulse[1].duty         = val >> 6;
        apu.pulse[1].length_halt  = (val >> 5) & 1;
        apu.pulse[1].constant_vol = (val >> 4) & 1;
        apu.pulse[1].volume       = val & 0xF;
        break;
    case 0x4005:
        apu.pulse[1].sweep_en     = (val >> 7) & 1;
        apu.pulse[1].sweep_period = (val >> 4) & 7;
        apu.pulse[1].sweep_negate = (val >> 3) & 1;
        apu.pulse[1].sweep_shift  = val & 7;
        apu.pulse[1].sweep_reload = 1;
        break;
    case 0x4006:
        apu.pulse[1].timer_reload = (apu.pulse[1].timer_reload & 0x700) | val;
        break;
    case 0x4007:
        apu.pulse[1].timer_reload = (apu.pulse[1].timer_reload & 0xFF) | ((uint16_t)(val&7)<<8);
        if (apu.pulse[1].enabled) apu.pulse[1].length = LENGTH_TABLE[val >> 3];
        apu.pulse[1].envelope_start = 1;
        apu.pulse[1].seq_pos = 0;
        break;
    /* Triangle */
    case 0x4008:
        apu.tri.control          = (val >> 7) & 1;
        apu.tri.linear_reload_val = val & 0x7F;
        break;
    case 0x400A:
        apu.tri.timer_reload = (apu.tri.timer_reload & 0x700) | val;
        break;
    case 0x400B:
        apu.tri.timer_reload = (apu.tri.timer_reload & 0xFF) | ((uint16_t)(val&7)<<8);
        if (apu.tri.enabled) apu.tri.length = LENGTH_TABLE[val >> 3];
        apu.tri.linear_reload = 1;
        break;
    /* Noise */
    case 0x400C:
        apu.noise.length_halt  = (val >> 5) & 1;
        apu.noise.constant_vol = (val >> 4) & 1;
        apu.noise.volume       = val & 0xF;
        break;
    case 0x400E:
        apu.noise.mode         = (val >> 7) & 1;
        apu.noise.timer_reload = NOISE_PERIOD[val & 0xF];
        break;
    case 0x400F:
        if (apu.noise.enabled) apu.noise.length = LENGTH_TABLE[val >> 3];
        apu.noise.envelope_start = 1;
        break;
    /* DMC */
    case 0x4010:
        apu.dmc.irq_en   = (val >> 7) & 1;
        apu.dmc.loop     = (val >> 6) & 1;
        apu.dmc.rate_idx = val & 0xF;
        break;
    case 0x4011:
        apu.dmc.output = val & 0x7F;
        break;
    case 0x4012:
        apu.dmc.sample_addr = 0xC000 | ((uint16_t)val << 6);
        break;
    case 0x4013:
        apu.dmc.sample_len = ((uint16_t)val << 4) + 1;
        break;
    /* Status $4015 */
    case 0x4015:
        apu.pulse[0].enabled = (val >> 0) & 1;
        apu.pulse[1].enabled = (val >> 1) & 1;
        apu.tri.enabled      = (val >> 2) & 1;
        apu.noise.enabled    = (val >> 3) & 1;
        apu.dmc.enabled      = (val >> 4) & 1;
        if (!apu.pulse[0].enabled) apu.pulse[0].length = 0;
        if (!apu.pulse[1].enabled) apu.pulse[1].length = 0;
        if (!apu.tri.enabled)      apu.tri.length = 0;
        if (!apu.noise.enabled)    apu.noise.length = 0;
        break;
    /* Frame counter $4017 */
    case 0x4017:
        apu.frame_counter_mode = (val >> 7) & 1;
        apu.frame_irq_inhibit  = (val >> 6) & 1;
        apu.frame_clock = 0;
        /* 5-step: immediately clock quarter + half frame */
        if (apu.frame_counter_mode)
            clock_half_frame();
        break;
    }
}

uint8_t apu_read_status(void) {
    return ((apu.pulse[0].length    > 0) ? 0x01 : 0)
         | ((apu.pulse[1].length    > 0) ? 0x02 : 0)
         | ((apu.tri.length         > 0) ? 0x04 : 0)
         | ((apu.noise.length       > 0) ? 0x08 : 0)
         | ((apu.dmc.bytes_remaining > 0) ? 0x10 : 0);
}

/* =========================================================================
   Envelope clocking
   ========================================================================= */
static void clock_envelope(int ch) {
    if (apu.pulse[ch].envelope_start) {
        apu.pulse[ch].envelope_start   = 0;
        apu.pulse[ch].envelope_decay   = 15;
        apu.pulse[ch].envelope_divider = apu.pulse[ch].volume;
    } else {
        if (apu.pulse[ch].envelope_divider == 0) {
            apu.pulse[ch].envelope_divider = apu.pulse[ch].volume;
            if (apu.pulse[ch].envelope_decay > 0)
                apu.pulse[ch].envelope_decay--;
            else if (apu.pulse[ch].length_halt)
                apu.pulse[ch].envelope_decay = 15;
        } else {
            apu.pulse[ch].envelope_divider--;
        }
    }
}

static void clock_noise_envelope(void) {
    if (apu.noise.envelope_start) {
        apu.noise.envelope_start   = 0;
        apu.noise.envelope_decay   = 15;
        apu.noise.envelope_divider = apu.noise.volume;
    } else {
        if (apu.noise.envelope_divider == 0) {
            apu.noise.envelope_divider = apu.noise.volume;
            if (apu.noise.envelope_decay > 0)
                apu.noise.envelope_decay--;
            else if (apu.noise.length_halt)
                apu.noise.envelope_decay = 15;
        } else {
            apu.noise.envelope_divider--;
        }
    }
}

/* =========================================================================
   Sweep unit for pulse channels
   ========================================================================= */

/* Compute sweep target period with correct negative clamping.
   Per wiki: target is clamped to 0 if negative (not wrapped as uint16_t).
   Pulse 1 uses ones'-complement negate (-c-1), Pulse 2 uses twos'-complement (-c). */
static uint16_t sweep_target(int ch) {
    int32_t t = (int32_t)apu.pulse[ch].timer_reload;
    int32_t delta = t >> apu.pulse[ch].sweep_shift;
    if (apu.pulse[ch].sweep_negate) delta = (ch == 0) ? -delta - 1 : -delta;
    int32_t target = t + delta;
    if (target < 0) target = 0;
    return (uint16_t)target;
}

/* Returns 1 if sweep unit mutes the channel (always applies, even when disabled). */
static int sweep_muted(int ch) {
    return (apu.pulse[ch].timer_reload < 8) || (sweep_target(ch) > 0x7FF);
}

static void clock_sweep(int ch) {
    uint16_t target = sweep_target(ch);

    /* Clock divider */
    if (apu.pulse[ch].sweep_reload) {
        apu.pulse[ch].sweep_reload   = 0;
        apu.pulse[ch].sweep_divider  = apu.pulse[ch].sweep_period;
    } else if (apu.pulse[ch].sweep_divider == 0) {
        apu.pulse[ch].sweep_divider  = apu.pulse[ch].sweep_period;
        /* Update period if enabled, shift > 0, and not muting */
        if (apu.pulse[ch].sweep_en &&
            apu.pulse[ch].sweep_shift > 0 &&
            !sweep_muted(ch)) {
            apu.pulse[ch].timer_reload = target;
        }
    } else {
        apu.pulse[ch].sweep_divider--;
    }
}

/* =========================================================================
   Frame sequencer
   ========================================================================= */
/* Frame sequencer fires at APU cycles 3728/7456/11185/14914 (4-step).
   apu_step() is called every CPU cycle, so frame_clock counts CPU cycles.
   1 APU cycle = 2 CPU cycles → multiply APU cycle counts by 2.
   Values from blargg's apu_ref.txt (standard NES emulation reference). */
static const uint32_t SEQ4[4] = {7457, 14913, 22371, 29829};
static const uint32_t SEQ5[5] = {7457, 14913, 22371, 29829, 37281};

static void clock_quarter_frame(void) {
    clock_envelope(0);
    clock_envelope(1);
    clock_noise_envelope();
    /* Triangle linear counter */
    if (apu.tri.linear_reload)
        apu.tri.linear_counter = apu.tri.linear_reload_val;
    else if (apu.tri.linear_counter > 0)
        apu.tri.linear_counter--;
    if (!apu.tri.control) apu.tri.linear_reload = 0;
}

static void clock_half_frame(void) {
    clock_quarter_frame();
    /* Length counters */
    for (int ch = 0; ch < 2; ch++) {
        if (!apu.pulse[ch].length_halt && apu.pulse[ch].length > 0)
            apu.pulse[ch].length--;
        clock_sweep(ch);
    }
    if (!apu.tri.control && apu.tri.length > 0) apu.tri.length--;
    if (!apu.noise.length_halt && apu.noise.length > 0) apu.noise.length--;
}

/* =========================================================================
   Mixer — NES non-linear mixing
   ========================================================================= */
static float mix_output(void) {
    /* Pulse output — muted by sweep unit when period < 8 or target > $7FF
       (applies regardless of sweep enable flag, per hardware spec) */
    uint8_t p0 = 0, p1 = 0;
    for (int ch = 0; ch < 2; ch++) {
        if (apu.pulse[ch].length > 0 && !sweep_muted(ch)) {
            uint8_t vol = apu.pulse[ch].constant_vol ? apu.pulse[ch].volume
                                                     : apu.pulse[ch].envelope_decay;
            uint8_t out = DUTY_SEQ[apu.pulse[ch].duty][apu.pulse[ch].seq_pos & 7] * vol;
            if (ch == 0) p0 = out; else p1 = out;
        }
    }

    /* Triangle output — when halted (length=0 or linear=0) the sequencer
       freezes but the DAC holds the last step value (frozen DC), not silence.
       Mute only at ultrasonic periods (timer < 2) to avoid aliasing clicks. */
    uint8_t tri = 0;
    if (apu.tri.timer_reload >= 2)
        tri = TRI_SEQ[apu.tri.seq_pos & 31];

    /* Noise output */
    uint8_t noise = 0;
    if (apu.noise.length > 0) {
        uint8_t vol = apu.noise.constant_vol ? apu.noise.volume
                                             : apu.noise.envelope_decay;
        if (!(apu.noise.shift_reg & 1)) noise = vol;
    }

    /* DMC output */
    uint8_t dmc = apu.dmc.output;

    /* NES APU non-linear mixer formula (nesdev) */
    float pulse_out = 0.0f;
    if (p0 || p1)
        pulse_out = 95.88f / (8128.0f / (p0 + p1) + 100.0f);

    float tnd_out = 0.0f;
    float tnd = tri / 8227.0f + noise / 12241.0f + dmc / 22638.0f;
    if (tnd > 0.0f)
        tnd_out = 159.79f / (1.0f / tnd + 100.0f);

    return (pulse_out + tnd_out);  /* 0..~1.0 */
}

/* =========================================================================
   APU step — one CPU cycle
   ========================================================================= */
/* Fixed-point sample accumulator: counts CPU cycles * 100 */
/* Sample threshold = CPU_HZ/SAMPLE_HZ * 100 = 1789773/44100*100 ~= 4058 */
static int32_t sample_frac = 0;
#define SAMPLE_FRAC_STEP  100
#define SAMPLE_FRAC_MAX   4058   /* 1789773*100/44100 */

/* Pulse channels clock every 2 CPU cycles (APU cycle), not every CPU cycle */
static uint8_t pulse_half = 0;

void apu_step(void) {
    apu.frame_clock++;
    pulse_half ^= 1;

    /* Frame sequencer */
    const uint32_t *seq   = apu.frame_counter_mode ? SEQ5 : SEQ4;
    int             steps = apu.frame_counter_mode ? 5 : 4;
    for (int i = 0; i < steps; i++) {
        if (apu.frame_clock == seq[i]) {
            if (!apu.frame_counter_mode) {
                /* 4-step: Q at every step, H at steps 1 and 3 */
                if (i == 1 || i == 3) clock_half_frame();
                else                   clock_quarter_frame();
            } else {
                /* 5-step: Q+H at steps 1 and 4, Q at steps 0 and 2, nothing at step 3 */
                if (i == 1 || i == 4)      clock_half_frame();
                else if (i == 0 || i == 2) clock_quarter_frame();
                /* i == 3: no clock */
            }
            break;
        }
    }
    if (apu.frame_clock >= (uint32_t)(apu.frame_counter_mode ? 37282 : 29830))
        apu.frame_clock = 0;

    /* Clock pulse timers every 2 CPU cycles (= 1 APU cycle) */
    if (pulse_half) {
        for (int ch = 0; ch < 2; ch++) {
            if (apu.pulse[ch].timer == 0) {
                apu.pulse[ch].timer = apu.pulse[ch].timer_reload;
                /* Sequencer always advances even when sweep mutes; muting is applied in mix_output */
                apu.pulse[ch].seq_pos = (apu.pulse[ch].seq_pos + 1) & 7;
            } else {
                apu.pulse[ch].timer--;
            }
        }
    }

    /* Triangle — clocked every CPU cycle */
    if (apu.tri.length > 0 && apu.tri.linear_counter > 0) {
        if (apu.tri.timer == 0) {
            apu.tri.timer = apu.tri.timer_reload;
            apu.tri.seq_pos = (apu.tri.seq_pos + 1) & 31;
        } else {
            apu.tri.timer--;
        }
    }

    /* Noise */
    if (apu.noise.timer == 0) {
        apu.noise.timer = apu.noise.timer_reload;
        uint16_t fb = (apu.noise.shift_reg & 1)
                    ^ ((apu.noise.shift_reg >> (apu.noise.mode ? 6 : 1)) & 1);
        apu.noise.shift_reg = (apu.noise.shift_reg >> 1) | (fb << 14);
    } else {
        apu.noise.timer--;
    }

    /* Sample output — apply NES hardware HPF chain before storing */
    sample_frac += SAMPLE_FRAC_STEP;
    if (sample_frac >= SAMPLE_FRAC_MAX) {
        sample_frac -= SAMPLE_FRAC_MAX;
        if (apu.sample_count < 4096) {
            float x = mix_output();
            /* NES hardware filter chain (post-DAC):
               HPF 90 Hz  — α = 0.98726, removes DC bias
               HPF 440 Hz — α = 0.93923, removes low-frequency rumble
               LPF 14 kHz — α = 0.13606, removes high-frequency aliasing
               y[n] = α*(y[n-1] + x[n] - x[n-1])  for HPF
               y[n] = α*y[n-1] + (1-α)*x[n]        for LPF */
            static float hp1_in = 0.0f, hp1_out = 0.0f;
            float h1 = 0.98726f * (hp1_out + x - hp1_in);
            hp1_in  = x;  hp1_out = h1;

            static float hp2_in = 0.0f, hp2_out = 0.0f;
            float h2 = 0.93923f * (hp2_out + h1 - hp2_in);
            hp2_in  = h1; hp2_out = h2;

            static float lp_out = 0.0f;
            lp_out = 0.13606f * lp_out + (1.0f - 0.13606f) * h2;

            /* Scale to SDL float range -1..1 */
            apu.sample_buf[apu.sample_count++] = lp_out * 2.5f;
        }
    }
}

void apu_fill_buffer(float *buf, int samples) {
    for (int i = 0; i < samples; i++)
        buf[i] = (i < apu.sample_count) ? apu.sample_buf[i] : 0.0f;
    apu.sample_count = 0;
}
