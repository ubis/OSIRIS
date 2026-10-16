// SPDX-License-Identifier: BSD-2-Clause
/*
 * 88.2 and 96 kHz: the DSP processes at half the rate. A half-band FIR
 * takes the inputs down and the outputs back up: 47 taps, Kaiser window
 * (beta 6), flat within 0.01 dB to 20 kHz and 62 dB down from 28 kHz at
 * 96 kHz. Every other tap is 0 and the centre is 1/2; hb[j] is h[2j] and
 * h[46 - 2j].
 */
#include <stdint.h>

#include "dsp.h"

#define HB_TAPS		47
#define HB_PAIRS	12
#define HB_UP		(HB_PAIRS * 2)	/* frames an output phase reaches back */

static const float hb[HB_PAIRS] = {
	-2.058404453e-04f, 7.124100060e-04f, -1.664347988e-03f, 3.262062586e-03f,
	-5.757889586e-03f, 9.484152400e-03f, -1.492488258e-02f, 2.290114812e-02f,
	-3.509569751e-02f, 5.585914810e-02f, -1.012575569e-01f, 3.166664892e-01f,
};

/*
 * Each history twice, newest first: x[pos] is always a whole window, a row
 * per frame, so a tap works on all channels at once. In the upper L2,
 * which hb_reset() clears before use.
 */
#define L2_HI		__attribute__((section(".l2_hi"), aligned(8)))
#define TEXT_HI		__attribute__((section(".text_hi")))
static float down_x[2 * HB_TAPS][DSPAUDIO_IN] L2_HI;
static float up_x[2 * HB_UP][DSPAUDIO_OUT] L2_HI;
int32_t hb_out[2][DSPAUDIO_OUT] L2_HI;
static unsigned int down_pos, up_pos;
static unsigned int up_quiet;		/* frames outputs 2..7 have been 0 */

static int32_t to_int(float v)
{
	if (v >= 2147483520.0f) {
		return INT32_MAX;
	}

	if (v <= -2147483648.0f) {
		return INT32_MIN;
	}

	return (int32_t)v;
}

void hb_reset(void)
{
	unsigned int c, i;

	for (i = 0; i < 2 * HB_TAPS; i++) {
		for (c = 0; c < DSPAUDIO_IN; c++) {
			down_x[i][c] = 0.0f;
		}
	}

	for (i = 0; i < 2 * HB_UP; i++) {
		for (c = 0; c < DSPAUDIO_OUT; c++) {
			up_x[i][c] = 0.0f;
		}
	}

	for (c = 0; c < DSPAUDIO_OUT; c++) {
		hb_out[0][c] = hb_out[1][c] = 0;
	}

	down_pos = up_pos = up_quiet = 0;
}

/* one frame at the full rate: the next window, then each input's sample */
void hb_down_step(void)
{
	down_pos = down_pos ? down_pos - 1 : HB_TAPS - 1;
}

void hb_down_put(unsigned int in, int32_t smp)
{
	down_x[down_pos][in] = down_x[down_pos + HB_TAPS][in] = (float)smp;
}

/*
 * s[c] += h * (a[c] + b[c]) on all eight channels, the sums in registers:
 * GCC keeps an array of them on the stack and waits on every step.
 */
#define HB_TAP8(h, a, b) do {				\
		s0 += (h) * ((a)[0] + (b)[0]);		\
		s1 += (h) * ((a)[1] + (b)[1]);		\
		s2 += (h) * ((a)[2] + (b)[2]);		\
		s3 += (h) * ((a)[3] + (b)[3]);		\
		s4 += (h) * ((a)[4] + (b)[4]);		\
		s5 += (h) * ((a)[5] + (b)[5]);		\
		s6 += (h) * ((a)[6] + (b)[6]);		\
		s7 += (h) * ((a)[7] + (b)[7]);		\
	} while (0)

/* and on the front pair alone, for the stereo case */
#define HB_TAP2(h, a, b) do {				\
		s0 += (h) * ((a)[0] + (b)[0]);		\
		s1 += (h) * ((a)[1] + (b)[1]);		\
	} while (0)

_Static_assert(DSPAUDIO_IN == 8 && DSPAUDIO_OUT == 8, "HB_TAP8 is for eight channels");

/* the inputs at half the rate, from the frames put so far; only the front pair without more */
TEXT_HI void hb_down(int32_t *y, unsigned int more)
{
	const float (*x)[DSPAUDIO_IN] = &down_x[down_pos];
	const float *m = x[HB_TAPS / 2];
	float s0 = 0.5f * m[0], s1 = 0.5f * m[1], s2 = 0.5f * m[2], s3 = 0.5f * m[3];
	float s4 = 0.5f * m[4], s5 = 0.5f * m[5], s6 = 0.5f * m[6], s7 = 0.5f * m[7];
	unsigned int j;

	if (!more) {
		for (j = 0; j < HB_PAIRS; j++) {
			HB_TAP2(hb[j], x[2 * j], x[HB_TAPS - 1 - 2 * j]);
		}

		y[0] = to_int(s0);
		y[1] = to_int(s1);
		return;
	}

#pragma GCC unroll 2
	for (j = 0; j < HB_PAIRS; j++) {
		HB_TAP8(hb[j], x[2 * j], x[HB_TAPS - 1 - 2 * j]);
	}

	y[0] = to_int(s0);
	y[1] = to_int(s1);
	y[2] = to_int(s2);
	y[3] = to_int(s3);
	y[4] = to_int(s4);
	y[5] = to_int(s5);
	y[6] = to_int(s6);
	y[7] = to_int(s7);
}

/*
 * A processed frame in, two frames at the full rate out: hb_out[0] lies
 * halfway between the frames HB_PAIRS and HB_PAIRS - 1 back, hb_out[1] is
 * the latter.
 */
TEXT_HI void hb_up(const int32_t *frame)
{
	float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
	float s4 = 0.0f, s5 = 0.0f, s6 = 0.0f, s7 = 0.0f;
	const float (*x)[DSPAUDIO_OUT];
	unsigned int c, j;

	up_pos = up_pos ? up_pos - 1 : HB_UP - 1;
	for (c = 0; c < DSPAUDIO_OUT; c++) {
		up_x[up_pos][c] = up_x[up_pos + HB_UP][c] = (float)frame[c];
	}

	x = &up_x[up_pos];
	up_quiet = frame[2] | frame[3] | frame[4] | frame[5] | frame[6] | frame[7] ? 0 :
		   up_quiet < HB_UP ? up_quiet + 1 : HB_UP;
	if (up_quiet == HB_UP) {
		/* the rest has been silent for a whole window: it stays 0 */
		for (j = 0; j < HB_PAIRS; j++) {
			HB_TAP2(2.0f * hb[j], x[j], x[HB_UP - 1 - j]);
		}

		hb_out[0][0] = to_int(s0);
		hb_out[0][1] = to_int(s1);
		hb_out[1][0] = to_int(x[HB_PAIRS - 1][0]);
		hb_out[1][1] = to_int(x[HB_PAIRS - 1][1]);
		for (c = 2; c < DSPAUDIO_OUT; c++) {
			hb_out[0][c] = hb_out[1][c] = 0;
		}
		return;
	}

#pragma GCC unroll 2
	for (j = 0; j < HB_PAIRS; j++) {
		HB_TAP8(2.0f * hb[j], x[j], x[HB_UP - 1 - j]);
	}

	hb_out[0][0] = to_int(s0);
	hb_out[0][1] = to_int(s1);
	hb_out[0][2] = to_int(s2);
	hb_out[0][3] = to_int(s3);
	hb_out[0][4] = to_int(s4);
	hb_out[0][5] = to_int(s5);
	hb_out[0][6] = to_int(s6);
	hb_out[0][7] = to_int(s7);
	for (c = 0; c < DSPAUDIO_OUT; c++) {
		hb_out[1][c] = to_int(x[HB_PAIRS - 1][c]);
	}
}
