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
 * made together, one frame per sample period. EDMA moves them in blocks of
 * frames, and the Dolby Digital decoder runs between blocks; AC3=0 leaves
 * the decoder out, and DMA=0 has the DSP poll the McASP instead, one sample
 * per slot through XBUF/RBUF.
 */
#include <stdint.h>

#include "dsp.h"
#include "regs.h"

#define TX_STALL_LOOPS	200000		/* a few ms without a transmit request */

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
	uint32_t xs, rs, idle = 0;
	unsigned int tx_odd = 0, slot, i;

	report_start();
#ifdef DSPAUDIO_AC3
	/* a decoder loaded anew into SDRAM: nothing of the old one cached */
	REG(L1DWB) = 1;
	while (REG(L1DWB)) {
	}
	REG(L2INV) = 1;
	while (REG(L2INV)) {
	}
	REG(L1PINV) = 1;
	while (REG(L1PINV)) {
	}
#endif
	reverb_clear();
	av_clear();
#ifdef DSPAUDIO_AC3
	ac3_init();
#endif
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
#ifdef DSPAUDIO_DMA
	dma_loop();
#endif

	/*
	 * Receive (PCM9211 clock) and transmit (McASP clock) are separate clock
	 * domains, so never block on one side: serve whichever is ready.
	 */
	for (;;) {
		/* a new rate: no half-made frame carries over */
		if (rx_follow_rate()) {
			tx_odd = 0;
		}

		rs = REG(RSTAT);
		if (rs & STAT_DATA) {
			int32_t rx[RX_SERS];

			for (i = 0; i < RX_SERS; i++) {
				rx[i] = (int32_t)REG(RBUF(rx_ser[i]));
			}

			rx_slot((rs & STAT_ODD_SLOT) ? 1 : 0, rx);
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
