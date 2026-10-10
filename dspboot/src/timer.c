/* SPDX-License-Identifier: BSD-2-Clause */
#include "timer.h"
#include "board.h"

u32 cpu_hz = BOARD_OSC_HZ; /* PLL bypass after reset: DSP runs from OSCIN */

static u32 last_lo;
static u32 accum_ms_ticks; /* ticks not yet folded into ms_base */
static u32 ms_base;

void timer_init(void)
{
	u32 zero = 0;

	/* Any write to TSCL starts the free running counter. */
	__asm__ volatile("mvc .s2 %0, TSCL" : : "b"(zero));
	last_lo = 0;
	accum_ms_ticks = 0;
	ms_base = 0;
}

u64 timer_ticks(void)
{
	u32 lo, hi;

	/* Reading TSCL latches TSCH. */
	__asm__ volatile("mvc .s2 TSCL, %0" : "=b"(lo));
	__asm__ volatile("mvc .s2 TSCH, %0" : "=b"(hi));
	return ((u64)hi << 32) | lo;
}

/*
 * Millisecond clock that stays monotonic across cpu_hz changes: elapsed ticks
 * are converted with the clock rate in effect when we last looked. Uses only
 * 32-bit arithmetic (no libgcc 64-bit division); callers poll it far more
 * often than the 10.7 s TSCL wrap at 400 MHz.
 */
u32 timer_ms(void)
{
	u32 lo = (u32)timer_ticks();
	u32 per_ms = cpu_hz / 1000;

	accum_ms_ticks += lo - last_lo;
	last_lo = lo;
	if (accum_ms_ticks >= per_ms) {
		u32 n = accum_ms_ticks / per_ms;
		ms_base += n;
		accum_ms_ticks -= n * per_ms;
	}

	return ms_base;
}

void udelay(u32 us)
{
	u32 start = (u32)timer_ticks();
	u32 per_us = cpu_hz / 1000000;
	u32 total;

	if (per_us == 0) {
		per_us = 1;
	}

	/* Split long waits so that us * per_us cannot overflow 32 bits. */
	while (us > 1000000) {
		mdelay(1000);
		us -= 1000000;
	}

	total = us * per_us;
	while ((u32)timer_ticks() - start < total)
		;
}

void mdelay(u32 ms)
{
	while (ms--) {
		udelay(1000);
	}
}
