#pragma once
#include <stdint.h>

/* Open a single .fm2 file or a directory of .fm2 files.
   Returns 1 on success, 0 on failure. */
int  fm2_open(const char *path);

/* Advance one NMI frame.  Fills *c0 / *c1 with controller bytes.
   Returns 1 while frames remain, 0 when all files are exhausted. */
int  fm2_tick(uint8_t *c0, uint8_t *c1);
int  fm2_tick_cmd(uint8_t *c0, uint8_t *c1, uint8_t *cmd); /* cmd: bit0=power reset, bit1=soft reset */
int  fm2_peek_cmd(uint8_t *cmd);  /* peek next record's cmd byte without consuming */

/* True while playback is active (buf loaded and frames remain). */
int  fm2_active(void);

/* Free all resources. */
void fm2_free(void);
