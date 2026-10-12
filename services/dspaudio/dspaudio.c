// SPDX-License-Identifier: BSD-2-Clause
/*
 * dspaudio: the audio path on the C674x DSP.
 *
 *   PCM9211 main output --I2S--> McASP1 AXR1_4 (receive, external BCK/LRCK)
 *   McASP1 AXR1_0..3 (transmit, BCK = AHCLKX / 4) --I2S--> PCM1690
 *
 * Every output takes the input dspd routes to it times the gain dspd sets
 * (struct dspaudio_shared), clipped. No DMA, FIFO or interrupts: the DSP
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
#define RX_SER		4		/* AXR1_4: PCM9211 */
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

	REG(SRCTL(RX_SER)) = SRCTL_RX;
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

static uint32_t output(unsigned int out)
{
	unsigned int r = SH->route[out];
	int64_t v;

	if (r >= DSPAUDIO_IN) {
		return 0;
	}

	v = ((int64_t)input[r] * SH->gain[out]) >> 24;
	if (v > INT32_MAX) {
		v = INT32_MAX;
	} else if (v < INT32_MIN) {
		v = INT32_MIN;
	}

	return (uint32_t)v;
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
	uint32_t xs, rs, idle = 0, mag;
	int32_t smp;
	unsigned int slot, i;

	report_start();
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
		rs = REG(RSTAT);
		if (rs & STAT_DATA) {
			slot = (rs & STAT_ODD_SLOT) ? 1 : 0;
			smp = (int32_t)REG(RBUF(RX_SER));
			input[slot] = smp;
			mag = smp < 0 ? -(uint32_t)smp : (uint32_t)smp;
			if (mag > SH->peak[slot]) {
				SH->peak[slot] = mag;
			}

			SH->samples++;
			if (rs & STAT_UNDERRUN) {
				SH->overruns++;
				REG(RSTAT) = STAT_UNDERRUN | STAT_ERR;
			}
		}

		xs = REG(XSTAT);
		if (xs & STAT_DATA) {
			slot = (xs & STAT_ODD_SLOT) ? 1 : 0;
			for (i = 0; i < TX_SERS; i++) {
				REG(XBUF(i)) = output(2 * i + slot);
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
