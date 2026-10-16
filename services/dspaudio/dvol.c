// SPDX-License-Identifier: BSD-2-Clause
/* Dynamic Volume: a slow gain on every output that evens out the level */
#include <stdint.h>

#include "dsp.h"

/* log2 and 2^x close enough for a gain in dB: bit tricks, no libm */
static float fast_log2(float x)
{
	union {
		float f;
		uint32_t u;
	} v = { .f = x };
	/* the polynomial gives log2(m) + 1 for m in [1, 2) */
	float e = (float)((int)((v.u >> 23) & 0xff) - 128), m;

	v.u = (v.u & 0x007fffff) | 0x3f800000;
	m = v.f;
	return e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;
}

static float fast_exp2(float x)
{
	union {
		float f;
		uint32_t u;
	} v;
	int i = (int)(x < 0.0f ? x - 1.0f : x);
	float f = x - (float)i;

	v.u = (uint32_t)(i + 127) << 23;
	return v.f * (1.0f + f * (0.6565f + f * 0.3435f));
}

#define DB_PER_LOG2	6.0206f

static float dvol_env, dvol_avg, dvol_db;	/* mean square, dB, gain in dB */
static unsigned int dvol_heard;		/* frames above the gate since on, up to DVOL_SETTLE */
static unsigned int dvol_quiet;		/* frames below the gate in a row */
#define DVOL_SETTLE	48000		/* the RMS meter has risen, ~1 s */
#define DVOL_FADE	0.0002f		/* back to 0 dB when switched off, ~100 ms */

/* the Dynamic Volume gain on every output */
void dvol_apply(float *y)
{
	const volatile struct dspaudio_dvol *s = &SH->dvol;
	float p = y[DSPAUDIO_OUT_FL] * y[DSPAUDIO_OUT_FL] + y[DSPAUDIO_OUT_FR] * y[DSPAUDIO_OUT_FR] +
		  y[DSPAUDIO_OUT_C] * y[DSPAUDIO_OUT_C], level, want, g;
	unsigned int out;

	if (!s->on) {
		dvol_heard = 0;
		if (dvol_db > -0.01f && dvol_db < 0.01f) {
			dvol_db = 0.0f;
			SH->dvol_gain = 0.0f;
			return;
		}

		dvol_db -= DVOL_FADE * dvol_db;
		goto apply;
	}

	dvol_env += s->rms * (p - dvol_env);
	level = dvol_env > 1e-12f ? 0.5f * DB_PER_LOG2 * fast_log2(dvol_env) : -120.0f;
	/* after a second of silence, as between inputs, the average starts over */
	if (level <= s->gate && ++dvol_quiet > DVOL_SETTLE) {
		dvol_heard = 0;
	}

	if (level > s->gate) {
		dvol_quiet = 0;
		/* start from the level playing, once the meter has risen */
		if (dvol_heard < DVOL_SETTLE) {
			dvol_heard++;
			dvol_avg = level;
		}

		dvol_avg += s->average * (level - dvol_avg);
		want = clampf(s->strength * (dvol_avg - level), -s->cut_max, s->boost_max);
		dvol_db += (want < dvol_db ? s->attack : s->release) * (want - dvol_db);
	}

apply:
	SH->dvol_gain = dvol_db;
	g = fast_exp2(dvol_db / DB_PER_LOG2);
	for (out = 0; out < DSPAUDIO_OUT; out++) {
		y[out] *= g;
	}
}
