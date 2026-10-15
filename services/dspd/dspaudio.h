/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The block the DSP firmware (dspaudio) and dspd share in DSP L2 RAM: the
 * DSP reports its state there, dspd sets how much of each input goes to
 * each output, the tone filters, the test noise, the output delays, the
 * surround decoder, the reverb and the bass management.
 * Both sides are 32-bit little endian; the ARM reaches it through /dev/mem.
 */
#ifndef DSPAUDIO_H
#define DSPAUDIO_H

#include <stdint.h>

#define DSPAUDIO_LOAD		0x11800000u	/* image start, DSP boot address */
#define DSPAUDIO_SHARED		0x1181F000u	/* this block; the image stays below */
#define DSPAUDIO_MAGIC		0x41505344u	/* "DSPA" once running */
#define DSPAUDIO_ABI		15

#define DSPAUDIO_IN		8	/* 4 serializers x 2 slots, as the outputs */
#define DSPAUDIO_OUT		8	/* PCM1690: 4 serializers x 2 slots */
#define DSPAUDIO_GAIN_ONE	(1 << 24)	/* gains are 8.24 fixed point */
#define DSPAUDIO_TONE		2	/* biquads on the front pair: bass, treble */
#define DSPAUDIO_NOISE_OFF	0xff
#define DSPAUDIO_DELAY_MAX	2047	/* samples at 48 kHz: 42 ms, 14.6 m */
#define DSPAUDIO_SURROUND_DELAY_MAX	2047	/* surround and back delay, samples */
#define DSPAUDIO_XO		2	/* biquads per crossover side: Linkwitz-Riley */
#define DSPAUDIO_EQ		6	/* equalizer biquads per output at most */
#define DSPAUDIO_REVERB_LINES	4
#define DSPAUDIO_REVERB_LINE_MAX	4095	/* samples per feedback line */
#define DSPAUDIO_PREDELAY_MAX	4095	/* samples before the reverb starts */
/* A/V sync: a delay line of every output in the SDRAM Linux leaves alone (mem=56M) */
#define DSPAUDIO_AV_BUF		0xC3800000u
#define DSPAUDIO_AV_LEN		131072	/* frames, 4 MiB: 800 ms at 96 kHz fits */

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

/*
 * Inputs: the PCM9211 brings the front pair of every input; multichannel
 * PCM from HDMI brings the rest on lines of its own, on the same clocks.
 */
enum dspaudio_in {
	DSPAUDIO_IN_L,		/* AXR1_4: PCM9211, front */
	DSPAUDIO_IN_R,
	DSPAUDIO_IN_C,		/* AXR1_5: HDMI receiver, centre, LFE */
	DSPAUDIO_IN_LFE,
	DSPAUDIO_IN_SL,		/* AXR1_8: surround */
	DSPAUDIO_IN_SR,
	DSPAUDIO_IN_SBL,	/* AXR1_9: surround back */
	DSPAUDIO_IN_SBR,
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

/*
 * Reverb for the sound field modes, on top of the surround decoder: the
 * mono sum, held back by the pre-delay, goes into four feedback delay lines
 * mixed by a Hadamard matrix, each with a decay gain and a low pass for the
 * damping. Taps of the lines go to the fronts, the surrounds and the back.
 */
struct dspaudio_reverb {
	uint32_t on;
	uint32_t length[DSPAUDIO_REVERB_LINES];	/* samples */
	float decay[DSPAUDIO_REVERB_LINES];	/* gain per round, from the reverb time */
	float damping;			/* one-pole low pass in the loop, 0 = none */
	uint32_t predelay;		/* samples */
	float front, surround, back;	/* how much of the reverb each pair gets */
};

/*
 * Bass management: a small speaker's output goes through a high pass, and
 * its bass, through the matching low pass, to the subwoofer, or to the
 * front pair when there is none. The subwoofer has a low pass of its own.
 * Double bass also sends the bass of large speakers to the subwoofer.
 * After it, each output's equalizer: a peaking filter per band in use, and
 * the loudness shelf on every output.
 */
struct dspaudio_bass {
	uint32_t small;			/* one bit per output: bass managed */
	uint32_t copy;			/* one bit per output: bass also to the subwoofer */
	uint32_t subwoofer;		/* 0: the bass goes to the front pair */
	struct dspaudio_biquad high[DSPAUDIO_OUT][DSPAUDIO_XO];
	struct dspaudio_biquad low[DSPAUDIO_OUT][DSPAUDIO_XO];
	struct dspaudio_biquad sub[DSPAUDIO_XO];
	uint32_t eq_n[DSPAUDIO_OUT];	/* equalizer biquads in use */
	struct dspaudio_biquad eq[DSPAUDIO_OUT][DSPAUDIO_EQ];
	uint32_t loud_on;
	struct dspaudio_biquad loud;	/* low shelf, the same on every output */
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
	struct dspaudio_reverb reverb;	/* only with the surround decoder */
	/*
	 * dspd fills bass[(bass_gen + 1) & 1], then increments bass_gen. The
	 * DSP copies the set in pieces and takes it only if bass_gen has not
	 * moved on meanwhile.
	 */
	struct dspaudio_bass bass[2];
	uint32_t bass_gen;
	uint32_t swap;			/* 1: left comes in the right slot (HDMI) */
	uint32_t inputs;		/* one bit per input the source has */
	uint32_t av_delay;		/* frames every output is held back for A/V sync */
	/* written by the DSP */
	uint32_t bitstream;		/* IEC 61937 data type while one plays, else 0 */
};

_Static_assert(sizeof(struct dspaudio_shared) <= 0x1000, "the shared block is 4 KiB");

#endif
