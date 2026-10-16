// SPDX-License-Identifier: BSD-2-Clause
/* Biquads from the RBJ cookbook, at the rate the DSP processes */
#include <math.h>

#include "dspd.h"

#define EQ_Q		2.0		/* about the 2/3 octave between bands */

/* RBJ cookbook shelving filter, slope 1: low shelf for bass, high for treble */
void shelf(volatile struct dspaudio_biquad *q, double hz, int db, int high)
{
	double a = pow(10.0, db / 40.0), w = 2.0 * M_PI * hz / proc_hz;
	double c = cos(w), alpha = sin(w) / 2.0 * sqrt(2.0), r = 2.0 * sqrt(a) * alpha;
	double s = high ? -1.0 : 1.0, a0;

	a0 = (a + 1) + s * (a - 1) * c + r;
	q->b0 = a * ((a + 1) - s * (a - 1) * c + r) / a0;
	q->b1 = s * 2 * a * ((a - 1) - s * (a + 1) * c) / a0;
	q->b2 = a * ((a + 1) - s * (a - 1) * c - r) / a0;
	q->a1 = -s * 2 * ((a - 1) + s * (a + 1) * c) / a0;
	q->a2 = ((a + 1) + s * (a - 1) * c - r) / a0;
}

/* RBJ cookbook high or low pass, Q 0.707 */
void pass(volatile struct dspaudio_biquad *q, double hz, int high)
{
	double w = 2.0 * M_PI * hz / proc_hz, c = cos(w), alpha = sin(w) / (2.0 * M_SQRT1_2);
	double a0 = 1.0 + alpha, k = high ? (1.0 + c) / 2.0 : (1.0 - c) / 2.0;

	q->b0 = k / a0;
	q->b1 = (high ? -2.0 * k : 2.0 * k) / a0;
	q->b2 = k / a0;
	q->a1 = -2.0 * c / a0;
	q->a2 = (1.0 - alpha) / a0;
}

/* RBJ cookbook peaking filter */
void peak(volatile struct dspaudio_biquad *q, double hz, int db)
{
	double a = pow(10.0, db / 40.0), w = 2.0 * M_PI * hz / proc_hz;
	double alpha = sin(w) / (2.0 * EQ_Q), a0 = 1.0 + alpha / a;

	q->b0 = (1.0 + alpha * a) / a0;
	q->b1 = -2.0 * cos(w) / a0;
	q->b2 = (1.0 - alpha * a) / a0;
	q->a1 = q->b1;
	q->a2 = (1.0 - alpha / a) / a0;
}
