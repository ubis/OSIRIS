// SPDX-License-Identifier: BSD-2-Clause
/* The front tone controls, bass management and the equalizer */
#include <stdint.h>

#include "dsp.h"

static float tone_z[2][DSPAUDIO_TONE][2];

/*
 * The biquads in transposed direct form II, on one input sample, made
 * TONE_SHIFT smaller first: a boost on a full scale source must not clip.
 */
int32_t tone(unsigned int in, int32_t smp)
{
	const volatile struct dspaudio_biquad *q = SH->tone[SH->tone_sel & 1];
	float x = (float)smp * (1.0f / TONE_GAIN), y;
	unsigned int k;

	for (k = 0; k < DSPAUDIO_TONE; k++) {
		float *z = tone_z[in][k];

		y = q[k].b0 * x + z[0];
		z[0] = q[k].b1 * x - q[k].a1 * y + z[1];
		z[1] = q[k].b2 * x - q[k].a2 * y;
		x = y;
	}

	if (x >= 2147483520.0f) {
		return INT32_MAX;
	}

	if (x <= -2147483648.0f) {
		return INT32_MIN;
	}

	return (int32_t)x;
}

/*
 * A copy of the set dspd selected, so that the filters run from plain
 * memory. Copying it at once takes longer than a sample period: it is
 * copied a few words per frame into the spare one, then switched to.
 */
#define BASS_WORDS	(sizeof(struct dspaudio_bass) / 4)
#define BASS_STEP	16

static struct dspaudio_bass bass_buf[2];
static const struct dspaudio_bass *bm = &bass_buf[0];
static uint32_t bass_gen, copy_gen, bass_copy;	/* words of the spare done */
static float bass_z[DSPAUDIO_OUT][2][DSPAUDIO_XO][2];	/* [high, low] */
static float sub_z[DSPAUDIO_XO][2];
static float eq_z[DSPAUDIO_OUT][DSPAUDIO_EQ][2];
static float loud_z[DSPAUDIO_OUT][2];

static float cascade(const struct dspaudio_biquad *q, float (*z)[2], float x)
{
	unsigned int k;

	for (k = 0; k < DSPAUDIO_XO; k++) {
		float y = q[k].b0 * x + z[k][0];

		z[k][0] = q[k].b1 * x - q[k].a1 * y + z[k][1];
		z[k][1] = q[k].b2 * x - q[k].a2 * y;
		x = y;
	}

	return x;
}

void bass_reset(void)
{
	unsigned int i;

	for (i = 0; i < sizeof(bass_z) / sizeof(float); i++) {
		((float *)bass_z)[i] = 0.0f;
	}

	for (i = 0; i < sizeof(sub_z) / sizeof(float); i++) {
		((float *)sub_z)[i] = 0.0f;
	}

	for (i = 0; i < sizeof(eq_z) / sizeof(float); i++) {
		((float *)eq_z)[i] = 0.0f;
	}

	for (i = 0; i < sizeof(loud_z) / sizeof(float); i++) {
		((float *)loud_z)[i] = 0.0f;
	}
}

/* the same filters in the same places: their state still fits */
static int bass_same_layout(const struct dspaudio_bass *a, const struct dspaudio_bass *b)
{
	unsigned int out;

	if (a->small != b->small || a->copy != b->copy || a->subwoofer != b->subwoofer ||
	    a->loud_on != b->loud_on) {
		return 0;
	}

	for (out = 0; out < DSPAUDIO_OUT; out++) {
		if (a->eq_n[out] != b->eq_n[out]) {
			return 0;
		}
	}

	return 1;
}

void bass_follow(void)
{
	uint32_t gen = SH->bass_gen;
	struct dspaudio_bass *spare = bm == &bass_buf[0] ? &bass_buf[1] : &bass_buf[0];
	const volatile uint32_t *src = (const volatile uint32_t *)&SH->bass[gen & 1];
	uint32_t *dst = (uint32_t *)spare;
	unsigned int end;

	if (gen == bass_gen) {
		return;
	}

	/* a newer set than the one being copied: start over */
	if (gen != copy_gen) {
		copy_gen = gen;
		bass_copy = 0;
	}

	end = bass_copy + BASS_STEP < BASS_WORDS ? bass_copy + BASS_STEP : BASS_WORDS;
	for (; bass_copy < end; bass_copy++) {
		dst[bass_copy] = src[bass_copy];
	}

	if (bass_copy < BASS_WORDS) {
		return;
	}

	bass_copy = 0;
	/* dspd may have rewritten this set while it was copied */
	if (SH->bass_gen != gen) {
		return;
	}

	/* other filters: the old state does not fit them, clear it */
	if (!bass_same_layout(bm, spare)) {
		bass_reset();
	}

	bm = spare;
	bass_gen = gen;
}

static float bass_sub;

/* a frame's bass for the subwoofer starts from nothing */
void bass_begin(void)
{
	bass_sub = 0.0f;
}

/* outputs from .. to - 1 */
void bass_split(float *y, unsigned int from, unsigned int to)
{
	unsigned int out;

	for (out = from; out < to; out++) {
		if (out == DSPAUDIO_OUT_SW || !((bm->small | bm->copy) & (1u << out))) {
			continue;
		}

		bass_sub += cascade(bm->low[out], bass_z[out][1], y[out]);
		if (bm->small & (1u << out)) {
			y[out] = cascade(bm->high[out], bass_z[out][0], y[out]);
		}
	}
}

/* the loudness shelf and the equalizer of outputs from .. to - 1 */
void eq_split(float *y, unsigned int from, unsigned int to)
{
	unsigned int out, k, n;

	for (out = from; out < to; out++) {
		if (bm->loud_on) {
			const struct dspaudio_biquad *q = &bm->loud;
			float *z = loud_z[out], x = y[out], v = q->b0 * x + z[0];

			z[0] = q->b1 * x - q->a1 * v + z[1];
			z[1] = q->b2 * x - q->a2 * v;
			y[out] = v;
		}

		n = bm->eq_n[out] < DSPAUDIO_EQ ? bm->eq_n[out] : DSPAUDIO_EQ;
		for (k = 0; k < n; k++) {
			const struct dspaudio_biquad *q = &bm->eq[out][k];
			float *z = eq_z[out][k], x = y[out], v = q->b0 * x + z[0];

			z[0] = q->b1 * x - q->a1 * v + z[1];
			z[1] = q->b2 * x - q->a2 * v;
			y[out] = v;
		}
	}
}

void bass_finish(float *y)
{
	float sub = bass_sub;

	if (bm->subwoofer) {
		y[DSPAUDIO_OUT_SW] = cascade(bm->sub, sub_z, y[DSPAUDIO_OUT_SW] + sub);
	} else {
		y[DSPAUDIO_OUT_FL] += 0.5f * sub;
		y[DSPAUDIO_OUT_FR] += 0.5f * sub;
	}
}

/* level calibration: the subwoofer as it plays, through its low pass alone */
void bass_sub_only(float *y)
{
	if (bm->subwoofer) {
		y[DSPAUDIO_OUT_SW] = cascade(bm->sub, sub_z, y[DSPAUDIO_OUT_SW]);
	}
}

