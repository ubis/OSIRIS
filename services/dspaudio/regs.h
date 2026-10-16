/* SPDX-License-Identifier: BSD-2-Clause */
/* The DSP's view of the OMAP-L137 registers dspaudio uses */
#ifndef DSPAUDIO_REGS_H
#define DSPAUDIO_REGS_H

#include <stdint.h>

#define REG(a)		(*(volatile uint32_t *)(a))

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
#define STAT_DMA_ERR	(1u << 7)	/* XDMAERR / RDMAERR */
#define STAT_SYNC_ERR	(1u << 1)	/* XSYNCERR / RSYNCERR */

#define SRCTL_TX	1
#define SRCTL_RX	2

/* EDMA3 channel controller */
#define EDMA		0x01C00000u
#define EDMA_DMAQNUM0	(EDMA + 0x0240)
#define EDMA_EMCR	(EDMA + 0x0308)
#define EDMA_ECR	(EDMA + 0x1008)
#define EDMA_EECR	(EDMA + 0x1028)
#define EDMA_EESR	(EDMA + 0x1030)
#define EDMA_SECR	(EDMA + 0x1040)
#define EDMA_IPR	(EDMA + 0x1068)
#define EDMA_ICR	(EDMA + 0x1070)
#define PARAM_OFS(n)	(0x4000 + 32 * (n))
#define PARAM(n)	(EDMA + PARAM_OFS(n))
#define MCASP_DATA	0x01D06000u		/* McASP1 base + 0x2000 */

/* cache control */
#define MAR(n)		(0x01848000u + 4 * (n))
#define L2CFG		0x01840000u
#define L2INV		0x01845008u	/* write 1: invalidate the L2 cache */
#define L1DWB		0x01845040u
#define L1PINV		0x01845028u
#define L2CFG_64K	2		/* L2MODE: 64 KiB cache at the top of L2 */

#endif
