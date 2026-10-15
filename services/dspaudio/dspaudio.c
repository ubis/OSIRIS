// SPDX-License-Identifier: BSD-2-Clause
/*
 * dspaudio: the audio path on the C674x DSP.
 *
 *   PCM9211 main output --I2S--> McASP1 AXR1_4 (receive, external BCK/LRCK)
 *   HDMI receiver, multichannel PCM --I2S--> AXR1_5, AXR1_8, AXR1_9
 *   McASP1 AXR1_0..3 (transmit, BCK = AHCLKX / 4) --I2S--> PCM1690
 *
 * Every output is the sum of the inputs, each times the gain dspd sets for
 * that output and input (struct dspaudio_shared), clipped, or comes from the
 * surround decoder; the bass of small speakers moves to the subwoofer, and
 * each output is held back by its speaker's distance. The eight outputs are
 * made together, one frame per sample period. No DMA, FIFO or interrupts: the DSP
 * polls the McASP and moves one sample per slot through XBUF/RBUF. Clock
 * and format settings are the ones the stock DSP image uses.
 */
#include <stdint.h>

#include "dspaudio.h"

#define REG(a)		(*(volatile uint32_t *)(a))
#define ETIMEDOUT	110

/* PSC1: McASP1 is LPSC 8 */
#define PSC1_BASE	0x01E27000u
#define PSC_PTCMD	(PSC1_BASE + 0x120)
#define PSC_PTSTAT	(PSC1_BASE + 0x128)
#define PSC_MDSTAT(n)	(PSC1_BASE + 0x800 + 4 * (n))
#define PSC_MDCTL(n)	(PSC1_BASE + 0xA00 + 4 * (n))
#define PSC_ENABLE	3
#define LPSC_MCASP1	8

/* McASP1 */
#define MCASP		0x01D04000u
#define AFIFO		0x01D05000u
#define PFUNC		(MCASP + 0x10)
#define PDIR		(MCASP + 0x14)
#define GBLCTL		(MCASP + 0x44)
#define AMUTE		(MCASP + 0x48)
#define DLBCTL		(MCASP + 0x4C)
#define DITCTL		(MCASP + 0x50)
#define RGBLCTL		(MCASP + 0x60)
#define RMASK		(MCASP + 0x64)
#define RFMT		(MCASP + 0x68)
#define AFSRCTL		(MCASP + 0x6C)
#define ACLKRCTL	(MCASP + 0x70)
#define AHCLKRCTL	(MCASP + 0x74)
#define RTDM		(MCASP + 0x78)
#define RINTCTL		(MCASP + 0x7C)
#define RSTAT		(MCASP + 0x80)
#define REVTCTL		(MCASP + 0x8C)
#define XGBLCTL		(MCASP + 0xA0)
#define XMASK		(MCASP + 0xA4)
#define XFMT		(MCASP + 0xA8)
#define AFSXCTL		(MCASP + 0xAC)
#define ACLKXCTL	(MCASP + 0xB0)
#define AHCLKXCTL	(MCASP + 0xB4)
#define XTDM		(MCASP + 0xB8)
#define XINTCTL		(MCASP + 0xBC)
#define XSTAT		(MCASP + 0xC0)
#define XEVTCTL		(MCASP + 0xCC)
#define SRCTL(n)	(MCASP + 0x180 + 4 * (n))
#define XBUF(n)		(MCASP + 0x200 + 4 * (n))
#define RBUF(n)		(MCASP + 0x280 + 4 * (n))
#define WFIFOCTL	(AFIFO + 0x10)
#define RFIFOCTL	(AFIFO + 0x18)

/* GBLCTL: receive in the low byte, transmit in the next */
#define RCLKRST		(1u << 0)
#define RHCLKRST	(1u << 1)
#define RSRCLR		(1u << 2)
#define RSMRST		(1u << 3)
#define RFRST		(1u << 4)
#define XCLKRST		(1u << 8)
#define XHCLKRST	(1u << 9)
#define XSRCLR		(1u << 10)
#define XSMRST		(1u << 11)
#define XFRST		(1u << 12)

/* XSTAT / RSTAT */
#define STAT_UNDERRUN	(1u << 0)	/* XUNDRN / ROVRN */
#define STAT_ODD_SLOT	(1u << 3)	/* XTDMSLOT / RTDMSLOT */
#define STAT_DATA	(1u << 5)	/* XDATA / RDATA */
#define STAT_ERR	(1u << 8)
#define STAT_CLEAR	0xFFFF

#define SRCTL_TX	1
#define SRCTL_RX	2
#define RX_SERS		4
/* inputs 2n and 2n + 1 come from rx_ser[n]; AXR1_5 has LFE as left, centre as right */
static const uint8_t rx_ser[RX_SERS] = { 4, 5, 8, 9 };
#define RX_FLIP		(1u << 1)
#define TX_SERS		4		/* AXR1_0..3: PCM1690 */

#define GBL_TIMEOUT	10000000
#define TX_STALL_LOOPS	200000		/* a few ms without a transmit request */

#define SH		((volatile struct dspaudio_shared *)DSPAUDIO_SHARED)

static void psc1_enable(unsigned int lpsc)
{
	if ((REG(PSC_MDSTAT(lpsc)) & 0x1F) == PSC_ENABLE) {
		return;
	}

	while (REG(PSC_PTSTAT) & 1) {
	}

	REG(PSC_MDCTL(lpsc)) = (REG(PSC_MDCTL(lpsc)) & ~0x1Fu) | PSC_ENABLE;
	REG(PSC_PTCMD) = 1;
	while (REG(PSC_PTSTAT) & 1) {
	}

	while ((REG(PSC_MDSTAT(lpsc)) & 0x1F) != PSC_ENABLE) {
	}
}

/*
 * Set GBLCTL bits and wait until they read back. Receive bits go through
 * RGBLCTL, transmit bits through XGBLCTL (GBLCTL does not reliably show the
 * receive half). A bit only latches while its clock runs, hence the timeout.
 */
static int gbl_set(uint32_t bits, uint32_t step)
{
	uint32_t rx = bits & 0x00FF, tx = bits & 0xFF00, n = 0;

	SH->state = step;
	if (rx) {
		REG(RGBLCTL) = REG(RGBLCTL) | rx;
	}

	if (tx) {
		REG(XGBLCTL) = REG(XGBLCTL) | tx;
	}

	while ((rx && (REG(RGBLCTL) & rx) != rx) ||
	       (tx && (REG(XGBLCTL) & tx) != tx)) {
		if (++n == GBL_TIMEOUT) {
			return -ETIMEDOUT;
		}
	}

	return 0;
}

static int mcasp_init(void)
{
	int i;

	SH->state = DSPAUDIO_INIT;
	REG(GBLCTL) = 0;
	while (REG(GBLCTL)) {
	}

	REG(WFIFOCTL) = 0;		/* XBUF/RBUF directly, no AFIFO */
	REG(RFIFOCTL) = 0;
	REG(XEVTCTL) = 1;		/* no DMA requests */
	REG(REVTCTL) = 1;
	REG(XINTCTL) = 0;
	REG(RINTCTL) = 0;
	REG(DLBCTL) = 0;
	REG(DITCTL) = 0;
	REG(AMUTE) = 0;

	/* receive: 2-slot I2S slave of the PCM9211 (external BCK and LRCK) */
	REG(RMASK) = 0xFFFFFFFF;
	REG(RFMT) = 0x0001C0F8;		/* 1-bit delay, MSB first, 32-bit slots */
	REG(AFSRCTL) = 0x00000111;	/* 2 slots, word frame sync, external, inverted */
	REG(ACLKRCTL) = 0x00000080;	/* external BCK, sample on the falling edge */
	REG(AHCLKRCTL) = 0x00000000;
	REG(RTDM) = 0x00000003;

	/* transmit: 2-slot I2S master, BCK = AHCLKX (12.288 MHz) / 4 = 64 fs */
	REG(XMASK) = 0xFFFFFFFF;
	REG(XFMT) = 0x000180F8;		/* 1-bit delay, MSB first, 32-bit slots */
	REG(AFSXCTL) = 0x00000113;	/* 2 slots, word frame sync, internal, inverted */
	REG(ACLKXCTL) = 0x000000E3;	/* internal BCK = AHCLKX / 4, async from receive */
	REG(AHCLKXCTL) = 0x00004000;	/* AHCLKX is an input (inverted), no divide */
	REG(XTDM) = 0x00000003;

	for (i = 0; i < 16; i++) {
		REG(SRCTL(i)) = 0;
	}

	for (i = 0; i < TX_SERS; i++) {
		REG(SRCTL(i)) = SRCTL_TX;
	}

	for (i = 0; i < RX_SERS; i++) {
		REG(SRCTL(rx_ser[i])) = SRCTL_RX;
	}
	REG(PFUNC) = 0;
	REG(PDIR) = 0x1400000F;		/* ACLKX, AFSX, AXR1_0..3 out; AHCLKX, ACLKR, AFSR in */

	/* receive has no high-frequency clock, only transmit's divider leaves reset */
	if (gbl_set(XHCLKRST, DSPAUDIO_INIT + 1) ||
	    gbl_set(XCLKRST | RCLKRST, DSPAUDIO_INIT + 2)) {
		return -ETIMEDOUT;
	}

	REG(XSTAT) = STAT_CLEAR;
	REG(RSTAT) = STAT_CLEAR;
	if (gbl_set(XSRCLR | RSRCLR, DSPAUDIO_INIT + 3)) {
		return -ETIMEDOUT;
	}

	for (i = 0; i < TX_SERS; i++) {
		REG(XBUF(i)) = 0;
	}

	if (gbl_set(XSMRST | RSMRST, DSPAUDIO_INIT + 4) ||
	    gbl_set(XFRST | RFRST, DSPAUDIO_INIT + 5)) {
		return -ETIMEDOUT;
	}

	return 0;
}

/* restart the transmitter after an underrun stopped it */
static void tx_restart(void)
{
	int i;

	REG(XGBLCTL) = REG(XGBLCTL) & ~(XSMRST | XFRST);
	REG(XSTAT) = STAT_CLEAR;
	for (i = 0; i < TX_SERS; i++) {
		REG(XBUF(i)) = 0;
	}

	gbl_set(XSMRST, DSPAUDIO_INIT + 4);
	gbl_set(XFRST, DSPAUDIO_INIT + 5);
	SH->state = DSPAUDIO_RUNNING;
}

static int32_t input[DSPAUDIO_IN];

#define RING		((volatile struct dspaudio_ring *)DSPAUDIO_RING)

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
 * words F872 4E1F, then one with the data type. The DSP cannot decode them;
 * played as PCM they are full scale noise, so the inputs stay silent while
 * bursts keep coming. The longest gap between them is 6144 frames (E-AC-3).
 */
#define BURST_PA	0xF872u
#define BURST_PB	0x4E1Fu
#define BURST_HOLD	16384		/* frames, ~340 ms at 48 kHz */

static uint32_t burst_hold, burst_word[3];	/* the last words of the front pair */

/* the data type, but not NULL and pause bursts, which come around a stream */
static void burst_type(uint32_t pc)
{
	pc &= 0x1f;
	if (pc != 0 && pc != 3) {
		SH->bitstream = pc;
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
	}
}
static float tone_z[2][DSPAUDIO_TONE][2];

/* the biquads in transposed direct form II, on one input sample */
static int32_t tone(unsigned int in, int32_t smp)
{
	const volatile struct dspaudio_biquad *q = SH->tone[SH->tone_sel & 1];
	float x = (float)smp, y;
	unsigned int k;

	for (k = 0; k < DSPAUDIO_TONE; k++) {
		float *z = tone_z[in][k];

		y = q[k].b0 * x + z[0];
		z[0] = q[k].b1 * x - q[k].a1 * y + z[1];
		z[1] = q[k].b2 * x - q[k].a2 * y;
		x = y;
	}

	if (x >= 2147483520.0f) {
		return INT32_MAX;
	}

	if (x <= -2147483648.0f) {
		return INT32_MIN;
	}

	return (int32_t)x;
}

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

#define DELAY_LEN	(DSPAUDIO_DELAY_MAX + 1)

static int32_t delay_buf[DSPAUDIO_OUT][DELAY_LEN];
static unsigned int delay_pos[DSPAUDIO_OUT];

#define SUR_LEN		(DSPAUDIO_SURROUND_DELAY_MAX + 1)
#define FULL_SCALE	2147483648.0f

static float sur_ll, sur_rr, sur_lr;	/* smoothed L*L, R*R, L*R */
static float sur_z[2][2];		/* the band filters' state */
static float sur_buf[SUR_LEN];
static unsigned int sur_pos;
static float sur_ap;			/* all-pass state, right surround */
static float upmixed[DSPAUDIO_OUT];	/* -1..1 */

static float clampf(float x, float lo, float hi)
{
	return x < lo ? lo : x > hi ? hi : x;
}

/* 1 / sqrt(x) for x > 0, close enough for a gain: bit trick, two Newton steps */
static float rsqrt(float x)
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

static float biquad(const volatile struct dspaudio_biquad *q, float *z, float x)
{
	float y = q->b0 * x + z[0];

	z[0] = q->b1 * x - q->a1 * y + z[1];
	z[1] = q->b2 * x - q->a2 * y;
	return y;
}

static float sur_tap(unsigned int d)
{
	if (d > DSPAUDIO_SURROUND_DELAY_MAX) {
		d = DSPAUDIO_SURROUND_DELAY_MAX;
	}

	return sur_buf[sur_pos >= d ? sur_pos - d : sur_pos + SUR_LEN - d];
}

#define REVERB_LEN	(DSPAUDIO_REVERB_LINE_MAX + 1)
#define PREDELAY_LEN	(DSPAUDIO_PREDELAY_MAX + 1)

static float rev_line[DSPAUDIO_REVERB_LINES][REVERB_LEN] __attribute__((section(".shram")));
static float rev_pre[PREDELAY_LEN] __attribute__((section(".shram")));
static unsigned int rev_pos[DSPAUDIO_REVERB_LINES], pre_pos;
static float rev_lp[DSPAUDIO_REVERB_LINES];

extern uint32_t __shram_start[], __shram_end[];

static void reverb_clear(void)
{
	uint32_t *p;

	for (p = __shram_start; p < __shram_end; p++) {
		*p = 0;
	}
}

/* adds the reverb of one sample pair to upmixed[] */
static void reverb(float m)
{
	const volatile struct dspaudio_reverb *s = &SH->reverb;
	float o[DSPAUDIO_REVERB_LINES], h0, h1, h2, h3, x;
	unsigned int i, d = s->predelay, len, r;

	if (d > DSPAUDIO_PREDELAY_MAX) {
		d = DSPAUDIO_PREDELAY_MAX;
	}

	rev_pre[pre_pos] = m;
	x = rev_pre[pre_pos >= d ? pre_pos - d : pre_pos + PREDELAY_LEN - d];
	pre_pos = pre_pos + 1 == PREDELAY_LEN ? 0 : pre_pos + 1;

	/* the oldest sample of each line, damped and decayed */
	for (i = 0; i < DSPAUDIO_REVERB_LINES; i++) {
		len = s->length[i];
		if (len < 1 || len > DSPAUDIO_REVERB_LINE_MAX) {
			len = DSPAUDIO_REVERB_LINE_MAX;
		}

		r = rev_pos[i] >= len ? rev_pos[i] - len : rev_pos[i] + REVERB_LEN - len;
		rev_lp[i] += (1.0f - s->damping) * (rev_line[i][r] - rev_lp[i]);
		o[i] = rev_lp[i] * s->decay[i];
	}

	/* Hadamard 4 x 4, scaled to keep the energy */
	h0 = 0.5f * (o[0] + o[1] + o[2] + o[3]);
	h1 = 0.5f * (o[0] - o[1] + o[2] - o[3]);
	h2 = 0.5f * (o[0] + o[1] - o[2] - o[3]);
	h3 = 0.5f * (o[0] - o[1] - o[2] + o[3]);
	rev_line[0][rev_pos[0]] = h0 + x;
	rev_line[1][rev_pos[1]] = h1 + x;
	rev_line[2][rev_pos[2]] = h2 + x;
	rev_line[3][rev_pos[3]] = h3 + x;
	for (i = 0; i < DSPAUDIO_REVERB_LINES; i++) {
		rev_pos[i] = rev_pos[i] + 1 == REVERB_LEN ? 0 : rev_pos[i] + 1;
	}

	/* different lines on each side: the sides are not correlated */
	upmixed[DSPAUDIO_OUT_FL] += s->front * o[0];
	upmixed[DSPAUDIO_OUT_FR] += s->front * o[1];
	upmixed[DSPAUDIO_OUT_SL] += s->surround * o[2];
	upmixed[DSPAUDIO_OUT_SR] += s->surround * o[3];
	upmixed[DSPAUDIO_OUT_SBL] += s->back * o[1];
	upmixed[DSPAUDIO_OUT_SBR] += s->back * o[0];
}

/* the eight outputs from one stereo sample pair, struct dspaudio_surround */
static void upmix(int32_t li, int32_t ri)
{
	const volatile struct dspaudio_surround *s = &SH->surround;
	float l = li / FULL_SCALE, r = ri / FULL_SCALE, a = s->smooth;
	float rho, c, m, diff, sl, sr, x, ap;
	unsigned int d = s->delay, bd = d + s->back_delay;

	sur_ll += a * (l * l - sur_ll);
	sur_rr += a * (r * r - sur_rr);
	sur_lr += a * (l * r - sur_lr);

	/* in-phase correlation 0..1: how much of the sound sits between the speakers */
	rho = sur_ll * sur_rr > 1e-12f ? clampf(sur_lr * rsqrt(sur_ll * sur_rr), 0.0f, 1.0f) : 0.0f;
	m = 0.5f * (l + r);
	c = s->center * rho * m;
	upmixed[DSPAUDIO_OUT_FL] = l - c;
	upmixed[DSPAUDIO_OUT_FR] = r - c;
	/* same power as the two front speakers gave it */
	upmixed[DSPAUDIO_OUT_C] = 1.41421356f * c;
	upmixed[DSPAUDIO_OUT_SW] = s->sub * m;

	/* the difference, less of it the more the sound is in phase, by steer */
	diff = 0.5f * (l - r) * s->surround * (1.0f - s->steer * rho);
	diff = biquad(&s->band[0], sur_z[0], diff);
	diff = biquad(&s->band[1], sur_z[1], diff);
	sur_buf[sur_pos] = diff;

	x = sur_tap(d);
	/* first order all-pass on the right one: the pair is not one mono source */
	ap = -0.6f * x + sur_ap;
	sur_ap = x + 0.6f * ap;
	sl = x + s->wrap * upmixed[DSPAUDIO_OUT_FL];
	sr = -ap + s->wrap * upmixed[DSPAUDIO_OUT_FR];
	upmixed[DSPAUDIO_OUT_SL] = sl;
	upmixed[DSPAUDIO_OUT_SR] = sr;

	x = sur_tap(bd);
	upmixed[DSPAUDIO_OUT_SBL] = s->back * x;
	upmixed[DSPAUDIO_OUT_SBR] = -s->back * x;
	sur_pos = sur_pos + 1 == SUR_LEN ? 0 : sur_pos + 1;
	if (SH->reverb.on) {
		reverb(m);
	}
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

static int32_t delayed(unsigned int out, int32_t v)
{
	unsigned int w = delay_pos[out], d = SH->delay[out], r;

	if (d > DSPAUDIO_DELAY_MAX) {
		d = DSPAUDIO_DELAY_MAX;
	}

	delay_buf[out][w] = v;
	delay_pos[out] = w + 1 == DELAY_LEN ? 0 : w + 1;
	r = w >= d ? w - d : w + DELAY_LEN - d;
	return delay_buf[out][r];
}

/*
 * A copy of the set dspd selected, so that the filters run from plain
 * memory. Copying it at once takes longer than a sample period: it is
 * copied a few words per frame into the spare one, then switched to.
 */
#define BASS_WORDS	(sizeof(struct dspaudio_bass) / 4)
#define BASS_STEP	16

static struct dspaudio_bass bass_buf[2];
static const struct dspaudio_bass *bm = &bass_buf[0];
static uint32_t bass_gen, copy_gen, bass_copy;	/* words of the spare done */
static float bass_z[DSPAUDIO_OUT][2][DSPAUDIO_XO][2];	/* [high, low] */
static float sub_z[DSPAUDIO_XO][2];
static float eq_z[DSPAUDIO_OUT][DSPAUDIO_EQ][2];
static float loud_z[DSPAUDIO_OUT][2];

static float cascade(const struct dspaudio_biquad *q, float (*z)[2], float x)
{
	unsigned int k;

	for (k = 0; k < DSPAUDIO_XO; k++) {
		float y = q[k].b0 * x + z[k][0];

		z[k][0] = q[k].b1 * x - q[k].a1 * y + z[k][1];
		z[k][1] = q[k].b2 * x - q[k].a2 * y;
		x = y;
	}

	return x;
}

static void bass_reset(void)
{
	unsigned int i;

	for (i = 0; i < sizeof(bass_z) / sizeof(float); i++) {
		((float *)bass_z)[i] = 0.0f;
	}

	for (i = 0; i < sizeof(sub_z) / sizeof(float); i++) {
		((float *)sub_z)[i] = 0.0f;
	}

	for (i = 0; i < sizeof(eq_z) / sizeof(float); i++) {
		((float *)eq_z)[i] = 0.0f;
	}

	for (i = 0; i < sizeof(loud_z) / sizeof(float); i++) {
		((float *)loud_z)[i] = 0.0f;
	}
}

/* the same filters in the same places: their state still fits */
static int bass_same_layout(const struct dspaudio_bass *a, const struct dspaudio_bass *b)
{
	unsigned int out;

	if (a->small != b->small || a->copy != b->copy || a->subwoofer != b->subwoofer ||
	    a->loud_on != b->loud_on) {
		return 0;
	}

	for (out = 0; out < DSPAUDIO_OUT; out++) {
		if (a->eq_n[out] != b->eq_n[out]) {
			return 0;
		}
	}

	return 1;
}

static void bass_follow(void)
{
	uint32_t gen = SH->bass_gen;
	struct dspaudio_bass *spare = bm == &bass_buf[0] ? &bass_buf[1] : &bass_buf[0];
	const volatile uint32_t *src = (const volatile uint32_t *)&SH->bass[gen & 1];
	uint32_t *dst = (uint32_t *)spare;
	unsigned int end;

	if (gen == bass_gen) {
		return;
	}

	/* a newer set than the one being copied: start over */
	if (gen != copy_gen) {
		copy_gen = gen;
		bass_copy = 0;
	}

	end = bass_copy + BASS_STEP < BASS_WORDS ? bass_copy + BASS_STEP : BASS_WORDS;
	for (; bass_copy < end; bass_copy++) {
		dst[bass_copy] = src[bass_copy];
	}

	if (bass_copy < BASS_WORDS) {
		return;
	}

	bass_copy = 0;
	/* dspd may have rewritten this set while it was copied */
	if (SH->bass_gen != gen) {
		return;
	}

	/* other filters: the old state does not fit them, clear it */
	if (!bass_same_layout(bm, spare)) {
		bass_reset();
	}

	bm = spare;
	bass_gen = gen;
}

static float bass_sub;

/* outputs from .. to - 1 */
static void bass_split(float *y, unsigned int from, unsigned int to)
{
	unsigned int out;

	for (out = from; out < to; out++) {
		if (out == DSPAUDIO_OUT_SW || !((bm->small | bm->copy) & (1u << out))) {
			continue;
		}

		bass_sub += cascade(bm->low[out], bass_z[out][1], y[out]);
		if (bm->small & (1u << out)) {
			y[out] = cascade(bm->high[out], bass_z[out][0], y[out]);
		}
	}
}

/* the loudness shelf and the equalizer of outputs from .. to - 1 */
static void eq_split(float *y, unsigned int from, unsigned int to)
{
	unsigned int out, k, n;

	for (out = from; out < to; out++) {
		if (bm->loud_on) {
			const struct dspaudio_biquad *q = &bm->loud;
			float *z = loud_z[out], x = y[out], v = q->b0 * x + z[0];

			z[0] = q->b1 * x - q->a1 * v + z[1];
			z[1] = q->b2 * x - q->a2 * v;
			y[out] = v;
		}

		n = bm->eq_n[out] < DSPAUDIO_EQ ? bm->eq_n[out] : DSPAUDIO_EQ;
		for (k = 0; k < n; k++) {
			const struct dspaudio_biquad *q = &bm->eq[out][k];
			float *z = eq_z[out][k], x = y[out], v = q->b0 * x + z[0];

			z[0] = q->b1 * x - q->a1 * v + z[1];
			z[1] = q->b2 * x - q->a2 * v;
			y[out] = v;
		}
	}
}

static void bass_finish(float *y)
{
	float sub = bass_sub;

	if (bm->subwoofer) {
		y[DSPAUDIO_OUT_SW] = cascade(bm->sub, sub_z, y[DSPAUDIO_OUT_SW] + sub);
	} else {
		y[DSPAUDIO_OUT_FL] += 0.5f * sub;
		y[DSPAUDIO_OUT_FR] += 0.5f * sub;
	}
}

#define LIMIT		0.95f		/* -0.45 dBFS */
#define LIMIT_RELEASE	0.99995f	/* per sample: ~0.4 s back to unity */

static int32_t frame[DSPAUDIO_OUT];

/*
 * The A/V sync delay line, in SDRAM, cached (MAR195 covers 0xC3000000 to
 * 0xC3FFFFFF). L1D does not keep writes: the top 64 KiB of L2, which the
 * image and the shared block below 0x11820000 leave alone, become cache,
 * so writes land there and go out to SDRAM a line at a time.
 */
#define MAR(n)		(0x01848000u + 4 * (n))
#define L2CFG		0x01840000u
#define L2CFG_64K	2		/* L2MODE: 64 KiB cache at the top of L2 */
#define AV_MASK		(DSPAUDIO_AV_LEN - 1)

static int32_t (*const av_buf)[DSPAUDIO_OUT] = (int32_t (*)[DSPAUDIO_OUT])DSPAUDIO_AV_BUF;
static unsigned int av_pos;

static void av_clear(void)
{
	unsigned int i, out;

	REG(L2CFG) = (REG(L2CFG) & ~7u) | L2CFG_64K;
	(void)REG(L2CFG);
	REG(MAR(DSPAUDIO_AV_BUF >> 24)) = 1;
	for (i = 0; i < DSPAUDIO_AV_LEN; i++) {
		for (out = 0; out < DSPAUDIO_OUT; out++) {
			av_buf[i][out] = 0;
		}
	}
}
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
static void dvol_apply(float *y)
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

/*
 * A processed frame is made in four steps. At 48 kHz two run after each
 * slot; above it each slot gets one, every other frame is processed and
 * played twice, and the inputs are the mean of two frames.
 */
static void frame_step1(void)
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

	bass_sub = 0.0f;
	if (!frame_noise) {
		bass_split(frame_y, 0, DSPAUDIO_OUT / 2);
	}
}

static void frame_step2(void)
{
	/* the subwoofer's is only final after step 3 */
	if (!frame_noise) {
		eq_split(frame_y, 0, DSPAUDIO_OUT_SW);
	}
}

static void frame_step3(void)
{
	float *y = frame_y;

	if (!frame_noise) {
		bass_split(y, DSPAUDIO_OUT / 2, DSPAUDIO_OUT);
		bass_finish(y);
	} else if (bm->subwoofer) {
		/* the subwoofer is measured as it plays: through its low pass */
		y[DSPAUDIO_OUT_SW] = cascade(bm->sub, sub_z, y[DSPAUDIO_OUT_SW]);
	}
}

/* the processed frame goes out from the next slot 0 on */
static void frame_step4(void)
{
	uint32_t d, r;
	float *y = frame_y;
	unsigned int out;

	if (!frame_noise) {
		eq_split(y, DSPAUDIO_OUT_SW, DSPAUDIO_OUT);
	}

	d = SH->av_delay;
	r = (av_pos - (d < DSPAUDIO_AV_LEN ? d : AV_MASK)) & AV_MASK;
	for (out = 0; out < DSPAUDIO_OUT; out++) {
		y[out] = limit(out, y[out]);
		av_buf[av_pos][out] = delayed(out, (int32_t)(clampf(y[out], -1.0f, 0.99999994f) * FULL_SCALE));
		frame[out] = av_buf[r][out];
	}

	av_pos = (av_pos + 1) & AV_MASK;
}

static void report_start(void)
{
	static const char v[] = DSPAUDIO_VERSION;
	unsigned int i;

	SH->magic = 0;
	SH->abi = DSPAUDIO_ABI;
	for (i = 0; i < sizeof(SH->version); i++) {
		SH->version[i] = i < sizeof(v) ? v[i] : 0;
	}

	SH->version[sizeof(SH->version) - 1] = 0;
	SH->samples = 0;
	SH->underruns = 0;
	SH->overruns = 0;
	SH->state = DSPAUDIO_POWER;
}

void dspaudio_main(void)
{
	uint32_t xs, rs, idle = 0, mag, used = 0, was, front[2] = { 0, 0 }, decim, last_decim = 1;
	int32_t rx_acc[DSPAUDIO_IN] = { 0 };
	unsigned int rx_odd = 0, tx_odd = 0, ready;
	int32_t smp;
	unsigned int slot, odd, in, i, j;

	report_start();
	reverb_clear();
	av_clear();
	RING->read = RING->write = 0;
	psc1_enable(LPSC_MCASP1);
	if (mcasp_init()) {
		/* leave the step that timed out for dspd to report */
		SH->state += DSPAUDIO_STUCK;
		for (;;) {
		}
	}

	SH->state = DSPAUDIO_RUNNING;
	SH->magic = DSPAUDIO_MAGIC;

	/*
	 * Receive (PCM9211 clock) and transmit (McASP clock) are separate clock
	 * domains, so never block on one side: serve whichever is ready.
	 */
	for (;;) {
		/* a new rate: no half-sum or half-made frame carries over */
		decim = SH->decim == 2 ? 2 : 1;
		if (decim != last_decim) {
			last_decim = decim;
			rx_odd = tx_odd = 0;
			for (in = 0; in < DSPAUDIO_IN; in++) {
				rx_acc[in] = 0;
			}
		}

		rs = REG(RSTAT);
		if (rs & STAT_DATA) {
			odd = (rs & STAT_ODD_SLOT) ? 1 : 0;
			slot = odd ^ (SH->swap & 1);
			for (i = 0; i < RX_SERS; i++) {
				in = 2 * i + (slot ^ (RX_FLIP >> i & 1));
				smp = (int32_t)REG(RBUF(rx_ser[i]));
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

			if (ready && SH->surround.on) {
				upmix(input[0], input[1]);
			}

			SH->samples++;
			if (rs & STAT_UNDERRUN) {
				SH->overruns++;
				REG(RSTAT) = STAT_UNDERRUN | STAT_ERR;
			}
		}

		xs = REG(XSTAT);
		if (xs & STAT_DATA) {
			/* XTDMSLOT is the slot going out now; XBUF is for the next one */
			slot = (xs & STAT_ODD_SLOT) ? 0 : 1;
			for (i = 0; i < TX_SERS; i++) {
				REG(XBUF(i)) = (uint32_t)frame[2 * i + slot];
			}

			if (decim != 2) {
				if (slot == 0) {
					frame_step1();
					frame_step2();
				} else {
					frame_step3();
					frame_step4();
				}
			} else if (slot == 0) {
				tx_odd ? frame_step3() : frame_step1();
			} else {
				tx_odd ? frame_step4() : frame_step2();
				tx_odd ^= 1;
			}

			idle = 0;
		} else if (++idle == TX_STALL_LOOPS) {
			SH->underruns++;
			tx_restart();
			idle = 0;
		}

		if (xs & STAT_UNDERRUN) {
			REG(XSTAT) = STAT_UNDERRUN | STAT_ERR;
		}
	}
}
