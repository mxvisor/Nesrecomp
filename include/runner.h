#ifndef RUNNER_H
#define RUNNER_H

/*
 * runner.h — главный заголовок, включает все модули
 *
 * Структура проекта:
 *
 *   cpu.h        — регистры CPU 6502, флаги, SET_NZ
 *   memory.h     — карта памяти, стек, контроллер
 *   ppu.h        — PPU 2C02: рендеринг, scroll, спрайты
 *   apu.h        — APU: pulse, triangle, noise, DMC
 *   mapper.h     — маппер картриджа: PRG/CHR банкинг, IRQ
 *   interrupts.h — NMI, IRQ, RESET, call_by_address
 *
 * Где искать баги:
 *   Графика битая        -> ppu.c / ppu.h
 *   Звук неправильный    -> apu.c / apu.h
 *   Игра вылетает        -> cpu_interp.c / cpu.h
 *   Тайлы/скролл        -> mapper.c / mapper.h
 *   Ввод не работает     -> memory.c (ctrl_read/write)
 *   NMI не срабатывает   -> runner.c + interrupts.h
 *   Загрузка ROM         -> runner.c (load_rom)
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
