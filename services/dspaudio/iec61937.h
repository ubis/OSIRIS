/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef DSPAUDIO_IEC61937_H
#define DSPAUDIO_IEC61937_H

#include <stdint.h>

/* IEC 61937 data types (Pc bits 0-4) */
#define IEC_AC3		1
#define IEC_DTS1	11
#define IEC_DTS2	12
#define IEC_DTS3	13
#define IEC_EAC3	21

#define IEC_FRAME_MAX	8192	/* bytes: E-AC-3 bursts carry up to 6144 frames of data */

struct iec61937 {
	uint32_t state, type, want, have;	/* have, want: bytes of the payload */
	uint16_t prev;				/* the word before, for Pa Pb */
	uint8_t frame[IEC_FRAME_MAX];
};

void iec61937_init(struct iec61937 *d);
/*
 * One 16-bit word of the stream, in the order the samples came (left, right,
 * left, ...). Returns the data type once a burst's payload is complete, in
 * d->frame and d->have bytes; 0 otherwise.
 */
uint32_t iec61937_word(struct iec61937 *d, uint16_t w);

#endif
