// SPDX-License-Identifier: BSD-2-Clause
/* The inputs: a receive slot at a time, the NET ring, IEC 61937 bursts */
#include <stdint.h>

#include "dsp.h"
#include "iec61937.h"

int32_t input[DSPAUDIO_IN];

/* the next frame from Linux, silence while it has none */
static void ring_take(int32_t *l, int32_t *r)
{
	uint32_t rd = RING->read;

	if (rd == RING->write || rd >= DSPAUDIO_RING_LEN) {
		*l = *r = 0;
		return;
	}

	*l = RING->frame[rd][0];
	*r = RING->frame[rd][1];
	RING->read = rd + 1 == DSPAUDIO_RING_LEN ? 0 : rd + 1;
}

/*
 * Dolby and DTS come wrapped in IEC 61937: each burst starts with the 16-bit
 * words F872 4E1F, then one with the data type. Played as PCM they are
 * full scale noise, so the inputs stay silent while bursts keep coming. The longest gap between them is 6144 frames (E-AC-3).
 */
#define BURST_PA	0xF872u
#define BURST_PB	0x4E1Fu
#define BURST_HOLD	16384		/* frames, ~340 ms at 48 kHz */

static uint32_t burst_hold, burst_word[3];	/* the last words of the front pair */

/* the data type, but not NULL and pause bursts, which come around a stream */
static void burst_type(uint32_t pc)
{
	pc &= 0x1f;
	/* a new type: the decoder starts clean and sets the layout again */
	if (pc != 0 && pc != 3 && pc != (SH->bitstream & 0x1f)) {
		SH->bitstream = pc;
		ac3_reset();
	}
}

/* on each frame of the front pair, in the order the samples came */
static void burst_find(uint32_t first, uint32_t second)
{
	uint32_t *w = burst_word;

	w[0] = w[2];
	w[1] = first >> 16;
	w[2] = second >> 16;
	/* Pa Pb in this frame, Pc next; or Pa last frame, Pb Pc in this one */
	if (w[1] == BURST_PA && w[2] == BURST_PB) {
		burst_hold = BURST_HOLD + 1;
	} else if (w[0] == BURST_PA && w[1] == BURST_PB) {
		burst_hold = BURST_HOLD;
		burst_type(w[2]);
	} else if (burst_hold == BURST_HOLD) {
		burst_type(w[1]);
	}

	if (burst_hold && !--burst_hold) {
		SH->bitstream = 0;
		ac3_reset();
	}
}

static uint32_t used, front[2];
uint32_t decim = 1;			/* 2: every other frame is processed */
static int32_t rx_acc[DSPAUDIO_IN];
static unsigned int rx_odd;

/* a new rate: no half-sum frame carries over; says whether it changed */
unsigned int rx_follow_rate(void)
{
	unsigned int was = decim, in;

	decim = SH->decim == 2 ? 2 : 1;
	if (decim == was) {
		return 0;
	}

	rx_odd = 0;
	for (in = 0; in < DSPAUDIO_IN; in++) {
		rx_acc[in] = 0;
	}

	return 1;
}

/*
 * One receive slot: rx holds the RX_SERS serializers' samples of the slot,
 * odd says it is the second of the frame. Returns whether a frame is in:
 * every frame, or every other one above 48 kHz.
 */
unsigned int rx_slot(unsigned int odd, const int32_t *rx)
{
	uint32_t mag, was;
	int32_t smp;
	unsigned int in, i, j, ready;

	for (i = 0; i < RX_SERS; i++) {
		in = 2 * i + (odd ^ (RX_FLIP >> i & 1));
		smp = rx[i];
		if (i == 0) {
			front[odd] = smp;
			if (odd) {
				was = burst_hold;
				burst_find(front[0], front[1]);
				/* found with this frame: its first half is stored already */
				for (j = 0; !was && burst_hold && j < DSPAUDIO_IN; j++) {
					input[j] = 0;
				}
			}

			used = burst_hold || SH->ring_on ? 0 : SH->inputs;
		}

		/* the receiver's tone controls are for the front pair */
		if (decim == 2) {
			rx_acc[in] += smp >> 1;
		} else if (in >= 2 || !SH->ring_on) {
			/* the ring's frame goes in as a whole, below */
			input[in] = !(used & (1u << in)) ? 0 : in < 2 ? tone(in, smp) : smp;
		}

		mag = smp < 0 ? -(uint32_t)smp : (uint32_t)smp;
		if (mag > SH->peak[in]) {
			SH->peak[in] = mag;
		}
	}

	/* a frame in: every frame, or every other one, the mean of two */
	ready = odd && (decim == 1 || (rx_odd ^= 1) == 0);
	for (in = 0; ready && decim == 2 && in < DSPAUDIO_IN; in++) {
		if (in >= 2 || !SH->ring_on) {
			input[in] = !(used & (1u << in)) ? 0 :
				    in < 2 ? tone(in, rx_acc[in]) : rx_acc[in];
		}
		rx_acc[in] = 0;
	}

	/* the NET input: the front pair from Linux */
	if (ready && SH->ring_on) {
		int32_t l, r;

		ring_take(&l, &r);
		input[0] = tone(0, l);
		input[1] = tone(1, r);
	}

#ifdef DSPAUDIO_AC3
	if (ready && burst_hold && !SH->ring_on && (SH->bitstream & 0x1f) == IEC_AC3) {
		ac3_take();
	}
#endif

	for (in = 0; ready && in < 2; in++) {
		float x = input[in] / FULL_SCALE;

		mag = input[in] < 0 ? -(uint32_t)input[in] : (uint32_t)input[in];
		if (mag > SH->meter[in]) {
			SH->meter[in] = mag;
		}
		SH->meter_sq[in] += x * x;
	}
	SH->meter_n += ready;

	if (ready && SH->surround.on) {
		upmix(input[0], input[1]);
	}

	SH->samples++;
	return ready;
}
