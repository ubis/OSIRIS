// SPDX-License-Identifier: BSD-2-Clause
/*
 * IEC 61937: compressed audio carried as 16-bit PCM words. A burst is the
 * preamble Pa Pb (F872 4E1F), Pc (data type), Pd (payload length: bits, or
 * bytes for E-AC-3), then the payload, two bytes a word with the first byte
 * high. Bursts repeat once per encoded frame; the gap is filled with zeros.
 */
#include "iec61937.h"

#define PA	0xF872u
#define PB	0x4E1Fu

enum { SEEK, PC, PD, DATA };

void iec61937_init(struct iec61937 *d)
{
	d->state = SEEK;
	d->prev = 0;
}

uint32_t iec61937_word(struct iec61937 *d, uint16_t w)
{
	uint16_t prev = d->prev;

	d->prev = w;
	switch (d->state) {
	case SEEK:
		if (prev == PA && w == PB) {
			d->state = PC;
		}
		return 0;
	case PC:
		d->type = w & 0x1f;
		d->state = PD;
		return 0;
	case PD:
		/* E-AC-3 gives bytes, the others bits */
		d->want = d->type == IEC_EAC3 ? w : (w + 7u) / 8;
		d->have = 0;
		/* NULL and pause bursts, and anything too long, carry nothing for us */
		d->state = d->want && d->want <= IEC_FRAME_MAX &&
			   d->type != 0 && d->type != 3 ? DATA : SEEK;
		return 0;
	default:
		d->frame[d->have++] = w >> 8;
		if (d->have < d->want) {
			d->frame[d->have++] = w & 0xff;
		}

		if (d->have < d->want) {
			return 0;
		}

		d->state = SEEK;
		d->prev = 0;
		return d->type;
	}
}
