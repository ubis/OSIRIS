/* SPDX-License-Identifier: BSD-2-Clause */
/* What the parts of dspaudio share */
#ifndef DSPAUDIO_DSP_H
#define DSPAUDIO_DSP_H

#include <stdint.h>

#include "dspaudio.h"

#define SH		((volatile struct dspaudio_shared *)DSPAUDIO_SHARED)
#define RING		((volatile struct dspaudio_ring *)DSPAUDIO_RING)
#define FULL_SCALE	2147483648.0f
/* the front pair after tone() is 12 dB down, as much as the controls boost */
#define TONE_SHIFT	2
#define TONE_GAIN	((float)(1 << TONE_SHIFT))

#define RX_SERS		4
#define TX_SERS		4		/* AXR1_0..3: PCM1690 */
/* inputs 2n and 2n + 1 come from rx_ser[n]; AXR1_5 has LFE as left, centre as right */
extern const uint8_t rx_ser[RX_SERS];
#define RX_FLIP		(1u << 1)

static inline float clampf(float x, float lo, float hi)
{
	return x < lo ? lo : x > hi ? hi : x;
}

/* 1 / sqrt(x) for x > 0, close enough for a gain: bit trick, two Newton steps */
static inline float rsqrt(float x)
{
	union {
		float f;
		uint32_t u;
	} v = { .f = x };
	float h = 0.5f * x;

	v.u = 0x5f3759df - (v.u >> 1);
	v.f = v.f * (1.5f - h * v.f * v.f);
	v.f = v.f * (1.5f - h * v.f * v.f);
	return v.f;
}

static inline float biquad(const volatile struct dspaudio_biquad *q, float *z, float x)
{
	float y = q->b0 * x + z[0];

	z[0] = q->b1 * x - q->a1 * y + z[1];
	z[1] = q->b2 * x - q->a2 * y;
	return y;
}

/* mcasp.c */
void psc1_enable(unsigned int lpsc);
int mcasp_init(void);
void tx_restart(void);

/* dma.c */
void dma_start(void);
void dma_loop(void) __attribute__((noreturn));

/* input.c */
extern int32_t input[DSPAUDIO_IN];
extern uint32_t decim;
unsigned int rx_follow_rate(void);
unsigned int rx_slot(unsigned int odd, const int32_t *rx);

/* bass.c */
int32_t tone(unsigned int in, int32_t smp);
void bass_follow(void);
void bass_reset(void);
void bass_begin(void);
void bass_split(float *y, unsigned int from, unsigned int to);
void eq_split(float *y, unsigned int from, unsigned int to);
void bass_finish(float *y);
void bass_sub_only(float *y);

/* surround.c */
extern float upmixed[DSPAUDIO_OUT];
void reverb_clear(void);
void upmix(int32_t li, int32_t ri);

/* dvol.c */
void dvol_apply(float *y);

/* delay.c */
void av_clear(void);
void delay_frame(const int32_t *in, int32_t *out);

/* frame.c */
extern int32_t frame[DSPAUDIO_OUT];
void frame_step1(void);
void frame_step2(void);
void frame_step3(void);
void frame_step4(void);

/* ac3.c */
#ifdef DSPAUDIO_AC3
void ac3_init(void);
void ac3_reset(void);
void ac3_word(uint32_t smp);
void ac3_work(void);
void ac3_take(void);
#else
static inline void ac3_reset(void)
{
}
#endif

#endif
