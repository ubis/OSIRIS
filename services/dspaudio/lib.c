// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The libc and libm functions liba52 calls; dspaudio has no libc. malloc
 * takes from a heap in SDRAM next to the decoder, and sin, cos and sqrt
 * only fill its IMDCT tables once. Its CPU acceleration hooks do nothing.
 */
#include <stddef.h>
#include <stdint.h>

#include "dspaudio.h"

uint32_t a52_detect_accel(uint32_t accel);
void a52_cpu_state_init(uint32_t accel);
void *malloc(size_t n);
void free(void *p);
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
double sqrt(double x);
double sin(double x);
double cos(double x);

uint32_t a52_detect_accel(uint32_t accel)
{
	(void)accel;
	return 0;
}

void a52_cpu_state_init(uint32_t accel)
{
	(void)accel;
}

#ifdef __TMS320C6X__
/* first in SDRAM: the DSP decodes only when this is there */
const uint32_t a52_ddr_magic __attribute__((section(".ddr_magic"), used)) = DSPAUDIO_DDR_MAGIC;

#define HEAP_SIZE	(64 * 1024)	/* a52_init: the state and 12 x 256 samples */

static uint8_t heap[HEAP_SIZE] __attribute__((aligned(16), section(".ddr_bss.heap")));
static size_t heap_used;

void *malloc(size_t n)
{
	void *p;

	n = (n + 15) & ~(size_t)15;
	if (n > HEAP_SIZE - heap_used) {
		return NULL;
	}

	p = heap + heap_used;
	heap_used += n;
	return memset(p, 0, n);
}

void free(void *p)
{
	(void)p;
}

void *memset(void *d, int c, size_t n)
{
	uint8_t *p = d;

	while (n--) {
		*p++ = (uint8_t)c;
	}
	return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
	uint8_t *p = d;
	const uint8_t *q = s;

	while (n--) {
		*p++ = *q++;
	}
	return d;
}

double sqrt(double x)
{
	double r = x > 1.0 ? x : 1.0;
	int i;

	for (i = 0; i < 64 && x > 0.0; i++) {
		r = 0.5 * (r + x / r);
	}
	return x > 0.0 ? r : 0.0;
}

/* the tables only need arguments up to about pi */
double sin(double x)
{
	double t = x, s = x;
	int i;

	for (i = 1; i < 24; i++) {
		t *= -x * x / ((2.0 * i) * (2.0 * i + 1.0));
		s += t;
	}
	return s;
}

double cos(double x)
{
	double t = 1.0, s = 1.0;
	int i;

	for (i = 1; i < 24; i++) {
		t *= -x * x / ((2.0 * i - 1.0) * (2.0 * i));
		s += t;
	}
	return s;
}
#endif
