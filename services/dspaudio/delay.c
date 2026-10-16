// SPDX-License-Identifier: BSD-2-Clause
/* The outputs held back: speaker distances, and A/V sync in SDRAM */
#include <stdint.h>

#include "dsp.h"
#include "regs.h"

#define DELAY_LEN	(DSPAUDIO_DELAY_MAX + 1)

static int32_t delay_buf[DSPAUDIO_OUT][DELAY_LEN];
static unsigned int delay_pos[DSPAUDIO_OUT];

/*
 * The A/V sync delay line, in SDRAM, cached (MAR195 covers 0xC3000000 to
 * 0xC3FFFFFF). L1D does not keep writes: the top 64 KiB of L2, which the
 * image (below 0x11830000) leaves alone, become cache, so writes land
 * there and go out to SDRAM a line at a time.
 */
#define AV_MASK		(DSPAUDIO_AV_LEN - 1)

static int32_t (*const av_buf)[DSPAUDIO_OUT] = (int32_t (*)[DSPAUDIO_OUT])DSPAUDIO_AV_BUF;
static unsigned int av_pos;

void av_clear(void)
{
	unsigned int i, out;

	REG(L2CFG) = (REG(L2CFG) & ~7u) | L2CFG_64K;
	(void)REG(L2CFG);
	REG(MAR(DSPAUDIO_AV_BUF >> 24)) = 1;
	for (i = 0; i < DSPAUDIO_AV_LEN; i++) {
		for (out = 0; out < DSPAUDIO_OUT; out++) {
			av_buf[i][out] = 0;
		}
	}
}

static int32_t delayed(unsigned int out, int32_t v)
{
	unsigned int w = delay_pos[out], d = SH->delay[out], r;

	if (d > DSPAUDIO_DELAY_MAX) {
		d = DSPAUDIO_DELAY_MAX;
	}

	delay_buf[out][w] = v;
	delay_pos[out] = w + 1 == DELAY_LEN ? 0 : w + 1;
	r = w >= d ? w - d : w + DELAY_LEN - d;
	return delay_buf[out][r];
}

/* every output through its speaker's delay, then the A/V sync line */
void delay_frame(const int32_t *in, int32_t *out)
{
	uint32_t d = SH->av_delay, r;
	unsigned int o;

	r = (av_pos - (d < DSPAUDIO_AV_LEN ? d : AV_MASK)) & AV_MASK;
	for (o = 0; o < DSPAUDIO_OUT; o++) {
		av_buf[av_pos][o] = delayed(o, in[o]);
		out[o] = av_buf[r][o];
	}

	av_pos = (av_pos + 1) & AV_MASK;
}

