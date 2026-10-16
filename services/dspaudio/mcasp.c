// SPDX-License-Identifier: BSD-2-Clause
/*
 * McASP1: power, clocks and serializers for both directions, with the clock
 * and format settings the stock DSP image uses.
 */
#include <stdint.h>

#include "dsp.h"
#include "regs.h"

#define ETIMEDOUT	110

#define GBL_TIMEOUT	10000000

#ifdef DSPAUDIO_DMA
#define FMT_BUSEL	0			/* X/RBUSEL 0: the data port */
#else
#define FMT_BUSEL	(1u << 3)		/* X/RBUSEL 1: XBUF/RBUF directly */
#endif

const uint8_t rx_ser[RX_SERS] = { 4, 5, 8, 9 };

void psc1_enable(unsigned int lpsc)
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

int mcasp_init(void)
{
	int i;

	SH->state = DSPAUDIO_INIT;
	/* GBLCTL changes on the McASP's clocks: without them it never reads 0 */
	REG(GBLCTL) = 0;
	for (i = 0; REG(GBLCTL); i++) {
		if (i == GBL_TIMEOUT) {
			return -ETIMEDOUT;
		}
	}

	REG(WFIFOCTL) = 0;		/* XBUF/RBUF directly, no AFIFO */
	REG(RFIFOCTL) = 0;
#ifdef DSPAUDIO_DMA
	REG(XEVTCTL) = 0;		/* EDMA serves the data port */
	REG(REVTCTL) = 0;
#else
	REG(XEVTCTL) = 1;		/* no DMA requests */
	REG(REVTCTL) = 1;
#endif
	REG(XINTCTL) = 0;
	REG(RINTCTL) = 0;
	REG(DLBCTL) = 0;
	REG(DITCTL) = 0;
	REG(AMUTE) = 0;

	/* receive: 2-slot I2S slave of the PCM9211 (external BCK and LRCK) */
	REG(RMASK) = 0xFFFFFFFF;
	REG(RFMT) = 0x0001C0F0 | FMT_BUSEL;	/* 1-bit delay, MSB first, 32-bit slots */
	REG(AFSRCTL) = 0x00000111;	/* 2 slots, word frame sync, external, inverted */
	REG(ACLKRCTL) = 0x00000080;	/* external BCK, sample on the falling edge */
	REG(AHCLKRCTL) = 0x00000000;
	REG(RTDM) = 0x00000003;

	/* transmit: 2-slot I2S master, BCK = AHCLKX (12.288 MHz) / 4 = 64 fs */
	REG(XMASK) = 0xFFFFFFFF;
	REG(XFMT) = 0x000180F0 | FMT_BUSEL;	/* 1-bit delay, MSB first, 32-bit slots */
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
#ifdef DSPAUDIO_DMA
	/* armed before XSRCLR, whose transmit request fills the buffers */
	dma_start();
#endif
	if (gbl_set(XSRCLR | RSRCLR, DSPAUDIO_INIT + 3)) {
		return -ETIMEDOUT;
	}

#ifdef DSPAUDIO_DMA
	for (i = 0; REG(XSTAT) & STAT_DATA; i++) {
		if (i == GBL_TIMEOUT) {
			return -ETIMEDOUT;
		}
	}
#else
	for (i = 0; i < TX_SERS; i++) {
		REG(XBUF(i)) = 0;
	}
#endif

	if (gbl_set(XSMRST | RSMRST, DSPAUDIO_INIT + 4) ||
	    gbl_set(XFRST | RFRST, DSPAUDIO_INIT + 5)) {
		return -ETIMEDOUT;
	}

	return 0;
}

/* restart the transmitter after an underrun stopped it */
void tx_restart(void)
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
