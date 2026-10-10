/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef DSPBOOT_SDRAM_H
#define DSPBOOT_SDRAM_H

#include "types.h"

/* bytes, 0 until sdram_bringup() found some */
extern u32 sdram_size;

/*
 * Set up, size and quick-test the SDRAM; prints the "DRAM:" line(s). 0 =
 * usable, else -ETIMEDOUT, -ENODEV or -EIO.
 */
int sdram_bringup(void);
/*
 * Full pattern test of [start, start + len), destroys the contents. 0 or -EIO.
 */
int sdram_memtest(u32 start, u32 len);
/* [addr, addr + len) lies inside the detected SDRAM */
int sdram_contains(u32 addr, u32 len);

#endif
