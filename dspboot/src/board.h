/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Board configuration: the board file picked with "make BOARD=<name>"
 * (boards/<name>.h, passed in as BOARD_FILE), then defaults for whatever it
 * leaves out, values derived from it and build-time checks against the
 * OMAP-L137 / DA830 limits. See boards/template.h for what a board sets.
 */
#ifndef DSPBOOT_BOARD_H
#define DSPBOOT_BOARD_H

#ifndef BOARD_FILE
#error "no board: build with make BOARD=<name> (see boards/)"
#endif
#include BOARD_FILE

/*
 * "v1.0", "v1.0~g4f6ccf8" or "v1.0~g4f6ccf8-dirty", from git describe
 * (Makefile)
 */
#ifndef DSPBOOT_VERSION
#define DSPBOOT_VERSION	"v1.0~unknown"
#endif

/* required */

#if !defined(BOARD_NAME) || !defined(BOARD_OSC_HZ) || !defined(BOARD_PLLM) || \
	!defined(BOARD_CONSOLE_UART) || !defined(BOARD_SDCR) ||               \
	!defined(BOARD_SDTIMR) || !defined(BOARD_SDTIMR2) ||                  \
	!defined(BOARD_SDRCR) || !defined(BOARD_NAND_CE3CFG)
#error "the board file must set BOARD_NAME, BOARD_OSC_HZ, BOARD_PLLM, BOARD_CONSOLE_UART, BOARD_SDCR, BOARD_SDTIMR, BOARD_SDTIMR2, BOARD_SDRCR and BOARD_NAND_CE3CFG"
#endif

/* defaults */

#ifndef BOARD_POSTDIV
#define BOARD_POSTDIV	0
#endif
#ifndef BOARD_PLLDIV3
#define BOARD_PLLDIV3	3
#endif
#ifndef BOARD_PLLDIV5
#define BOARD_PLLDIV5	2
#endif
#ifndef BOARD_PLLDIV7
#define BOARD_PLLDIV7	7
#endif
#ifndef BOARD_EMIFA_DIV4P5
#define BOARD_EMIFA_DIV4P5	0
#endif

#ifndef BOARD_BAUD
#define BOARD_BAUD	115200u
#endif

/* Seconds to wait for a key before booting */
#ifndef BOARD_BOOTDELAY
#define BOARD_BOOTDELAY	1
#endif

/* SDRAM init + quick test attempts, 1 s apart, before giving up */
#ifndef BOARD_SDRAM_TRIES
#define BOARD_SDRAM_TRIES	10
#endif

/*
 * Flash layout, the same on every supported NAND (512 B, 2 KiB and 4 KiB
 * pages), and it has to match the board's U-Boot:
 *
 *   block 0             U-Boot environment
 *   block 1 .. 1 MiB    dspboot (the ROM looks for it from block 1)
 *   0x100000  256 KiB   U-Boot environment, redundant copy
 *   0x140000  512 KiB   U-Boot slot a
 *   0x1C0000  512 KiB   U-Boot slot b
 *   0x240000  ..end     free for the OS (UBI)
 *
 * Everything after block 1 is aligned to 256 KiB, the largest block dspboot
 * accepts, so these offsets are block boundaries on all of them.
 *
 * "bootslot=a|b" in the U-Boot environment picks the slot, the other one is
 * the fallback. Slots only start dspboot images (header and payload CRC).
 */
#ifndef BOARD_ENV_OFS
#define BOARD_ENV_OFS	0x00000000u	/* CONFIG_ENV_OFFSET */
#endif
#ifndef BOARD_ENV_REDUND_OFS
#define BOARD_ENV_REDUND_OFS	0x00100000u	/* CONFIG_ENV_OFFSET_REDUND */
#endif
#ifndef BOARD_ENV_SIZE
#define BOARD_ENV_SIZE	0x00004000u	/* CONFIG_ENV_SIZE */
#endif
#ifndef BOARD_UBOOT_A_OFS
#define BOARD_UBOOT_A_OFS	0x00140000u
#endif
#ifndef BOARD_UBOOT_B_OFS
#define BOARD_UBOOT_B_OFS	0x001C0000u
#endif
#ifndef BOARD_UBOOT_SLOT_LEN
#define BOARD_UBOOT_SLOT_LEN	0x00080000u
#endif

/*
 * SDRAM scratch for the two environment copies (clear of loady at 0xC1000000)
 */
#define BOARD_ENV_SCRATCH	0xC0F00000u
/* Handoff record for U-Boot/Linux in shared RAM, see struct dspboot_handoff */
#define BOARD_HANDOFF_ADDR	0x8001FF00u
/* Default load address for images received over YMODEM */
#define BOARD_UBOOT_ADDR	0xC1080000u

/* derived */

#define BOARD_PLL_HZ \
	(BOARD_OSC_HZ / (BOARD_POSTDIV + 1) * (BOARD_PLLM + 1))
#define BOARD_CPU_HZ		BOARD_PLL_HZ		/* PLLDIV1 fixed /1 */
#define BOARD_SYSCLK2_HZ	(BOARD_CPU_HZ / 2)	/* PLLDIV2 fixed /2 */
#define BOARD_EMIFA_HZ                             \
	(BOARD_EMIFA_DIV4P5 ? BOARD_PLL_HZ / 9 * 2 \
			    : BOARD_PLL_HZ / (BOARD_PLLDIV3 + 1))
#define BOARD_EMIFB_HZ	(BOARD_PLL_HZ / (BOARD_PLLDIV5 + 1))

/* checks against the data sheet */

#if BOARD_OSC_HZ < 12000000u || BOARD_OSC_HZ > 30000000u
#error "BOARD_OSC_HZ: the oscillator takes a 12..30 MHz crystal"
#endif
#if BOARD_PLLM > 31 || BOARD_POSTDIV > 31 || BOARD_PLLDIV3 > 31 || \
	BOARD_PLLDIV5 > 31 || BOARD_PLLDIV7 > 31
#error "PLL0 multiplier/divider fields are 5 bits (0..31)"
#endif
#if BOARD_PLLM < 3
#error "BOARD_PLLM: the PLL multiplies by at least 4 (BOARD_PLLM >= 3)"
#endif
#if BOARD_OSC_HZ / 1000 * (BOARD_PLLM + 1) < 300000u || \
	BOARD_OSC_HZ / 1000 * (BOARD_PLLM + 1) > 600000u
#error "PLL0 output before POSTDIV must be 300..600 MHz"
#endif
#if BOARD_CPU_HZ > 456000000u
#error "DSP/ARM clock above 456 MHz, the fastest OMAP-L137 speed grade"
#endif
#if BOARD_EMIFB_HZ > 133333334u
#error "EMIFB (SDRAM) clock above 133 MHz"
#endif
#if BOARD_EMIFA_HZ > 100000000u
#error "EMIFA clock above 100 MHz"
#endif
#if BOARD_CONSOLE_UART < 0 || BOARD_CONSOLE_UART > 2
#error "BOARD_CONSOLE_UART must be 0, 1 or 2"
#endif
#if BOARD_BOOTDELAY < 0 || BOARD_BOOTDELAY > 9
#error "BOARD_BOOTDELAY: 0..9 seconds"
#endif

#endif
