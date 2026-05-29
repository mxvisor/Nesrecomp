#include "runner.h"
#include <SDL2/SDL.h>
#include <signal.h>
#include <sys/stat.h>

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

/* FM2 TAS playback */
static uint8_t *playback_buf = NULL;   /* [playback_total][2] */
static int playback_total = 0;
static int playback_frame = 0;

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

static volatile int ring_write = 0;
static volatile int ring_read  = 0;

static SDL_AudioDeviceID audio_dev = 0;

static void audio_callback(void *ud, Uint8 *stream, int len) {
    (void)ud;

    float *out = (float*)stream;
    int n = len / (int)sizeof(float);

    static float last_sample = 0.0f;

    for (int i = 0; i < n; i++) {
        int next_read = (ring_read + 1) & RING_MASK;

        if (next_read != ring_write) {
            last_sample = ring_buf[ring_read];
            out[i] = last_sample;
            ring_read = next_read;
        } else {
            out[i] = last_sample;
        }
    }
}

static inline void audio_push(float s) {
    int nw = (ring_write + 1) & RING_MASK;

    if (nw != ring_read) {
        ring_buf[ring_write] = s;
        ring_write = nw;
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

    printf("[runner] State saved\n");
}

/* ------------------------------------------------------------------------- */
/* LOAD */
/* ------------------------------------------------------------------------- */

static void load_state(void) {

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

    /* FIX black screen after load */
    printf("[runner] State loaded\n");
}

/* =========================================================================
   Input
   ========================================================================= */
static void handle_key(SDL_Keycode k, int down) {
    if (playback_buf) return;  /* ignore keyboard during TAS playback */
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

volatile int g_nmi_pending = 0;
volatile int g_irq_pending = 0;

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

/* =========================================================================
   Learning mode — автоматический сбор промахов dispatch
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

static void runner_miss_init(void) {
    const char *mode = getenv("RECOMP_LEARN");
    if (!mode || *mode == '0') return;
    miss_map = calloc(65536 / 8, 1);
    if (!miss_map) return;
    const char *game = getenv("GAME");
    if (!game) game = "game";
    char path[512];
    mkdir("cfg", 0755);
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
   FM2 TAS movie loader
   ========================================================================= */
static uint8_t fm2_to_nes(const char *s) {
    uint8_t v = 0;
    if (strlen(s) >= 8) {
        if (s[0] != '.') v |= 0x01;  /* Right */
        if (s[1] != '.') v |= 0x02;  /* Left */
        if (s[2] != '.') v |= 0x04;  /* Down */
        if (s[3] != '.') v |= 0x08;  /* Up */
        if (s[4] != '.') v |= 0x10;  /* Start (T) */
        if (s[5] != '.') v |= 0x20;  /* Select (S) */
        if (s[6] != '.') v |= 0x40;  /* B */
        if (s[7] != '.') v |= 0x80;  /* A */
    }
    return v;
}

static int fm2_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[fm2] cannot open %s\n", path); return 0; }

    /* Count frames */
    char line[1024];
    int raw = 0;
    while (fgets(line, sizeof(line), f))
        if (line[0] == '|') raw++;
    if (raw == 0) { fclose(f); return 0; }

    playback_buf = malloc(raw * 2);
    if (!playback_buf) { fclose(f); return 0; }

    rewind(f);
    int idx = 0;
    while (fgets(line, sizeof(line), f) && idx < raw) {
        if (line[0] != '|') continue;
        int skip;
        char p0[16] = "", p1[16] = "";
        int n = sscanf(line, "|%d|%8[^|]|%8[^|]", &skip, p0, p1);
        if (n < 2) continue;
        uint8_t c0 = fm2_to_nes(p0);
        uint8_t c1 = fm2_to_nes(p1);
        int repeat = skip + 1;
        for (int r = 0; r < repeat && idx < raw; r++) {
            playback_buf[idx * 2 + 0] = c0;
            playback_buf[idx * 2 + 1] = c1;
            idx++;
        }
    }

    playback_total = idx;
    fprintf(stderr, "[fm2] loaded %d frames from %s\n", playback_total, path);
    fclose(f);
    return 1;
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
        SCREEN_W * 3,
        SCREEN_H * 3,
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

    memset(ram,  0, sizeof(ram));
    memset(sram, 0, sizeof(sram));
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
    int last_frame_scanline = -1;

    while (g_running) {

        if (g_headless) {
            if (playback_buf) {
                /* FM2 playback controls the duration */
            } else if (SDL_GetTicks() >= g_run_until) {
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

            /* Advance FM2 playback one frame */
            if (playback_buf && playback_frame < playback_total) {
                controller[0] = playback_buf[playback_frame * 2 + 0];
                controller[1] = playback_buf[playback_frame * 2 + 1];
                if (++playback_frame >= playback_total) {
                    fprintf(stderr, "[fm2] playback complete (%d frames)\n",
                            playback_total);
                    g_running = 0;
                }
            }

            g_nmi_pending = 0;

            stack_push((cpu.PC >> 8) & 0xFF);
            stack_push(cpu.PC & 0xFF);
            stack_push(get_P() & ~0x10);

            cpu.I = 1;

            cpu.PC = mem_read(0xFFFA) |
                    ((uint16_t)mem_read(0xFFFB) << 8);
        }

        if (g_irq_pending && !cpu.I) {

            g_irq_pending = 0;

            stack_push((cpu.PC >> 8) & 0xFF);
            stack_push(cpu.PC & 0xFF);
            stack_push(get_P() & ~0x10);

            cpu.I = 1;

            cpu.PC = mem_read(0xFFFE) |
                    ((uint16_t)mem_read(0xFFFF) << 8);
        }

        call_by_address(cpu.PC);

        if (g_cpu_cycles == 0)
            g_cpu_cycles = 1;

        for (uint32_t c = 0; c < g_cpu_cycles; c++)
            apu_step();

        for (uint32_t c = 0; c < g_cpu_cycles * 3; c++)
            ppu_step();

        g_cpu_cycles = 0;

        if (ppu.scanline == 241 &&
            last_frame_scanline != 241) {

            last_frame_scanline = 241;

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

        } else if (ppu.scanline != 241) {

            last_frame_scanline = ppu.scanline;
        }
    }
}

/* =========================================================================
   runner_quit
   ========================================================================= */
void runner_quit(void) {

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

    free(playback_buf);
    playback_buf = NULL;

    SDL_Quit();
}

/* =========================================================================
   main
   ========================================================================= */

int main(int argc, char **argv) {

    signal(SIGTERM, handle_sigterm);

    const char *rom_path = NULL;
    const char *playback_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0)
            g_headless = 1;
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            g_seconds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--playback") == 0 && i + 1 < argc)
            playback_path = argv[++i];
        else if (argv[i][0] != '-')
            rom_path = argv[i];
    }

    if (playback_path) {
        if (!fm2_load(playback_path))
            return 1;
    }

    if (!runner_init("NESRecomp", rom_path))
        return 1;

    runner_run();

    runner_quit();

    return 0;
}