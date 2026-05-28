#include "runner.h"
#include <SDL2/SDL.h>

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

static volatile int g_nmi_pending = 0;
static volatile int g_irq_pending = 0;

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
   runner_init
   ========================================================================= */
int runner_init(const char *title, const char *rom_path) {
    (void)rom_path;

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

    return 1;
}

/* =========================================================================
   runner_run
   ========================================================================= */
void runner_run(void) {

    int event_divider = 0;
    int last_frame_scanline = -1;

    while (1) {

        if (++event_divider >= 100) {

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

        int cyc = cpu_interp_step();

        if (cyc <= 0)
            cyc = 1;

        for (int c = 0; c < cyc; c++)
            apu_step();

        for (int c = 0; c < cyc * 3; c++)
            ppu_step();

        if (ppu.scanline == 241 &&
            last_frame_scanline != 241) {

            last_frame_scanline = 241;

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

        } else if (ppu.scanline != 241) {

            last_frame_scanline = ppu.scanline;
        }
    }
}

/* =========================================================================
   runner_quit
   ========================================================================= */
void runner_quit(void) {

    if (audio_dev)
        SDL_CloseAudioDevice(audio_dev);

    if (texture)
        SDL_DestroyTexture(texture);

    if (renderer)
        SDL_DestroyRenderer(renderer);

    if (window)
        SDL_DestroyWindow(window);

    SDL_Quit();
}

/* =========================================================================
   main
   ========================================================================= */
int main(int argc, char **argv) {

    const char *rom_path = (argc >= 2) ? argv[1] : NULL;

    if (!runner_init("NESRecomp", rom_path))
        return 1;

    runner_run();

    runner_quit();

    return 0;
}