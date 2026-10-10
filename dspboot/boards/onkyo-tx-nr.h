/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Onkyo TX-NR515 / TX-NR717 (and their Integra twins) main board: DA830 or
 * OMAP-L137, 20 MHz crystal, 64 MiB SDR SDRAM on a 32-bit bus, console on
 * UART1. Clock and memory values are the ones the vendor's first stage feeds
 * the boot ROM (its AIS: PLL0 config 0x13800302 0x000701EC, EMIFB config
 * 0x00018621 0x10913208 0x68090006 0x000003FC, CE3CFG 0x3FFFFFFC).
 */
#define BOARD_NAME	"onkyo-tx-nr"

#define BOARD_OSC_HZ	20000000u

/* 20 MHz x20 = 400 MHz: DSP/ARM 400, EMIFB 133.3, RMII 50 MHz */
#define BOARD_PLLM		19
#define BOARD_POSTDIV		0
#define BOARD_PLLDIV3		3
#define BOARD_PLLDIV5		2
#define BOARD_PLLDIV7		7
#define BOARD_EMIFA_DIV4P5	1	/* EMIFA = PLL0 / 4.5 = 88.9 MHz */

#define BOARD_CONSOLE_UART	1

/* 32-bit, CAS 3, 4 banks, 512-word pages; 7.65 us refresh at 133.3 MHz */
#define BOARD_SDCR	0x00018621u
#define BOARD_SDTIMR	0x10913208u
#define BOARD_SDTIMR2	0x68090006u
#define BOARD_SDRCR	0x000003FCu

#define BOARD_NAND_CE3CFG	0x3FFFFFFCu /* slowest async timing, 8-bit */
