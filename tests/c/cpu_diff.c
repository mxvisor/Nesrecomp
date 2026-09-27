/* cpu_diff.c — differential test: recompiled code vs cpu_interp_step().
 *
 * Built by tests/test_cpu_diff.py together with src/{memory,cpu_interp,ppu,
 * apu,apu_fceux,mapper}.c, a generated T_full.c (one single-instruction
 * function per case) and a generated cases.c (PRG image + case table).
 * runner.c is not linked (it needs SDL); the few globals it owns are stubbed
 * below.
 *
 * For every case and a number of random CPU/RAM states, the instruction is
 * executed once by the interpreter and once by the recompiled function from
 * the same starting state; registers, flags, the 2 KB RAM, the cycle count
 * and (for control flow) the new PC must match. Operands are chosen by the
 * generator so that every access stays in RAM or PRG — no I/O side effects.
 */
#include <stdio.h>
#include <string.h>
#include "runner.h"

/* ---- stubs for symbols owned by runner.c ---- */
int g_ppudead = 0, g_ppu_backend = 0, g_ppu_catchup_dots = 0, g_ppu_caught_up = 0;
int g_fceux_dot = 0, g_fceux_vbase = 0;
volatile int g_nmi_pending = 0, g_irq_pending = 0;
void nes_nmi(void) {}
void nes_irq(void) {}
void runner_miss(uint16_t addr) { (void)addr; }
void tick_ppu_apu(void) {}   /* cases never touch I/O */

/* ---- provided by the generated cases.c ---- */
typedef struct {
    uint8_t     opcode;
    uint16_t    pc;
    void      (*fn)(void);
    uint8_t     indirect_zp;   /* 1 = (zp,X), 2 = (zp),Y: keep zero-page pointers in RAM */
    uint8_t     control_flow;  /* compare PC */
    uint8_t     branch;        /* compare PC only when taken */
    uint8_t     size;
    const char *name;
} diff_case_t;
extern const uint8_t     test_prg[32768];
extern const diff_case_t test_cases[];
extern const int         test_case_count;

static uint32_t rng_state = 0x12345678u;
static uint32_t rnd(void) {           /* xorshift32 — deterministic */
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

typedef struct { CPU cpu; uint8_t ram[0x800]; uint32_t cycles; } snap_t;

static void take(snap_t *s) {
    s->cpu = cpu;
    memcpy(s->ram, ram, sizeof(s->ram));
    s->cycles = g_cpu_cycles;
}
static void put(const snap_t *s) {
    cpu = s->cpu;
    memcpy(ram, s->ram, sizeof(s->ram));
    g_cpu_cycles = s->cycles;
}

static void randomize(const diff_case_t *c) {
    for (int i = 0; i < 0x800; i++) ram[i] = (uint8_t)rnd();
    if (c->indirect_zp)
        for (int i = 0; i < 0x100; i++) ram[i] &= 0x07;   /* pointers -> $0000-$07FF */
    uint32_t r = rnd();
    cpu.A = (uint8_t)r; cpu.X = (uint8_t)(r >> 8); cpu.Y = (uint8_t)(r >> 16);
    cpu.SP = (uint8_t)(r >> 24);
    set_P((uint8_t)rnd());
    cpu.D = 0;                                           /* 2A03 has no decimal mode */
    cpu.PC = c->pc;
    g_cpu_cycles = 0;
}

/* Zero-page address the pointer of an indirect instruction is fetched from. */
static uint8_t zp_pointer(const diff_case_t *c) {
    uint8_t op = mem_read((uint16_t)(c->pc + 1));
    return c->indirect_zp == 1 ? (uint8_t)(op + cpu.X) : op;
}

/* Usage: cpu_diff [iterations] [wrap]
 * Default mode skips iterations whose indirect pointer sits at $FF (see
 * "wrap" below) and compares everything else.
 * "wrap" mode tests ONLY those iterations: the pointer's high byte must be
 * fetched from $00 (zero-page wrap), as on hardware and in FCEUX. */
int main(int argc, char **argv) {
    int iters = argc > 1 ? atoi(argv[1]) : 64;
    int wrap_mode = argc > 2 && strcmp(argv[2], "wrap") == 0;
    int fails = 0, reported = 0, wrap_seen = 0;

    mapper_init(0, 2, 1, 0);
    memcpy(prg_rom, test_prg, sizeof(test_prg));
    prg_rom_size = sizeof(test_prg);

    for (int k = 0; k < test_case_count; k++) {
        const diff_case_t *c = &test_cases[k];
        int case_failed = 0;
        for (int it = 0; it < iters && !case_failed; it++) {
            snap_t start, ref, got;
            randomize(c);
            if (wrap_mode) {
                if (!c->indirect_zp) break;
                if (c->indirect_zp == 1)                 /* force (op + X) == $FF */
                    cpu.X = (uint8_t)(0xFF - mem_read((uint16_t)(c->pc + 1)));
                if (zp_pointer(c) != 0xFF) break;        /* (zp),Y with operand != $FF */
                ram[0x100] |= 0x80;                      /* make a $0100 read visible */
                wrap_seen++;
            } else if (c->indirect_zp && zp_pointer(c) == 0xFF) {
                continue;                                /* covered by "wrap" mode */
            }
            take(&start);

            cpu_interp_step();
            take(&ref);

            put(&start);
            c->fn();
            take(&got);

            char why[160] = "";
            uint8_t pr = (uint8_t)(0), pg = (uint8_t)(0);
            { CPU s = cpu; cpu = ref.cpu; pr = get_P() & 0xCF; cpu = got.cpu; pg = get_P() & 0xCF; cpu = s; }
            if      (ref.cpu.A  != got.cpu.A)  snprintf(why, sizeof why, "A interp=%02X recomp=%02X", ref.cpu.A, got.cpu.A);
            else if (ref.cpu.X  != got.cpu.X)  snprintf(why, sizeof why, "X interp=%02X recomp=%02X", ref.cpu.X, got.cpu.X);
            else if (ref.cpu.Y  != got.cpu.Y)  snprintf(why, sizeof why, "Y interp=%02X recomp=%02X", ref.cpu.Y, got.cpu.Y);
            else if (ref.cpu.SP != got.cpu.SP) snprintf(why, sizeof why, "SP interp=%02X recomp=%02X", ref.cpu.SP, got.cpu.SP);
            else if (pr != pg)                 snprintf(why, sizeof why, "P interp=%02X recomp=%02X", pr, pg);
            else if (ref.cycles != got.cycles) snprintf(why, sizeof why, "cycles interp=%u recomp=%u", ref.cycles, got.cycles);
            else if (memcmp(ref.ram, got.ram, sizeof ref.ram)) {
                int i = 0; while (ref.ram[i] == got.ram[i]) i++;
                snprintf(why, sizeof why, "RAM[$%03X] interp=%02X recomp=%02X", i, ref.ram[i], got.ram[i]);
            } else if (c->control_flow) {
                int taken = !c->branch || ref.cpu.PC != (uint16_t)(c->pc + c->size);
                if (taken && ref.cpu.PC != got.cpu.PC)
                    snprintf(why, sizeof why, "PC interp=%04X recomp=%04X", ref.cpu.PC, got.cpu.PC);
            }
            if (why[0]) {
                case_failed = 1;
                if (reported++ < 200)
                    printf("MISMATCH $%02X %s @%04X: %s  (start A=%02X X=%02X Y=%02X SP=%02X P=%02X)\n",
                           c->opcode, c->name, c->pc, why,
                           start.cpu.A, start.cpu.X, start.cpu.Y, start.cpu.SP,
                           (put(&start), get_P()));
            }
        }
        fails += case_failed;
    }
    printf("cases=%d iterations=%d failed=%d%s\n", test_case_count, iters, fails,
           wrap_mode ? (wrap_seen ? " (wrap mode)" : " (wrap mode: nothing tested)") : "");
    if (wrap_mode && !wrap_seen) return 2;
    return fails ? 1 : 0;
}
