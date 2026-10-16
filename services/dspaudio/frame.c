// SPDX-License-Identifier: BSD-2-Clause
/* A frame of the eight outputs: mixing, the chain of processing, the limiter */
#include <stdint.h>

#include "dsp.h"

static uint32_t noise_x = 0x12345678;
static float noise_b[3];

/* white noise from xorshift32, through Paul Kellet's economy pink filter */
static int32_t noise(void)
{
	float w, p;

	noise_x ^= noise_x << 13;
	noise_x ^= noise_x >> 17;
	noise_x ^= noise_x << 5;
	w = (float)(noise_x >> 8) / 8388608.0f - 1.0f;
	noise_b[0] = 0.99765f * noise_b[0] + w * 0.0990460f;
	noise_b[1] = 0.96300f * noise_b[1] + w * 0.2965164f;
	noise_b[2] = 0.57000f * noise_b[2] + w * 1.0526913f;
	p = (noise_b[0] + noise_b[1] + noise_b[2] + w * 0.1848f) * SH->noise_gain;
	if (p >= 2147483520.0f) {
		return INT32_MAX;
	}

	if (p <= -2147483648.0f) {
		return INT32_MIN;
	}

	return (int32_t)p;
}

/*
 * -1..1 at full scale, with 18 dB of headroom: folded channels can add up
 * past it. The gains are read as plain memory; a torn update is one sample.
 */
static float mix(unsigned int out)
{
	const int32_t *g = (const int32_t *)SH->gain[out];
	int64_t v = 0;
	unsigned int i;

	if (out == SH->noise_out) {
		return noise() / FULL_SCALE;
	}

	if (SH->surround.on) {
		return upmixed[out] * (g[0] / (float)DSPAUDIO_GAIN_ONE);
	}

	/* unused inputs are 0 */
	for (i = 0; i < DSPAUDIO_IN; i++) {
		v += (int64_t)input[i] * g[i];
	}

	v >>= 27;
	if (v > INT32_MAX) {
		v = INT32_MAX;
	} else if (v < INT32_MIN) {
		v = INT32_MIN;
	}

	return (int32_t)v * (8.0f / FULL_SCALE);
}

#define LIMIT		0.95f		/* -0.45 dBFS */
#define LIMIT_RELEASE	0.99995f	/* per sample: ~0.4 s back to unity */

int32_t frame[DSPAUDIO_OUT];
static float peak_env[DSPAUDIO_OUT];

/*
 * The bass the small speakers send can add up past full scale: rather than
 * clip, turn the output down at once and back up slowly.
 */
static float limit(unsigned int out, float x)
{
	float a = x < 0.0f ? -x : x;

	peak_env[out] = a > peak_env[out] ? a : peak_env[out] * LIMIT_RELEASE;
	return peak_env[out] > LIMIT ? x * (LIMIT / peak_env[out]) : x;
}

/* a frame takes longer than one slot: the samples going out are the last one's */
static float frame_y[DSPAUDIO_OUT];

static int frame_noise;		/* level calibration: the noise alone, no bass split */

/*
 * A processed frame is made in four steps. At 48 kHz two run after each
 * slot; above it each slot gets one, every other frame is processed and
 * played twice, and the inputs are the mean of two frames.
 */
void frame_step1(void)
{
	unsigned int out;

	bass_follow();
	for (out = 0; out < DSPAUDIO_OUT; out++) {
		frame_y[out] = mix(out);
	}

	/* the filters held the music from before: do not let it ring out after */
	if (frame_noise && SH->noise_out == DSPAUDIO_NOISE_OFF) {
		bass_reset();
	}

	frame_noise = SH->noise_out != DSPAUDIO_NOISE_OFF;
	if (!frame_noise) {
		dvol_apply(frame_y);
	}

	bass_begin();
	if (!frame_noise) {
		bass_split(frame_y, 0, DSPAUDIO_OUT / 2);
	}
}

void frame_step2(void)
{
	/* the subwoofer's is only final after step 3 */
	if (!frame_noise) {
		eq_split(frame_y, 0, DSPAUDIO_OUT_SW);
	}
}

void frame_step3(void)
{
	float *y = frame_y;

	if (!frame_noise) {
		bass_split(y, DSPAUDIO_OUT / 2, DSPAUDIO_OUT);
		bass_finish(y);
	} else {
		bass_sub_only(y);
	}
}

/* the processed frame goes out from the next slot 0 on */
void frame_step4(void)
{
	int32_t v[DSPAUDIO_OUT];
	float *y = frame_y;
	unsigned int out;

	if (!frame_noise) {
		eq_split(y, DSPAUDIO_OUT_SW, DSPAUDIO_OUT);
	}

	for (out = 0; out < DSPAUDIO_OUT; out++) {
		y[out] = limit(out, y[out]);
		v[out] = (int32_t)(clampf(y[out], -1.0f, 0.99999994f) * FULL_SCALE);
	}

	delay_frame(v, frame);
}
