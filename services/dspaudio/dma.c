// SPDX-License-Identifier: BSD-2-Clause
/*
 * EDMA moves the audio between the McASP's data port and two blocks of
 * DMA_FRAMES frames in L2, each direction in ping-pong: a channel's PaRAM
 * reloads from the other half when one is done and flags it in IPR, which
 * the loop polls. One event per slot moves one word per serializer.
 */
#include <stdint.h>

#include "dsp.h"
#include "regs.h"

#define CH_RX		2			/* McASP1 AREVT */
#define CH_TX		3			/* McASP1 AXEVT */
/* PaRAM sets and completion codes from those TI leaves to the DSP */
#define SET_RX		32			/* reload sets: ping, pong */
#define SET_TX		34
#define TCC_RX		8			/* completion codes: ping, pong */
#define TCC_TX		12
#define OPT_ABSYNC	(1u << 2)
#define OPT_TCINTEN	(1u << 20)
#define OPT_TCC(c)	((uint32_t)(c) << 12)
#ifdef DSPAUDIO_AC3
#define DMA_FRAMES	128			/* half an AC-3 block */
#else
#define DMA_FRAMES	64			/* 1.3 ms at 48 kHz */
#endif

static int32_t dma_rx[2][DMA_FRAMES][2][RX_SERS] __attribute__((aligned(8)));
static int32_t dma_tx[2][DMA_FRAMES][2][TX_SERS] __attribute__((aligned(8)));

static void param(unsigned int n, unsigned int half, int tx, unsigned int link)
{
	volatile uint32_t *p = (volatile uint32_t *)PARAM(n);
	uint32_t buf = tx ? (uint32_t)dma_tx[half] : (uint32_t)dma_rx[half];
	uint32_t slot = 4 * (tx ? TX_SERS : RX_SERS);

	p[0] = OPT_ABSYNC | OPT_TCINTEN | OPT_TCC((tx ? TCC_TX : TCC_RX) + half);
	p[1] = tx ? buf : MCASP_DATA;
	p[2] = (slot / 4) << 16 | 4;		/* BCNT serializers, ACNT a word */
	p[3] = tx ? MCASP_DATA : buf;
	p[4] = tx ? 4 : 4u << 16;		/* B index: the buffer side steps */
	p[5] = (slot / 4) << 16 | PARAM_OFS(link);
	p[6] = tx ? slot : slot << 16;		/* C index: a slot on */
	p[7] = 2 * DMA_FRAMES;			/* events: slots in the block */
}

void dma_start(void)
{
	uint32_t ch = 1u << CH_RX | 1u << CH_TX;
	volatile uint32_t *p, *q;
	unsigned int i;

	REG(EDMA_EECR) = ch;
	REG(EDMA_ECR) = ch;
	REG(EDMA_SECR) = ch;
	REG(EDMA_EMCR) = ch;
	REG(EDMA_ICR) = 3u << TCC_RX | 3u << TCC_TX;
	/* both on queue 0 */
	REG(EDMA_DMAQNUM0) &= ~(7u << 4 * CH_RX | 7u << 4 * CH_TX);

	for (i = 0; i < 2; i++) {
		param(SET_RX + i, i, 0, SET_RX + (i ^ 1));
		param(SET_TX + i, i, 1, SET_TX + (i ^ 1));
	}

	/* the channels start with ping, then reload pong, ping, ... */
	p = (volatile uint32_t *)PARAM(CH_RX);
	q = (volatile uint32_t *)PARAM(SET_RX);
	for (i = 0; i < 8; i++) {
		p[i] = q[i];
	}
	p = (volatile uint32_t *)PARAM(CH_TX);
	q = (volatile uint32_t *)PARAM(SET_TX);
	for (i = 0; i < 8; i++) {
		p[i] = q[i];
	}

	REG(EDMA_EESR) = ch;
}

/* one block from EDMA: each frame through the same steps as the polled loop */
static void dma_block(unsigned int half, unsigned int to)
{
	unsigned int f, i;

	for (f = 0; f < DMA_FRAMES; f++) {
#ifdef DSPAUDIO_AC3
		ac3_word((uint32_t)dma_rx[half][f][0][0]);
		ac3_word((uint32_t)dma_rx[half][f][1][0]);
#endif
		rx_slot(0, dma_rx[half][f][0]);
		if (rx_slot(1, dma_rx[half][f][1])) {
			frame_step1();
			frame_step2();
			frame_step3();
			frame_step4();
		}

		for (i = 0; i < TX_SERS; i++) {
			dma_tx[to][f][0][i] = frame[2 * i];
			dma_tx[to][f][1][i] = frame[2 * i + 1];
		}
	}
}

/* the half of dma_tx EDMA is not reading now */
static unsigned int tx_free(void)
{
	uint32_t src = ((volatile uint32_t *)PARAM(CH_TX))[1];

	return src >= (uint32_t)dma_tx[1] ? 0 : 1;
}

/* McASP and EDMA from the start, after a stall or an error */
static void dma_restart(void)
{
	uint32_t n;

	REG(EDMA_EECR) = 1u << CH_RX | 1u << CH_TX;
	REG(GBLCTL) = 0;

	for (n = 0; n < sizeof(dma_tx) / 4; n++) {
		((int32_t *)dma_tx)[n] = 0;
	}
	/* without a receive clock the McASP will not start: until it comes */
	while (mcasp_init()) {
		REG(EDMA_EECR) = 1u << CH_RX | 1u << CH_TX;
		REG(GBLCTL) = 0;
	}
	SH->state = DSPAUDIO_RUNNING;
}

#define DMA_STALL_LOOPS	2000000		/* several blocks without a received one */

/*
 * Each received half goes into the transmit half EDMA is not reading, which
 * goes out next. A receive stall (no clock, an input switch) or a McASP
 * error stops both and starts them again from slot 0, in step and silent.
 */
void dma_loop(void)
{
	unsigned int half = 0;
	uint32_t done, n, err;

	for (;;) {
		rx_follow_rate();
		done = 1u << (TCC_RX + half);
		for (n = 0; !(REG(EDMA_IPR) & done) && n < DMA_STALL_LOOPS; n++) {
		}

		err = (REG(RSTAT) | REG(XSTAT)) & (STAT_UNDERRUN | STAT_SYNC_ERR | STAT_DMA_ERR);
		if (n == DMA_STALL_LOOPS || err) {
			SH->underruns++;
			dma_restart();
			half = 0;
			continue;
		}

		/* the other half in already: the block before took too long */
		if (REG(EDMA_IPR) & 1u << (TCC_RX + (half ^ 1))) {
			SH->overruns++;
		}

		REG(EDMA_ICR) = done | 3u << TCC_TX;
		dma_block(half, tx_free());
		half ^= 1;
#ifdef DSPAUDIO_AC3
		ac3_work();
#endif
	}
}
