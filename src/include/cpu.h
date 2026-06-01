#ifndef CPU_H
#define CPU_H

/*
 * cpu.h — CPU 6502 state, flags, stack helpers
 *
 * Scope: CPU registers and flags only.
 * No knowledge of PPU, APU, or mapper.
 * To fix CPU behaviour edit cpu_interp.c and this file.
 */

#include <stdint.h>

typedef struct {
    uint8_t  A, X, Y, SP;
    uint16_t PC;
    /* Flags — separate bytes for speed (avoids bit-field operations) */
    uint8_t  N, V, D, I, Z, C;
} CPU;

extern CPU cpu;

/* Set N and Z flags from value */
#define SET_NZ(v) do { \
    cpu.N = ((v) >> 7) & 1; \
    cpu.Z = ((v) == 0) ? 1 : 0; \
} while(0)

/* Pack flags into P byte */
static inline uint8_t get_P(void) {
    return (cpu.N << 7) | (cpu.V << 6) | 0x20
         | (cpu.D << 3) | (cpu.I << 2) | (cpu.Z << 1) | cpu.C;
}

/* Unpack P byte into flags */
static inline void set_P(uint8_t p) {
    cpu.N = (p >> 7) & 1;
    cpu.V = (p >> 6) & 1;
    cpu.D = (p >> 3) & 1;
    cpu.I = (p >> 2) & 1;
    cpu.Z = (p >> 1) & 1;
    cpu.C =  p       & 1;
}

/* Cycle counter — accumulated CPU cycles between PPU/APU steps */
extern uint32_t g_cpu_cycles;

/*
 * Interpreter — cpu_interp.c
 * cpu_interp_step(): execute one instruction, return cycle count
 * cpu_interp_run(addr): run until RTS/RTI (dispatch fallback)
 */
int  cpu_interp_step(void);
void cpu_interp_run(uint16_t entry);

/* BRK handler (defined in memory.c — needs memory access) */
void cpu_brk(void);

#endif /* CPU_H */
