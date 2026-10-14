/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The block the DSP firmware (dspaudio) and dspd share in DSP L2 RAM: the
 * DSP reports its state there, dspd sets how much of each input goes to
 * each output, the tone filters, the test noise, the output delays and the
 * surround decoder.
 * Both sides are 32-bit little endian; the ARM reaches it through /dev/mem.
 */
#ifndef DSPAUDIO_H
#define DSPAUDIO_H

#include <stdint.h>

#define DSPAUDIO_LOAD		0x11800000u	/* image start, DSP boot address */
#define DSPAUDIO_SHARED		0x1181F000u	/* this block; the image stays below */
#define DSPAUDIO_MAGIC		0x41505344u	/* "DSPA" once running */
#define DSPAUDIO_ABI		6

#define DSPAUDIO_IN		2	/* PCM9211 I2S: left, right */
#define DSPAUDIO_OUT		8	/* PCM1690: 4 serializers x 2 slots */
#define DSPAUDIO_GAIN_ONE	(1 << 24)	/* gains are 8.24 fixed point */
#define DSPAUDIO_TONE		2	/* biquads on every input: bass, treble */
#define DSPAUDIO_NOISE_OFF	0xff
#define DSPAUDIO_DELAY_MAX	2047	/* samples at 48 kHz: 42 ms, 14.6 m */
#define DSPAUDIO_SURROUND_DELAY_MAX	2047	/* surround and back delay, samples */

/*
 * Outputs: serializer AXR1_n slot s is output 2n + s, and AXR1_n feeds
 * PCM1690 DIN4 - n (schematic). Which slot of a pair is which speaker is
 * the usual left/right order.
 */
enum dspaudio_out {
	DSPAUDIO_OUT_FL,	/* AXR1_0 -> DIN4: front */
	DSPAUDIO_OUT_FR,
	DSPAUDIO_OUT_C,		/* AXR1_1 -> DIN3: centre, subwoofer */
	DSPAUDIO_OUT_SW,
	DSPAUDIO_OUT_SL,	/* AXR1_2 -> DIN2: surround */
	DSPAUDIO_OUT_SR,
	DSPAUDIO_OUT_SBL,	/* AXR1_3 -> DIN1: surround back */
	DSPAUDIO_OUT_SBR,
};

enum dspaudio_in {
	DSPAUDIO_IN_L,
	DSPAUDIO_IN_R,
};

enum dspaudio_state {
	DSPAUDIO_STOPPED	= 0,
	DSPAUDIO_POWER		= 1,	/* enabling McASP1 */
	DSPAUDIO_RUNNING	= 3,
	DSPAUDIO_INIT		= 10,	/* 10..15: McASP init steps */
	DSPAUDIO_STUCK		= 100,	/* + the init step that timed out */
};

/* y = b0 x + b1 x[-1] + b2 x[-2] - a1 y[-1] - a2 y[-2] */
struct dspaudio_biquad {
	float b0, b1, b2, a1, a2;
};

/*
 * Surround from stereo: correlated, in-phase sound goes to the centre and is
 * taken out of the front pair, the difference (L - R) goes to the surrounds,
 * band-limited and held back, and the surround back pair repeats the
 * surrounds later. The amounts are what make movie, music and game.
 */
struct dspaudio_surround {
	uint32_t on;		/* 0: outputs from the gain matrix */
	float smooth;		/* one-pole coefficient of the correlation estimate */
	float center;		/* share of the in-phase sound moved to the centre */
	float surround;		/* gain of the difference on the surrounds */
	float wrap;		/* share of the front pair added to the surrounds */
	float back;		/* gain of the surrounds on the surround back pair */
	float sub;		/* gain of (L + R) / 2 on the subwoofer */
	uint32_t delay;		/* samples the surrounds are held back */
	uint32_t back_delay;	/* and the surround back pair after them */
	struct dspaudio_biquad band[2];	/* on the difference: high pass, low pass */
};

struct dspaudio_shared {
	/* written by the DSP */
	uint32_t magic;
	uint32_t abi;
	char version[16];
	uint32_t state;
	uint32_t samples;		/* input samples moved */
	uint32_t underruns;		/* transmit restarts */
	uint32_t overruns;		/* receive overruns */
	uint32_t peak[DSPAUDIO_IN];	/* largest |input| since dspd cleared it */
	/* written by dspd */
	int32_t gain[DSPAUDIO_OUT][DSPAUDIO_IN];	/* output = sum of input x gain */
	/* dspd fills the set the DSP does not use, then flips tone_sel */
	struct dspaudio_biquad tone[2][DSPAUDIO_TONE];
	uint32_t tone_sel;
	uint32_t noise_out;		/* output playing pink noise, or NOISE_OFF */
	float noise_gain;		/* sample value for an RMS of 1 */
	uint32_t delay[DSPAUDIO_OUT];	/* samples each output is held back */
	struct dspaudio_surround surround;	/* gain[out][0] scales each output */
};

#endif
