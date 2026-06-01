#ifndef INTERRUPTS_H
#define INTERRUPTS_H

/*
 * interrupts.h — NES interrupt declarations
 *
 * Implemented in runner.c (set g_nmi_pending / g_irq_pending flags).
 * Called from:
 *   ppu.c        -> nes_nmi()   (VBlank at scanline 241, dot 1)
 *   mapper.c     -> nes_irq()   (MMC3 scanline counter)
 *   cpu_interp.c -> nes_irq()   (BRK instruction)
 *   runner.c     -> nes_reset() (on startup)
 *
 * Interrupts are handled in runner_run() main loop:
 * flags are checked before each CPU instruction and PC+P pushed to stack.
 */

void nes_nmi(void);    /* Non-Maskable Interrupt — VBlank */
void nes_irq(void);    /* Maskable IRQ — mapper, APU frame */
void nes_reset(void);  /* Reset — reads vector $FFFC */

/* Interrupt flags — checked in main loop and in cpu_interp_run() */
extern volatile int g_nmi_pending;
extern volatile int g_irq_pending;

/* Dispatch table from generated code (or stub fallback) */
void call_by_address(uint16_t addr);

#endif /* INTERRUPTS_H */
