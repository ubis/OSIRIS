/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * dspboot - register map for the TI OMAP-L137 / TMS320DA830 (C674x side).
 *
 * Addresses and bit fields follow the DA830/OMAP-L137 TRM and data sheet and
 * were cross-checked against TI's OMAP-L137 FlashAndBootUtils 2.40
 * (device.h, BSD-3-Clause, Copyright (C) 2012 Texas Instruments) and the
 * Linux mach-davinci da830 pinmux table.
 */
#ifndef DSPBOOT_HW_H
#define DSPBOOT_HW_H

#include "types.h"

#define REG32(a)	(*(volatile u32 *)(a))
#define REG8(a)		(*(volatile u8 *)(a))

/* SYSCFG (System configuration module 0) */
#define SYSCFG_BASE		0x01C14000u
#define SYSCFG_REVID		REG32(SYSCFG_BASE + 0x000)
#define SYSCFG_DIEIDR(n)	REG32(SYSCFG_BASE + 0x008 + 4 * (n))
#define SYSCFG_JTAGID		REG32(SYSCFG_BASE + 0x018)
#define SYSCFG_BOOTCFG		REG32(SYSCFG_BASE + 0x020)
#define SYSCFG_CHIPREVID	REG32(SYSCFG_BASE + 0x024)
#define SYSCFG_KICK0R		REG32(SYSCFG_BASE + 0x038)
#define SYSCFG_KICK1R		REG32(SYSCFG_BASE + 0x03C)
#define SYSCFG_HOST0CFG		REG32(SYSCFG_BASE + 0x040)	/* ARM boot */
#define SYSCFG_HOST1CFG		REG32(SYSCFG_BASE + 0x044)	/* DSP boot */
#define SYSCFG_PINMUX(n)	REG32(SYSCFG_BASE + 0x120 + 4 * (n))
#define SYSCFG_CFGCHIP(n)	REG32(SYSCFG_BASE + 0x17C + 4 * (n))

#define KICK0_UNLOCK	0x83E70B13u
#define KICK1_UNLOCK	0x95A4F1E0u

#define CFGCHIP3_EMA_CLKSRC	(1u << 1) /* EMIFA clock = PLL0 /4.5 output */
#define CFGCHIP3_DIV4P5ENA	(1u << 2) /* enable the /4.5 divider */

/* JTAG ID: variant[31:28] part[27:12] manufacturer[11:1] */
#define JTAGID_PART_DA830	0xB7DFu

/* PLL controller 0 */
#define PLL0_BASE	0x01C11000u
#define PLL0_PLLCTL	REG32(PLL0_BASE + 0x100)
#define PLL0_PLLM	REG32(PLL0_BASE + 0x110)
#define PLL0_PREDIV	REG32(PLL0_BASE + 0x114)
#define PLL0_PLLDIV1	REG32(PLL0_BASE + 0x118)
#define PLL0_PLLDIV2	REG32(PLL0_BASE + 0x11C)
#define PLL0_PLLDIV3	REG32(PLL0_BASE + 0x120)
#define PLL0_POSTDIV	REG32(PLL0_BASE + 0x128)
#define PLL0_PLLCMD	REG32(PLL0_BASE + 0x138)
#define PLL0_PLLSTAT	REG32(PLL0_BASE + 0x13C)
#define PLL0_PLLDIV4	REG32(PLL0_BASE + 0x160)
#define PLL0_PLLDIV5	REG32(PLL0_BASE + 0x164)
#define PLL0_PLLDIV6	REG32(PLL0_BASE + 0x168)
#define PLL0_PLLDIV7	REG32(PLL0_BASE + 0x16C)

#define PLLCTL_PLLEN		(1u << 0)
#define PLLCTL_PLLPWRDN		(1u << 1)
#define PLLCTL_PLLRST		(1u << 3) /* 0 = PLL in reset */
#define PLLCTL_PLLDIS		(1u << 4)
#define PLLCTL_PLLENSRC		(1u << 5)
#define PLLCTL_CLKMODE		(1u << 8) /* 0 = crystal, 1 = square wave */
#define PLLCTL_EXTCLKSRC	(1u << 9)
#define PLLDIV_EN		(1u << 15)
#define PLLSTAT_GOSTAT		(1u << 0)
#define PLLCMD_GOSET		(1u << 0)

/* PSC0 / PSC1 */
#define PSC0_BASE		0x01C10000u
#define PSC1_BASE		0x01E27000u
#define PSC_PTCMD(b)		REG32((b) + 0x120)
#define PSC_PTSTAT(b)		REG32((b) + 0x128)
#define PSC_MDSTAT(b, m)	REG32((b) + 0x800 + 4 * (m))
#define PSC_MDCTL(b, m)		REG32((b) + 0xA00 + 4 * (m))

#define PSC_STATE_SWRSTDISABLE	0x0
#define PSC_STATE_SYNCRESET	0x1
#define PSC_STATE_DISABLE	0x2
#define PSC_STATE_ENABLE	0x3
#define PSC_MDCTL_LRSTZ		(1u << 8)

/* PSC0 modules */
#define LPSC0_TPCC	0
#define LPSC0_EMIFA	3
#define LPSC0_UART0	9
#define LPSC0_ARMRAM	7
#define LPSC0_PRU	13
#define LPSC0_ARM	14
#define LPSC0_DSP	15
/* PSC1 modules */
#define LPSC1_EMIFB	6
#define LPSC1_UART1	12
#define LPSC1_UART2	13

/* UART (16550 compatible) */
#define UART0_BASE	0x01C42000u
#define UART1_BASE	0x01D0C000u
#define UART2_BASE	0x01D0D000u
#define UART_RBR	0x00
#define UART_THR	0x00
#define UART_IER	0x04
#define UART_FCR	0x08
#define UART_LCR	0x0C
#define UART_MCR	0x10
#define UART_LSR	0x14
#define UART_DLL	0x20
#define UART_DLH	0x24
#define UART_PWREMU	0x30
#define UART_MDR	0x34

#define LSR_DR		(1u << 0)
#define LSR_OE		(1u << 1)
#define LSR_PE		(1u << 2)
#define LSR_FE		(1u << 3)
#define LSR_BI		(1u << 4)
#define LSR_THRE	(1u << 5)
#define LSR_TEMT	(1u << 6)
#define PWREMU_FREE	(1u << 0)
#define PWREMU_URRST	(1u << 13)
#define PWREMU_UTRST	(1u << 14)

/* EMIFA (async: NAND on CS3) */
#define EMIFA_BASE		0x68000000u
#define EMIFA_AWCC		REG32(EMIFA_BASE + 0x04)
#define EMIFA_CE2CFG		REG32(EMIFA_BASE + 0x10)
#define EMIFA_CE3CFG		REG32(EMIFA_BASE + 0x14)
#define EMIFA_NANDFCR		REG32(EMIFA_BASE + 0x60)
#define EMIFA_NANDFSR		REG32(EMIFA_BASE + 0x64)
#define EMIFA_NAND4BITECCLOAD	REG32(EMIFA_BASE + 0xBC)
#define EMIFA_NAND4BITECC(n)	REG32(EMIFA_BASE + 0xC0 + 4 * (n))
#define EMIFA_NANDERRADD1	REG32(EMIFA_BASE + 0xD0)
#define EMIFA_NANDERRADD2	REG32(EMIFA_BASE + 0xD4)
#define EMIFA_NANDERRVAL1	REG32(EMIFA_BASE + 0xD8)
#define EMIFA_NANDERRVAL2	REG32(EMIFA_BASE + 0xDC)

#define NANDFCR_4BITECC_SEL_SHIFT	4
#define NANDFCR_4BITECC_START		(1u << 12)
#define NANDFCR_4BITECC_ADD_CALC	(1u << 13)
#define NANDFSR_READY			(1u << 0)

#define NAND_CS		3		/* nEMA_CS[3] */
#define NAND_BASE	0x62000000u	/* CS3 data window */
#define NAND_ALE_OFS	0x08u		/* EMA_A[1] */
#define NAND_CLE_OFS	0x10u		/* EMA_A[2] */

/* EMIFB (SDR SDRAM) */
#define EMIFB_BASE	0xB0000000u
#define EMIFB_SDCR	REG32(EMIFB_BASE + 0x08)
#define EMIFB_SDRCR	REG32(EMIFB_BASE + 0x0C)
#define EMIFB_SDTIMR	REG32(EMIFB_BASE + 0x10)
#define EMIFB_SDTIMR2	REG32(EMIFB_BASE + 0x14)
#define SDCR_TIMUNLOCK	(1u << 15)	/* SDTIMR/SDTIMR2 writable */

#define SDRAM_BASE	0xC0000000u
#define SDRAM_MAX	0x10000000u	/* EMIFB window: 256 MiB */

/* PRU0 (a.k.a. dMAX PDSP on early docs) */
#define PRU0_CTRL_BASE		0x01C37000u
#define PRU0_CONTROL		REG32(PRU0_CTRL_BASE + 0x00)
#define PRU0_STATUS		REG32(PRU0_CTRL_BASE + 0x04)
#define PRU0_IRAM_BASE		0x01C38000u
#define PRU_CONTROL_ENABLE	(1u << 1)
#define PRU_CONTROL_COUNTEN	(1u << 3)

/* Timer64P1 (watchdog, used for "reset") */
#define TIMER1_BASE	0x01C21000u
#define TMR_TIM12	REG32(TIMER1_BASE + 0x10)
#define TMR_TIM34	REG32(TIMER1_BASE + 0x14)
#define TMR_PRD12	REG32(TIMER1_BASE + 0x18)
#define TMR_PRD34	REG32(TIMER1_BASE + 0x1C)
#define TMR_TCR		REG32(TIMER1_BASE + 0x20)
#define TMR_TGCR	REG32(TIMER1_BASE + 0x24)
#define TMR_WDTCR	REG32(TIMER1_BASE + 0x28)

/* C674x megamodule cache control */
#define CACHE_L2WB	REG32(0x01845000u)
#define CACHE_L2WBINV	REG32(0x01845004u)
#define CACHE_L1DWB	REG32(0x01845040u)
#define CACHE_L1DWBINV	REG32(0x01845044u)
#define CACHE_MAR(n)	REG32(0x01848000u + 4 * (n))	/* n = addr >> 24 */

#endif
