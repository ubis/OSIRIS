// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Dolby Digital: the bursts in the front pair (iec61937.c) are AC-3 frames
 * of 1536 samples, six blocks of 256. Between DMA blocks liba52 decodes one
 * block into a buffer that plays, in place of the muted inputs, once the
 * one before is used up: a block lasts two DMA blocks, a frame twelve, as
 * long as it lasts. liba52 reads a frame as it decodes it: the next one is
 * collected into the other buffer. The buffers are in SDRAM with liba52.
 */
#include <stdint.h>

#include "dsp.h"
#include "iec61937.h"
#include "libs/liba52/a52.h"

#define AC3_FRAME_MAX	3840		/* bytes: 640 kbit/s at 32 kHz */
#define A52_LFE_FLAG	0x10
#define AC3_BAD_MAX	3		/* frames in a row before the layout goes */

extern char __ddr_bss_start[], __ddr_bss_end[];

static struct iec61937 iec;
#define DDR_BSS		__attribute__((section(".ddr_bss.ac3"), aligned(8)))

static uint8_t ac3_frame[2][AC3_FRAME_MAX] DDR_BSS;
static unsigned int ac3_len[2];		/* bytes the burst brought */
static unsigned int ac3_next = 2;	/* collected and waiting, 2: none */
static unsigned int ac3_cur;		/* being decoded */
static int ac3_blk = -1;		/* its next block, -1: no frame */
static int ac3_flags;
static unsigned int ac3_bad;		/* frames in a row the decoder could not take */
static a52_state_t *a52;
static sample_t *a52_out;
static float ac3_pcm[2][256][DSPAUDIO_IN] DDR_BSS;	/* decoded: played, being made */
static unsigned int ac3_play, ac3_pos, ac3_have[2], ac3_made;

/*
 * liba52's channels for each layout, LFE first when there is one, and the
 * input each goes to; mono feeds the front pair, a single rear channel both
 * surrounds.
 */
static const int8_t ac3_map[8][5] = {
	[A52_CHANNEL] = { DSPAUDIO_IN_L, DSPAUDIO_IN_R, -1 },
	[A52_MONO] = { DSPAUDIO_IN_L, -1 },
	[A52_STEREO] = { DSPAUDIO_IN_L, DSPAUDIO_IN_R, -1 },
	[A52_3F] = { DSPAUDIO_IN_L, DSPAUDIO_IN_C, DSPAUDIO_IN_R, -1 },
	[A52_2F1R] = { DSPAUDIO_IN_L, DSPAUDIO_IN_R, DSPAUDIO_IN_SL, -1 },
	[A52_3F1R] = { DSPAUDIO_IN_L, DSPAUDIO_IN_C, DSPAUDIO_IN_R, DSPAUDIO_IN_SL, -1 },
	[A52_2F2R] = { DSPAUDIO_IN_L, DSPAUDIO_IN_R, DSPAUDIO_IN_SL, DSPAUDIO_IN_SR, -1 },
	[A52_3F2R] = { DSPAUDIO_IN_L, DSPAUDIO_IN_C, DSPAUDIO_IN_R, DSPAUDIO_IN_SL, DSPAUDIO_IN_SR },
};

void ac3_init(void)
{
	char *p;

	/* no decoder without its own .ddr part beside the image */
	if (*(volatile uint32_t *)DSPAUDIO_DDR != DSPAUDIO_DDR_MAGIC) {
		return;
	}

	for (p = __ddr_bss_start; p < __ddr_bss_end; p++) {
		*p = 0;
	}

	iec61937_init(&iec);
	a52 = a52_init();
	a52_out = a52 ? a52_samples(a52) : 0;
}

/* Late Night: the stream's own range compression, by half its dB or all */
static level_t ac3_drc(level_t range, void *data)
{
	(void)data;
	return SH->late_night == 1 ? range * rsqrt(range) : range;
}

/* a bad frame now and then keeps the layout: the policy must not flap */
static void ac3_fail(void)
{
	if (++ac3_bad >= AC3_BAD_MAX) {
		SH->bitstream &= 0xff;
	}
}

/* no frame half decoded or waiting: a stream that comes back starts clean */
void ac3_reset(void)
{
	ac3_next = 2;
	ac3_blk = -1;
	ac3_made = 0;
	ac3_pos = 0;
	ac3_have[0] = ac3_have[1] = 0;
	ac3_bad = 0;
	iec61937_init(&iec);
}

/* a burst word of the front pair, in the order they came */
void ac3_word(uint32_t smp)
{
	unsigned int i, to;

	if (iec61937_word(&iec, smp >> 16) != IEC_AC3 || iec.have > AC3_FRAME_MAX) {
		return;
	}

	/* while the decoder is on a frame the next waits in the other buffer */
	to = ac3_blk >= 0 ? ac3_cur ^ 1 : 0;

	for (i = 0; i < iec.have; i++) {
		ac3_frame[to][i] = iec.frame[i];
	}
	ac3_len[to] = iec.have;
	ac3_next = to;
}

/* between DMA blocks: the next block decoded, swapped in when one ends */
void ac3_work(void)
{
	unsigned int make = ac3_play ^ 1, i, ch, n, mode;

	if (ac3_pos >= ac3_have[ac3_play] && ac3_made) {
		ac3_play = make;
		make ^= 1;
		ac3_pos = 0;
		ac3_made = 0;
	}

	if (ac3_made) {
		return;
	}
	ac3_made = 1;
	ac3_have[make] = 0;
	int rate, bitrate, len;
	level_t level = 1;
	const int8_t *map;

	if (!a52) {
		return;
	}

	if (ac3_blk < 0 && ac3_next != 2) {
		ac3_cur = ac3_next;
		ac3_next = 2;
		len = a52_syncinfo(ac3_frame[ac3_cur], &ac3_flags, &rate, &bitrate);
		/* a frame longer than its burst would be read past what came */
		if (!len || (unsigned int)len > ac3_len[ac3_cur] ||
		    (ac3_flags = A52_3F2R | A52_LFE | A52_ADJUST_LEVEL,
		     a52_frame(a52, ac3_frame[ac3_cur], &ac3_flags, &level, 0))) {
			ac3_fail();
			return;
		}

		ac3_bad = 0;

		a52_dynrng(a52, SH->late_night ? ac3_drc : 0, 0);
		ac3_blk = 0;
		SH->bitstream = (SH->bitstream & 0xff) |
				(uint32_t)(ac3_flags & (A52_CHANNEL_MASK | A52_LFE_FLAG)) << 8;
	}

	if (ac3_blk < 0) {
		return;
	}

	if (a52_block(a52)) {
		ac3_fail();
		ac3_blk = -1;
		return;
	}

	mode = ac3_flags & A52_CHANNEL_MASK;
	map = ac3_map[mode & 7];
	for (i = 0; i < 256; i++) {
		float *o = ac3_pcm[make][i];
		const sample_t *x = a52_out + i;

		for (ch = 0; ch < DSPAUDIO_IN; ch++) {
			o[ch] = 0.0f;
		}

		if (ac3_flags & A52_LFE) {
			o[DSPAUDIO_IN_LFE] = *x;
			x += 256;
		}

		for (n = 0; n < 5 && map[n] >= 0; n++, x += 256) {
			o[map[n]] = *x;
		}

		/* mono: both fronts, 3 dB down */
		if (mode == A52_MONO) {
			o[DSPAUDIO_IN_L] *= 0.70710678f;
			o[DSPAUDIO_IN_R] = o[DSPAUDIO_IN_L];
		}

		/* one rear channel: both surrounds, 3 dB down */
		if (mode == A52_2F1R || mode == A52_3F1R) {
			o[DSPAUDIO_IN_SL] *= 0.70710678f;
			o[DSPAUDIO_IN_SR] = o[DSPAUDIO_IN_SL];
		}
	}

	if (++ac3_blk == 6) {
		ac3_blk = -1;
	}
	ac3_have[make] = 256;
}

/* a decoded frame into the inputs, in place of the muted bitstream */
void ac3_take(void)
{
	const float *x;
	unsigned int in;

	if (ac3_pos >= ac3_have[ac3_play]) {
		return;
	}

	x = ac3_pcm[ac3_play][ac3_pos++];
	for (in = 0; in < DSPAUDIO_IN; in++) {
		float v = x[in] * FULL_SCALE;

		input[in] = v >= 2147483520.0f ? INT32_MAX : v <= -2147483648.0f ? INT32_MIN : (int32_t)v;
	}
	input[0] = tone(0, input[0]);
	input[1] = tone(1, input[1]);
}
