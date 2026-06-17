#include "runner.h"
#include "fm2_player.h"
#include <SDL2/SDL.h>
#include <signal.h>
#include <sys/stat.h>
#include <stdatomic.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* Flag set by SIGTERM handler so runner_run() exits cleanly */
static volatile int g_running = 1;

static void handle_sigterm(int sig) {
    (void)sig;
    g_running = 0;
}

/* Headless mode — run without SDL video for N seconds */
static int g_headless = 0;
static int g_seconds  = 30;
static uint32_t g_run_until = 0;

/* Interpreter mode — use cpu_interp_step instead of recompiled dispatch */
static int g_interp_mode = 0;

/* Frame hash dump — per-frame CRC32 to file for accuracy comparison with FCEUX */
static FILE    *g_frame_hash_file  = NULL;
static uint32_t g_frame_hash_count = 0;
static uint32_t g_frame_limit      = 0;   /* stop after N frames (0 = unlimited) */
/* PPU power-up warm-up: for the first frame(s) after reset the PPU sets no VBL
 * flag and fires no NMI (ppu.c gates the scanline-241 block on this), so the
 * game spins in its reset wait-loop. Decremented once per frame in runner_run
 * AFTER the dump logic reads it. Value 1 is correct under the unified FM2
 * timing model (record N applied at the start of frame N, see runner_run):
 * it yields exact lag sync (Mario/Battlecity/Felix/Zelda 100%, drift 0). */
int             g_ppudead          = 1;

/* --dump-sync mode: lag+RAM-hash log (format: "frame lag lagcount djb2").
 * lag and RAM are captured together at each VBL boundary; compare_lags.sh
 * aligns our log to FCEUX's with a constant offset (see runner_run). */
static FILE    *g_sync_file        = NULL;
static uint32_t g_lag_count        = 0;   /* cumulative lag-frame counter */

/* Frame counter for debug traces */
int g_current_frame = 0;

/* Hermetic mode: FM2 playback / lag-frame dumps must start from a clean
 * power-on SRAM (matching how the FCEUX movie was recorded) and must NOT
 * persist SRAM — otherwise a battery save from a previous run boots the game
 * into a different state and the demo desyncs (e.g. Zelda). Set in main(). */
static int g_hermetic = 0;

/* Total CPU cycles since power-on */
uint64_t g_total_cpu_cycles = 0;

/* FCEUX default NTSC palette (64 entries) — must match FCEUX for hash comparison */
static const uint8_t FCEUX_PAL_R[64] = {
    0x75, 0x24, 0x00, 0x45, 0x8E, 0xAA, 0xA6, 0x7D, 0x41, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
    0xBE, 0x00, 0x20, 0x82, 0xBE, 0xE7, 0xDB, 0xCB, 0x8A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0x3C, 0x5D, 0xCF, 0xF7, 0xFF, 0xFF, 0xFF, 0xF3, 0x82, 0x4D, 0x59, 0x00, 0x79, 0x00, 0x00,
    0xFF, 0xAA, 0xC7, 0xD7, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xE3, 0xAA, 0xB2, 0x9E, 0xC7, 0x00, 0x00,
};
static const uint8_t FCEUX_PAL_G[64] = {
    0x75, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x2C, 0x45, 0x51, 0x3C, 0x3C, 0x00, 0x00, 0x00,
    0xBE, 0x71, 0x38, 0x00, 0x00, 0x00, 0x28, 0x4D, 0x71, 0x96, 0xAA, 0x92, 0x82, 0x00, 0x00, 0x00,
    0xFF, 0xBE, 0x96, 0x8A, 0x79, 0x75, 0x75, 0x9A, 0xBE, 0xD3, 0xDF, 0xFB, 0xEB, 0x79, 0x00, 0x00,
    0xFF, 0xE7, 0xD7, 0xCB, 0xC7, 0xC7, 0xBE, 0xDB, 0xE7, 0xFF, 0xF3, 0xFF, 0xFF, 0xC7, 0x00, 0x00,
};
static const uint8_t FCEUX_PAL_B[64] = {
    0x75, 0x8E, 0xAA, 0x9E, 0x75, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x5D, 0x00, 0x00, 0x00,
    0xBE, 0xEF, 0xEF, 0xF3, 0xBE, 0x59, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x38, 0x8A, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xB6, 0x61, 0x38, 0x3C, 0x10, 0x49, 0x9A, 0xDB, 0x79, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xDB, 0xB2, 0xAA, 0xA2, 0xA2, 0xBE, 0xCF, 0xF3, 0xC7, 0x00, 0x00,
};

static uint32_t crc32_buf(const uint8_t *buf, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(uint32_t)(crc & 1));
    }
    return crc ^ 0xFFFFFFFF;
}

static uint32_t djb2_buf(const uint8_t *buf, size_t len) {
    uint32_t h = 5381;
    for (size_t i = 0; i < len; i++)
        h = h * 33 + buf[i];
    return h;
}

#ifndef DEFAULT_SCALE
#define DEFAULT_SCALE 1
#endif
static int g_scale = DEFAULT_SCALE;

/* Directory for save files — set from argv[0] in main() */
static char g_sav_dir[520] = "sav";

/* Optional screenshot path set via --screenshot FILE */
static const char *g_screenshot_path = NULL;

/* Save ppu.framebuf as a PNG file without requiring SDL video */
static void save_screenshot(const char *path) {
    int w = SCREEN_W, h = SCREEN_H;
    /* Convert ARGB8888 framebuf to packed RGB24 */
    uint8_t *rgb = (uint8_t *)malloc(w * h * 3);
    if (!rgb) { fprintf(stderr, "[screenshot] out of memory\n"); return; }
    for (int i = 0; i < w * h; i++) {
        uint32_t px = ppu.framebuf[i]; /* 0x00RRGGBB */
        rgb[i * 3 + 0] = (px >> 16) & 0xFF; /* R */
        rgb[i * 3 + 1] = (px >>  8) & 0xFF; /* G */
        rgb[i * 3 + 2] =  px        & 0xFF; /* B */
    }
    if (stbi_write_png(path, w, h, 3, rgb, w * 3))
        fprintf(stderr, "[screenshot] saved %s\n", path);
    else
        fprintf(stderr, "[screenshot] failed to write %s\n", path);
    free(rgb);
}

static void sav_mkdir(void) {
#ifdef _WIN32
    mkdir(g_sav_dir);
#else
    mkdir(g_sav_dir, 0755);
#endif
}

/* embedded data provided via -include generated/$(GAME)_embedded_data.h */

/* =========================================================================
   ROM loading (embedded data mode — no external ROM file needed)
   ========================================================================= */
static int load_rom(const char *path) {
    (void)path;

    prg_rom_size = EMBEDDED_PRG_SIZE;
    if (prg_rom_size > PRG_ROM_MAX)
        prg_rom_size = PRG_ROM_MAX;

    memcpy(prg_rom, embedded_prg_rom, prg_rom_size);
    memcpy(ppu.chr,  embedded_chr_rom, EMBEDDED_CHR_SIZE);

    mapper_init(EMBEDDED_MAPPER_ID, EMBEDDED_PRG_BANKS,
                EMBEDDED_CHR_BANKS, EMBEDDED_MIRRORING);

    return 1;
}

/* =========================================================================
   Audio
   ========================================================================= */
#define RING_SIZE 32768
#define RING_MASK (RING_SIZE - 1)
#define AUDIO_SAMPLES 512

static float ring_buf[RING_SIZE];

static _Atomic int ring_write = 0;
static _Atomic int ring_read  = 0;

static SDL_AudioDeviceID audio_dev = 0;

static void audio_callback(void *ud, Uint8 *stream, int len) {
    (void)ud;

    float *out = (float*)stream;
    int n = len / (int)sizeof(float);

    static float last_sample = 0.0f;

    for (int i = 0; i < n; i++) {
        int rp = atomic_load_explicit(&ring_read, memory_order_relaxed);
        int wp = atomic_load_explicit(&ring_write, memory_order_acquire);

        if (rp != wp) {
            last_sample = ring_buf[rp];
            atomic_store_explicit(&ring_read, (rp + 1) & RING_MASK, memory_order_release);
        }
        out[i] = last_sample;
    }
}

static inline void audio_push(float s) {
    int wp = atomic_load_explicit(&ring_write, memory_order_relaxed);
    int nw = (wp + 1) & RING_MASK;

    if (nw != atomic_load_explicit(&ring_read, memory_order_acquire)) {
        ring_buf[wp] = s;
        atomic_store_explicit(&ring_write, nw, memory_order_release);
    }
}

/* =========================================================================
   Save State
   ========================================================================= */

typedef struct {
    CPU cpu;
    PPU ppu;
    APU apu;
    Mapper mapper;

    uint8_t ram_copy[sizeof(ram)];
    uint8_t sram_copy[sizeof(sram)];

} SaveState;

static SaveState savestate;
static int state_exists = 0;

/* ------------------------------------------------------------------------- */
/* SAVE */
/* ------------------------------------------------------------------------- */

static void save_state(void) {

    savestate.cpu    = cpu;
    savestate.ppu    = ppu;
    savestate.apu    = apu;
    savestate.mapper = mapper;

    memcpy(savestate.ram_copy, ram, sizeof(ram));
    memcpy(savestate.sram_copy, sram, sizeof(sram));

    state_exists = 1;

    sav_mkdir();
    char path[600];
    snprintf(path, sizeof(path), "%s/%s.state", g_sav_dir, GAME_NAME);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(&savestate, 1, sizeof(savestate), f); fclose(f); }

    printf("[runner] State saved: %s\n", path);
}

/* ------------------------------------------------------------------------- */
/* LOAD */
/* ------------------------------------------------------------------------- */

static void load_state(void) {

    /* Try loading from disk first */
    char path[600];
    snprintf(path, sizeof(path), "%s/%s.state", g_sav_dir, GAME_NAME);
    FILE *f = fopen(path, "rb");
    if (f) {
        fread(&savestate, 1, sizeof(savestate), f);
        fclose(f);
        state_exists = 1;
    }

    if (!state_exists) {
        printf("[runner] No save state\n");
        return;
    }

    cpu    = savestate.cpu;
    ppu    = savestate.ppu;
    apu    = savestate.apu;
    mapper = savestate.mapper;

    memcpy(ram, savestate.ram_copy, sizeof(ram));
    memcpy(sram, savestate.sram_copy, sizeof(sram));

    printf("[runner] State loaded: %s\n", path);
}

/* =========================================================================
   Input
   ========================================================================= */
static void handle_key(SDL_Keycode k, int down) {
    if (fm2_active()) return;  /* ignore keyboard during TAS playback */
    uint8_t bit = 0;

    switch (k) {
    case SDLK_z:      bit = 0x80; break;
    case SDLK_x:      bit = 0x40; break;
    case SDLK_RSHIFT: bit = 0x20; break;
    case SDLK_RETURN: bit = 0x10; break;
    case SDLK_UP:     bit = 0x08; break;
    case SDLK_DOWN:   bit = 0x04; break;
    case SDLK_LEFT:   bit = 0x02; break;
    case SDLK_RIGHT:  bit = 0x01; break;
    default: return;
    }

    if (down)
        controller[0] |= bit;
    else
        controller[0] &= ~bit;
}

/* =========================================================================
   SDL objects
   ========================================================================= */
static SDL_Window   *window   = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture  *texture  = NULL;

static int fullscreen = 0;
static int widescreen = 0;

static int fps_counter = 0;
static Uint32 fps_timer = 0;

volatile int g_nmi_pending    = 0;
volatile int g_irq_pending    = 0;
         int g_nmi_just_fired  = 0;  /* set when NMI processed, cleared in ring-buf block */

void nes_nmi(void) {
    g_nmi_pending = 1;
}

void nes_irq(void) {
    if (!cpu.I)
        g_irq_pending = 1;
}

void nes_reset(void) {
    cpu.PC = mem_read(0xFFFC) |
            ((uint16_t)mem_read(0xFFFD) << 8);
}

/* Flush accumulated CPU cycles to PPU/APU.
 * Called from recompiled code before time-sensitive memory accesses (e.g. $2002 reads).
 * Final instruction cycles are flushed by the main loop after call_by_address() returns. */
void tick_ppu_apu(void) {
    if (g_cpu_cycles) {
        for (uint32_t _c = 0; _c < g_cpu_cycles; _c++) apu_step();
        for (uint32_t _c = 0; _c < g_cpu_cycles * 3; _c++) ppu_step();
        g_cpu_cycles = 0;
    }
}

/* =========================================================================
   Learning mode — automatic collection of dispatch misses
   ========================================================================= */
static void runner_miss_write_all(void);

static uint8_t *miss_map = NULL;   /* bitmap 8192 bytes = 65536 bits */
static char    *miss_path = NULL;

void runner_miss(uint16_t addr) {
    if (!miss_map) return;  /* learning disabled */
    if (addr < 0x8000) return;  /* only PRG ROM addresses are valid code */
    uint16_t idx = addr >> 3;
    uint8_t  bit = 1 << (addr & 7);
    if (miss_map[idx] & bit) return;   /* already seen */
    miss_map[idx] |= bit;
    /* log immediately (safe if killed mid-game) */
    FILE *f = fopen(miss_path, "a");
    if (f) {
        fprintf(f, "%04X\n", addr);
        fclose(f);
    }
}

/* =========================================================================
   Battery-backed SRAM persistence  (cfg/GAME.sav)
   ========================================================================= */
static void sram_load(void) {
#if EMBEDDED_BATTERY
    char path[600];
    snprintf(path, sizeof(path), "%s/%s_battery.sav", g_sav_dir, GAME_NAME);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fread(sram, 1, sizeof(sram), f);
    fclose(f);
    fprintf(stderr, "[sram] loaded %s\n", path);
#endif
}

static void sram_save(void) {
#if EMBEDDED_BATTERY
    sav_mkdir();
    char path[600];
    snprintf(path, sizeof(path), "%s/%s_battery.sav", g_sav_dir, GAME_NAME);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[sram] failed to write %s\n", path); return; }
    fwrite(sram, 1, sizeof(sram), f);
    fclose(f);
    fprintf(stderr, "[sram] saved %s\n", path);
#endif
}

static void runner_miss_init(void) {
    const char *mode = getenv("RECOMP_LEARN");
    if (!mode || *mode == '0') return;
    miss_map = calloc(65536 / 8, 1);
    if (!miss_map) return;
    const char *game = GAME_NAME;
    char path[512];
#ifdef _WIN32
    mkdir("cfg");
#else
    mkdir("cfg", 0755);
#endif
    snprintf(path, sizeof(path), "cfg/%s.cfg", game);
    miss_path = strdup(path);
    if (!miss_path) { free(miss_map); miss_map = NULL; return; }
    /* load existing misses to avoid duplicates */
    FILE *f = fopen(miss_path, "r");
    if (f) {
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            unsigned a = 0;
            if (sscanf(line, "extra_func = %x", &a) == 1 ||
                sscanf(line, "%x", &a) == 1) {
                if (a >= 0x8000 && a < 65536) {
                    uint16_t idx = a >> 3;
                    uint8_t  bit = 1 << (a & 7);
                    miss_map[idx] |= bit;
                }
            }
        }
        fclose(f);
    }
    fprintf(stderr, "[learn] logging miss addresses to %s\n", miss_path);
}

static void runner_miss_write_all(void) {
    if (!miss_map || !miss_path) return;
    /* build sorted list */
    uint16_t addrs[65536];
    int n = 0;
    for (uint32_t a = 0x8000; a < 65536; a++) {
        uint16_t idx = a >> 3;
        uint8_t  bit = 1 << (a & 7);
        if (miss_map[idx] & bit)
            addrs[n++] = (uint16_t)a;
    }
    if (n == 0) return;
    FILE *f = fopen(miss_path, "w");
    if (!f) return;
    fprintf(f, "# Auto-generated miss addresses (%d total)\n", n);
    for (int i = 0; i < n; i++)
        fprintf(f, "extra_func = %04X\n", addrs[i]);
    fclose(f);
    fprintf(stderr, "[learn] wrote %d addresses to %s\n", n, miss_path);
}

/* =========================================================================
   runner_init
   ========================================================================= */
int runner_init(const char *title, const char *rom_path) {
    (void)rom_path;

    if (g_headless) {
        /* Minimal init — no video, no audio */
        if (SDL_Init(SDL_INIT_TIMER) != 0) {
            fprintf(stderr, "[runner] SDL_Init: %s\n", SDL_GetError());
            return 0;
        }
        /* Enable learning automatically in headless mode */
        putenv("RECOMP_LEARN=1");
        g_run_until = SDL_GetTicks() + g_seconds * 1000;
        fprintf(stderr, "[runner] Headless mode — running for %d s\n", g_seconds);
    } else {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "[runner] SDL_Init: %s\n", SDL_GetError());
            return 0;
        }

        const char *display_name = title;

        window = SDL_CreateWindow(
        display_name,
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        SCREEN_W * g_scale,
        SCREEN_H * g_scale,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );

        if (!window) {
            fprintf(stderr, "[runner] Window: %s\n", SDL_GetError());
            return 0;
        }

        renderer = SDL_CreateRenderer(
            window,
            -1,
            SDL_RENDERER_ACCELERATED |
            SDL_RENDERER_PRESENTVSYNC
        );

        SDL_RenderSetLogicalSize(renderer, SCREEN_W, SCREEN_H);
        SDL_RenderSetIntegerScale(renderer, SDL_TRUE);

        texture = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            SCREEN_W,
            SCREEN_H
        );

        SDL_AudioSpec want, got;

        SDL_memset(&want, 0, sizeof(want));

        want.freq     = 44100;
        want.format   = AUDIO_F32SYS;
        want.channels = 1;
        want.samples  = AUDIO_SAMPLES;
        want.callback = audio_callback;

        audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);

        if (audio_dev)
            SDL_PauseAudioDevice(audio_dev, 0);
    }

    memset(&cpu, 0, sizeof(cpu));
    cpu.SP = 0xFD;
    cpu.I  = 1;

    /* FCEUX power-on RAM pattern: 00 00 00 00 FF FF FF FF repeating */
    for (int _ri = 0; _ri < (int)sizeof(ram); _ri++)
        ram[_ri] = (_ri & 4) ? 0xFF : 0x00;
    memset(sram, 0, sizeof(sram));
    if (!g_hermetic) sram_load();   /* skip battery load during playback/dump */
    memset(&ppu, 0, sizeof(ppu));
    memset(&apu, 0, sizeof(apu));

    apu.noise.shift_reg = 1;

    if (!load_rom(rom_path))
        return 0;

    cpu.PC = mem_read(0xFFFC) |
            ((uint16_t)mem_read(0xFFFD) << 8);

    fps_timer = SDL_GetTicks();

    runner_miss_init();

    /* Load FM2 playback file (must be parsed after CPU state is ready) */
    /* FM2 is loaded via --playback in main() */

    return 1;
}

/* =========================================================================
   runner_run
   ========================================================================= */
void runner_run(void) {

    int event_divider = 0;

    /* Unified FM2 timing (match FCEUX): apply movie record N at the START of
     * frame N. FCEUX calls FCEU_UpdateInput() (controller latch + reset/power
     * command) before emulating each frame. Our loop ticks the record at the
     * VBL that ENDS a frame, which applied record N to frame N+1 (one frame
     * late) — fine for input on most games but wrong for a frame-1 reset
     * (Battletoads booted an extra time). Pre-load record 1 here so frame N
     * consumes record N exactly like FCEUX; the in-loop tick then advances to
     * record N+1 at each frame end. */
    if (fm2_active()) {
        uint8_t c0 = 0, c1 = 0, cmd = 0;
        if (fm2_tick_cmd(&c0, &c1, &cmd)) {
            controller[0] = c0;
            controller[1] = c1;
            if (cmd & 3) nes_reset();
        }
        g_lag_flag = 1;   /* begin frame 1 */
    }

    while (g_running) {

        if (g_headless) {
            if (!g_frame_hash_file && !fm2_active() && SDL_GetTicks() >= g_run_until) {
                fprintf(stderr, "[runner] Headless run complete\n");
                return;
            }
        } else if (++event_divider >= 100) {

            event_divider = 0;

            SDL_Event ev;

            while (SDL_PollEvent(&ev)) {

                if (ev.type == SDL_QUIT)
                    return;

                if (ev.type == SDL_KEYDOWN) {

                    SDL_Keycode key = ev.key.keysym.sym;

                    if (key == SDLK_ESCAPE)
                        return;

                    /* Fullscreen */
                    if (key == SDLK_F11) {

                        fullscreen = !fullscreen;

                        SDL_SetWindowFullscreen(
                            window,
                            fullscreen ?
                            SDL_WINDOW_FULLSCREEN_DESKTOP : 0
                        );
                    }

                    /* Widescreen */
                    if (key == SDLK_TAB) {
                        widescreen = !widescreen;

                        if (widescreen) {
                            SDL_RenderSetLogicalSize(renderer, 0, 0);
                        } else {
                            SDL_RenderSetLogicalSize(renderer, SCREEN_W, SCREEN_H);
                        }
                    }

                    /* Screenshot F12 */
                    if (key == SDLK_F12) {
                        char path[64];
                        snprintf(path, sizeof(path), "screenshot_%u.png",
                                 (unsigned)SDL_GetTicks());
                        save_screenshot(path);
                    }

                    /* Save / Load */
                    if (key == SDLK_F5)
                        save_state();

                    if (key == SDLK_F8)
                        load_state();

                    handle_key(key, 1);
                }

                if (ev.type == SDL_KEYUP)
                    handle_key(ev.key.keysym.sym, 0);
            }
        }

        if (g_nmi_pending) {

            g_nmi_pending    = 0;
            g_nmi_just_fired = 1;

            stack_push((cpu.PC >> 8) & 0xFF);
            stack_push(cpu.PC & 0xFF);
            stack_push(get_P() & ~0x10);

            cpu.I = 1;

            cpu.PC = mem_read(0xFFFA) |
                    ((uint16_t)mem_read(0xFFFB) << 8);

            /* NMI delivery takes 7 CPU cycles on real 6502; advance PPU accordingly */
            for (int _i = 0; _i < 7 * 3; _i++)
                ppu_step();
        }

        if (g_irq_pending && !cpu.I) {

            g_irq_pending = 0;

            stack_push((cpu.PC >> 8) & 0xFF);
            stack_push(cpu.PC & 0xFF);
            stack_push(get_P() & ~0x10);

            cpu.I = 1;

            cpu.PC = mem_read(0xFFFE) |
                    ((uint16_t)mem_read(0xFFFF) << 8);

            /* IRQ delivery takes 7 CPU cycles on real 6502; advance PPU accordingly */
            for (int _i = 0; _i < 7 * 3; _i++)
                ppu_step();
        }

        /* PC ring buffer for stuck-loop detection */
        {
            static uint16_t pc_ring[64]    = {0};
            static int      pc_ring_pos    = 0;
            static uint64_t cycles_no_nmi  = 0;
            static int      stuck_reported = 0;
            static int      nmi_ever_fired = 0;

            extern int g_nmi_just_fired;
            if (g_nmi_just_fired) {
                nmi_ever_fired   = 1;
                cycles_no_nmi    = 0;
                stuck_reported   = 0;
                g_nmi_just_fired = 0;
            }
            pc_ring[pc_ring_pos & 63] = cpu.PC;
            pc_ring_pos++;
            if (nmi_ever_fired)
                cycles_no_nmi += (g_cpu_cycles ? g_cpu_cycles : 1);

            /* ~3 frames without NMI = stuck (3*29780 cycles) */
            if (nmi_ever_fired && cycles_no_nmi > 90000 && !stuck_reported) {
                stuck_reported = 1;
                fprintf(stderr, "[stuck] No NMI for %llu cycles! PC=$%04X bank=%d. Last 32 PCs:\n",
                        (unsigned long long)cycles_no_nmi,
                        cpu.PC, mapper.m1_prg_bank);
                for (int _i = 32; _i > 0; _i--) {
                    int idx = (pc_ring_pos - _i) & 63;
                    fprintf(stderr, "  $%04X", pc_ring[idx]);
                    if (_i % 8 == 1) fprintf(stderr, "\n");
                }
                extern uint8_t ram[2048];
                fprintf(stderr, "[stuck] RAM: $0013=%02X $0014=%02X $0023=%02X $0024=%02X $09=%02X $10=%02X\n",
                        ram[0x13], ram[0x14], ram[0x23], ram[0x24],
                        ram[0x09], ram[0x10]);
                fprintf(stderr, "[stuck] PPU: $2000=%02X $2001=%02X $2002=%02X SP=$%02X A=$%02X\n",
                        ppu.regs[0], ppu.regs[1], ppu.regs[2], cpu.SP, cpu.A);
                /* Check if sprite-0 (tile 0) has any pixels in CHR RAM */
                {
                    int has_px = 0;
                    for (int b = 0; b < 16; b++) if (ppu.chr[b]) { has_px = 1; break; }
                    fprintf(stderr, "[stuck] CHR tile0: %s OAM[0]=%02X,%02X,%02X,%02X\n",
                            has_px ? "has pixels" : "BLANK",
                            ppu.oam[0], ppu.oam[1], ppu.oam[2], ppu.oam[3]);
                    fprintf(stderr, "[stuck] CHR tile0 bytes: ");
                    for (int b = 0; b < 16; b++) fprintf(stderr, "%02X ", ppu.chr[b]);
                    fprintf(stderr, "\n");
                    /* Nametable at column 31, rows 3-4 — respect current mirroring */
                    {
                        uint16_t base = (mapper.mirroring == 4) ? 0x0400 : 0x0000;
                        uint16_t nt_idx_3 = base | ((3*32+31) & 0x3FF);
                        uint16_t nt_idx_4 = base | ((4*32+31) & 0x3FF);
                        fprintf(stderr, "[stuck] NT[row3,col31]=%02X NT[row4,col31]=%02X (mir=%d)\n",
                                ppu.vram[nt_idx_3], ppu.vram[nt_idx_4], mapper.mirroring);
                    }
                    /* $2000 bit4=1 → BG uses PT1 ($1000); bit3=0 → sprite PT0 ($0000) */
                    uint8_t bg_pt = (ppu.regs[0] & 0x10) ? 1 : 0;
                    uint8_t sp_pt = (ppu.regs[0] & 0x08) ? 1 : 0;
                    uint16_t bg_t61_base = (bg_pt ? 0x1000 : 0x0000) + 0x610;
                    fprintf(stderr, "[stuck] BG PT=%d SP PT=%d  BG tile$61 base=$%04X bytes: ",
                            bg_pt, sp_pt, bg_t61_base);
                    for (int b = 0; b < 16; b++) fprintf(stderr, "%02X ", ppu.chr[bg_t61_base + b]);
                    fprintf(stderr, "\n");
                    /* Scroll state: v_addr and t_addr encode scroll position */
                    fprintf(stderr, "[stuck] v_addr=$%04X t_addr=$%04X fine_x=%d scan=%d cycle=%d\n",
                            ppu.v_addr, ppu.t_addr, ppu.fine_x, ppu.scanline, ppu.cycle);
                    /* Coarse scroll from v_addr: bits 4-0 = coarse X, bits 9-5 = coarse Y */
                    int coarse_x = ppu.v_addr & 0x1F;
                    int coarse_y = (ppu.v_addr >> 5) & 0x1F;
                    int fine_y = (ppu.v_addr >> 12) & 7;
                    fprintf(stderr, "[stuck] scroll: coarseX=%d coarseY=%d fineY=%d (BG starts at screen Y=%d)\n",
                            coarse_x, coarse_y, fine_y, coarse_y*8 + fine_y);
                }
            }
        }

        if (g_interp_mode) cpu_interp_step(); else call_by_address(cpu.PC);

        if (g_cpu_cycles == 0)
            g_cpu_cycles = 1;

        g_total_cpu_cycles += g_cpu_cycles;

        for (uint32_t c = 0; c < g_cpu_cycles; c++)
            apu_step();

        for (uint32_t c = 0; c < g_cpu_cycles * 3; c++)
            ppu_step();

        g_cpu_cycles = 0;

        /* Drain DMC DMA stalls: the CPU was halted for those cycles while the
         * PPU/APU kept running. Advancing them here (no CPU executed) shrinks
         * the CPU's effective per-frame budget like hardware, so DMC-heavy
         * games (e.g. Castlevania III) lag a bit more, matching FCEUX. Byte
         * fetches are >400 cyc apart so this short drain never re-arms. */
        { extern uint32_t g_dmc_stall;
          if (g_dmc_stall) {
            uint32_t s = g_dmc_stall; g_dmc_stall = 0;
            for (uint32_t c = 0; c < s; c++)     apu_step();
            for (uint32_t c = 0; c < s * 3; c++) ppu_step();
          }
        }

        if (ppu.frame_ready) {
            ppu.frame_ready = 0;

            g_current_frame++;

            /* Frame hash dump for accuracy comparison */
            if (g_frame_hash_file) {
                uint32_t h;
                if (g_ppudead > 0) {
                    /* FCEUX ppudead: force gray (palette index 0 = 0x75,0x75,0x75) */
                    h = 0x4A964AF8u;
                } else {
                    static uint8_t argb_buf[SCREEN_W * SCREEN_H * 4];
                    for (int _i = 0; _i < SCREEN_W * SCREEN_H; _i++) {
                        uint8_t _idx = ppu.indexbuf[_i] & 0x3F;
                        argb_buf[_i*4+0] = 0x00;
                        argb_buf[_i*4+1] = FCEUX_PAL_R[_idx];
                        argb_buf[_i*4+2] = FCEUX_PAL_G[_idx];
                        argb_buf[_i*4+3] = FCEUX_PAL_B[_idx];
                    }
                    h = crc32_buf(argb_buf, sizeof(argb_buf));
                }
                g_frame_hash_count++;
                fprintf(g_frame_hash_file, "%06u %08X\n", g_frame_hash_count, h);
                if (g_frame_limit && g_frame_hash_count >= g_frame_limit)
                    g_running = 0;
            }

            /* Sync dump (lag-sequence metric — see AGENTS.md "Synchronization
             * Methodology"). Capture lag bit AND RAM hash at the SAME boundary
             * (this VBL start) so a single constant offset can align our log to
             * FCEUX's registerafter log. Our boundary is before this frame's NMI
             * handler runs; FCEUX's is after — that is a CONSTANT shift, which
             * compare_lags.sh detects. The previous code captured lag and RAM at
             * different boundaries, so no single offset could align both. */
            if (g_sync_file) {
                if (g_lag_flag) g_lag_count++;
                fprintf(g_sync_file, "%d %d %u %08X\n",
                        g_current_frame,
                        g_lag_flag,
                        g_lag_count,
                        djb2_buf(ram, 0x800));
                if (g_frame_limit && g_current_frame >= (int)g_frame_limit)
                    g_running = 0;
            }

            /* General frame cap: stop after N emulated frames regardless of
             * mode (dumps, screenshot, or plain playback). FM2 playback ignores
             * the wall-clock --seconds limit so demos can run to the end at full
             * speed; --frames is the speed-independent way to stop at a given
             * point — e.g. a screenshot at "T seconds" = --frames (T*60). */
            if (g_frame_limit && g_current_frame >= (int)g_frame_limit)
                g_running = 0;

            /* Count down warm-up once per frame, AFTER both dumps read it
             * (framebuffer gray + PPU VBL/NMI suppression key off g_ppudead). */
            if (g_ppudead > 0) g_ppudead--;

            /* Advance FM2 once per VBlank — AFTER hash emit, so next frame uses new input */
            if (fm2_active()) {
                g_lag_flag = 1;  /* reset lag flag for next frame */
                uint8_t c0 = 0, c1 = 0, fm2_cmd = 0;
                if (!fm2_tick_cmd(&c0, &c1, &fm2_cmd))
                    g_running = 0;
                controller[0] = c0;
                controller[1] = c1;
                if (fm2_cmd & 3) {
                    nes_reset();
                }
            }

            if (!g_headless) {
                for (int s = 0; s < apu.sample_count; s++) {
                    audio_push(apu.sample_buf[s]);
                }
                apu.sample_count = 0;

                SDL_UpdateTexture(
                    texture,
                    NULL,
                    ppu.framebuf,
                    SCREEN_W * 4
                );

                SDL_RenderClear(renderer);

                if (widescreen) {

                    SDL_Rect dst;

                    int ww, wh;

                    SDL_GetWindowSize(window, &ww, &wh);

                    dst.x = 0;
                    dst.y = 0;
                    dst.w = ww;
                    dst.h = wh;

                    SDL_RenderCopy(renderer, texture, NULL, &dst);

                } else {

                    SDL_RenderCopy(renderer, texture, NULL, NULL);
                }

                SDL_RenderPresent(renderer);

                /* FPS */
                fps_counter++;

                Uint32 now = SDL_GetTicks();

                if (now - fps_timer >= 1000) {

                    char title[256];

                    snprintf(
                        title,
                        sizeof(title),
                        "NESRecomp | FPS: %d",
                        fps_counter
                    );

                    SDL_SetWindowTitle(window, title);

                    fps_counter = 0;
                    fps_timer = now;
                }
            }

        }
    }
}

/* =========================================================================
   runner_quit
   ========================================================================= */
void runner_quit(void) {

    if (g_screenshot_path) save_screenshot(g_screenshot_path);
    if (!g_hermetic) sram_save();   /* don't persist battery during playback/dump */
    runner_miss_write_all();
    free(miss_map);
    free(miss_path);
    miss_map = NULL;

    if (!g_headless) {
        if (audio_dev)
            SDL_CloseAudioDevice(audio_dev);

        if (texture)
            SDL_DestroyTexture(texture);

        if (renderer)
            SDL_DestroyRenderer(renderer);

        if (window)
            SDL_DestroyWindow(window);
    }

    fm2_free();

    if (g_frame_hash_file) { fclose(g_frame_hash_file); g_frame_hash_file = NULL; }
    if (g_sync_file) {
        /* Each frame is emitted inline at its VBL boundary — nothing pending. */
        fclose(g_sync_file); g_sync_file = NULL;
    }

    SDL_Quit();
}

/* =========================================================================
   main
   ========================================================================= */

int main(int argc, char **argv) {

    signal(SIGTERM, handle_sigterm);
    signal(SIGINT,  handle_sigterm);

    /* Build sav dir path: dirname(argv[0])/sav */
    {
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%s", argv[0]);
        char *slash = strrchr(tmp, '/');
#ifdef _WIN32
        char *bslash = strrchr(tmp, '\\');
        if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
        if (slash) {
            *slash = '\0';
            snprintf(g_sav_dir, sizeof(g_sav_dir), "%s/sav", tmp);
        } else {
            snprintf(g_sav_dir, sizeof(g_sav_dir), "sav");
        }
    }

    const char *rom_path = NULL;
    const char *playback_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0)
            g_headless = 1;
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            g_seconds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--playback") == 0 && i + 1 < argc)
            playback_path = argv[++i];
        else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
            g_scale = atoi(argv[++i]);
        else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            g_screenshot_path = argv[++i];
        else if (strcmp(argv[i], "--interp") == 0)
            g_interp_mode = 1;
        else if (strcmp(argv[i], "--dump-frames") == 0 && i + 1 < argc) {
            g_frame_hash_file = fopen(argv[++i], "w");
            g_headless = 1;
        }
        else if (strcmp(argv[i], "--dump-sync") == 0 && i + 1 < argc) {
            g_sync_file = fopen(argv[++i], "w");
            g_headless = 1;
        }
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            g_frame_limit = (uint32_t)atoi(argv[++i]);
        else if (argv[i][0] != '-')
            rom_path = argv[i];
    }

    /* Hermetic: playback or lag/frame dumps must start from a clean power-on
     * SRAM and not persist it (match the FCEUX movie's recording conditions). */
    g_hermetic = (playback_path != NULL) || g_sync_file || g_frame_hash_file;

    if (playback_path) {
        if (!fm2_open(playback_path))
            return 1;
    }

    if (!runner_init("NESRecomp", rom_path))
        return 1;

    runner_run();

    runner_quit();

    return 0;
}