// SPDX-License-Identifier: BSD-2-Clause
/* Surround from stereo: the matrix upmix, and the reverb of the sound field modes */
#include <stdint.h>

#include "dsp.h"

#define SUR_LEN		(DSPAUDIO_SURROUND_DELAY_MAX + 1)

static float sur_ll, sur_rr, sur_lr;	/* smoothed L*L, R*R, L*R */
static float sur_z[2][2];		/* the band filters' state */
static float sur_buf[SUR_LEN];
static unsigned int sur_pos;
static float sur_ap;			/* all-pass state, right surround */
float upmixed[DSPAUDIO_OUT];	/* -1..1 */

static float sur_tap(unsigned int d)
{
	if (d > DSPAUDIO_SURROUND_DELAY_MAX) {
		d = DSPAUDIO_SURROUND_DELAY_MAX;
	}

	return sur_buf[sur_pos >= d ? sur_pos - d : sur_pos + SUR_LEN - d];
}

#define REVERB_LEN	(DSPAUDIO_REVERB_LINE_MAX + 1)
#define PREDELAY_LEN	(DSPAUDIO_PREDELAY_MAX + 1)

static float rev_line[DSPAUDIO_REVERB_LINES][REVERB_LEN] __attribute__((section(".shram")));
static float rev_pre[PREDELAY_LEN] __attribute__((section(".shram")));
static unsigned int rev_pos[DSPAUDIO_REVERB_LINES], pre_pos;
static float rev_lp[DSPAUDIO_REVERB_LINES];

extern uint32_t __shram_start[], __shram_end[];

void reverb_clear(void)
{
	uint32_t *p;

	for (p = __shram_start; p < __shram_end; p++) {
		*p = 0;
	}
}

/* adds the reverb of one sample pair to upmixed[] */
static void reverb(float m)
{
	const volatile struct dspaudio_reverb *s = &SH->reverb;
	float o[DSPAUDIO_REVERB_LINES], h0, h1, h2, h3, x;
	unsigned int i, d = s->predelay, len, r;

	if (d > DSPAUDIO_PREDELAY_MAX) {
		d = DSPAUDIO_PREDELAY_MAX;
	}

	rev_pre[pre_pos] = m;
	x = rev_pre[pre_pos >= d ? pre_pos - d : pre_pos + PREDELAY_LEN - d];
	pre_pos = pre_pos + 1 == PREDELAY_LEN ? 0 : pre_pos + 1;

	/* the oldest sample of each line, damped and decayed */
	for (i = 0; i < DSPAUDIO_REVERB_LINES; i++) {
		len = s->length[i];
		if (len < 1 || len > DSPAUDIO_REVERB_LINE_MAX) {
			len = DSPAUDIO_REVERB_LINE_MAX;
		}

		r = rev_pos[i] >= len ? rev_pos[i] - len : rev_pos[i] + REVERB_LEN - len;
		rev_lp[i] += (1.0f - s->damping) * (rev_line[i][r] - rev_lp[i]);
		o[i] = rev_lp[i] * s->decay[i];
	}

	/* Hadamard 4 x 4, scaled to keep the energy */
	h0 = 0.5f * (o[0] + o[1] + o[2] + o[3]);
	h1 = 0.5f * (o[0] - o[1] + o[2] - o[3]);
	h2 = 0.5f * (o[0] + o[1] - o[2] - o[3]);
	h3 = 0.5f * (o[0] - o[1] - o[2] + o[3]);
	rev_line[0][rev_pos[0]] = h0 + x;
	rev_line[1][rev_pos[1]] = h1 + x;
	rev_line[2][rev_pos[2]] = h2 + x;
	rev_line[3][rev_pos[3]] = h3 + x;
	for (i = 0; i < DSPAUDIO_REVERB_LINES; i++) {
		rev_pos[i] = rev_pos[i] + 1 == REVERB_LEN ? 0 : rev_pos[i] + 1;
	}

	/* different lines on each side: the sides are not correlated */
	upmixed[DSPAUDIO_OUT_FL] += s->front * o[0];
	upmixed[DSPAUDIO_OUT_FR] += s->front * o[1];
	upmixed[DSPAUDIO_OUT_SL] += s->surround * o[2];
	upmixed[DSPAUDIO_OUT_SR] += s->surround * o[3];
	upmixed[DSPAUDIO_OUT_SBL] += s->back * o[1];
	upmixed[DSPAUDIO_OUT_SBR] += s->back * o[0];
}

/* the eight outputs from one stereo sample pair, struct dspaudio_surround */
void upmix(int32_t li, int32_t ri)
{
	const volatile struct dspaudio_surround *s = &SH->surround;
	float l = li * TONE_GAIN / FULL_SCALE, r = ri * TONE_GAIN / FULL_SCALE, a = s->smooth;
	float rho, c, m, diff, sl, sr, x, ap;
	unsigned int d = s->delay, bd = d + s->back_delay;

	sur_ll += a * (l * l - sur_ll);
	sur_rr += a * (r * r - sur_rr);
	sur_lr += a * (l * r - sur_lr);

	/* in-phase correlation 0..1: how much of the sound sits between the speakers */
	rho = sur_ll * sur_rr > 1e-12f ? clampf(sur_lr * rsqrt(sur_ll * sur_rr), 0.0f, 1.0f) : 0.0f;
	m = 0.5f * (l + r);
	c = s->center * rho * m;
	upmixed[DSPAUDIO_OUT_FL] = l - c;
	upmixed[DSPAUDIO_OUT_FR] = r - c;
	/* same power as the two front speakers gave it */
	upmixed[DSPAUDIO_OUT_C] = 1.41421356f * c;
	upmixed[DSPAUDIO_OUT_SW] = s->sub * m;

	/* the difference, less of it the more the sound is in phase, by steer */
	diff = 0.5f * (l - r) * s->surround * (1.0f - s->steer * rho);
	diff = biquad(&s->band[0], sur_z[0], diff);
	diff = biquad(&s->band[1], sur_z[1], diff);
	sur_buf[sur_pos] = diff;

	x = sur_tap(d);
	/* first order all-pass on the right one: the pair is not one mono source */
	ap = -0.6f * x + sur_ap;
	sur_ap = x + 0.6f * ap;
	sl = x + s->wrap * upmixed[DSPAUDIO_OUT_FL];
	sr = -ap + s->wrap * upmixed[DSPAUDIO_OUT_FR];
	upmixed[DSPAUDIO_OUT_SL] = sl;
	upmixed[DSPAUDIO_OUT_SR] = sr;

	x = sur_tap(bd);
	upmixed[DSPAUDIO_OUT_SBL] = s->back * x;
	upmixed[DSPAUDIO_OUT_SBR] = -s->back * x;
	sur_pos = sur_pos + 1 == SUR_LEN ? 0 : sur_pos + 1;
	if (SH->reverb.on) {
		reverb(m);
	}
}
