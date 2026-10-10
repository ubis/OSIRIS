/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Time keeping with the C64x+/C674x time stamp counter (TSCL/TSCH, CPU clock).
 */
#ifndef DSPBOOT_TIMER_H
#define DSPBOOT_TIMER_H

#include "types.h"

extern u32 cpu_hz; /* current DSP clock, updated after PLL setup */

void timer_init(void);
u64 timer_ticks(void);
/* milliseconds since timer_init (wraps after 49 days) */
u32 timer_ms(void);
void udelay(u32 us);
void mdelay(u32 ms);

#endif
