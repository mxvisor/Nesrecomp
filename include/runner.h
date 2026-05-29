#ifndef RUNNER_H
#define RUNNER_H

/*
 * runner.h — master header, includes all modules
 *
 * Project layout:
 *
 *   cpu.h        — 6502 CPU registers, flags, SET_NZ
 *   memory.h     — memory map, stack, controller
 *   ppu.h        — PPU 2C02: rendering, scroll, sprites
 *   apu.h        — APU: pulse, triangle, noise, DMC
 *   mapper.h     — cartridge mapper: PRG/CHR banking, IRQ
 *   interrupts.h — NMI, IRQ, RESET, call_by_address
 *
 * Where to look for bugs:
 *   Corrupted graphics   -> ppu.c / ppu.h
 *   Wrong audio          -> apu.c / apu.h
 *   Game crashes         -> cpu_interp.c / cpu.h
 *   Wrong tiles/scroll   -> mapper.c / mapper.h
 *   Input not working    -> memory.c (ctrl_read/write)
 *   NMI not firing       -> runner.c + interrupts.h
 *   ROM loading          -> runner.c (load_rom)
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cpu.h"
#include "memory.h"
#include "ppu.h"
#include "apu.h"
#include "mapper.h"
#include "interrupts.h"

/* Dispatch miss learning */
void runner_miss(uint16_t addr);

/* Runner lifecycle */
int  runner_init(const char *title, const char *rom_path);
void runner_run(void);
void runner_quit(void);

#endif /* RUNNER_H */
