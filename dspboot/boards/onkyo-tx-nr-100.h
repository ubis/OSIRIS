/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * onkyo-tx-nr with the SDRAM at 100 MHz instead of 133 MHz, for boards whose
 * SDRAM reads fail at 133 MHz (the low half of a word returns the next
 * word's). The SDRAM timings are the vendor's 133 MHz ones, which leave more
 * margin at 100 MHz; only the clock divider and the refresh rate change.
 */
#define BOARD_NAME	"onkyo-tx-nr-100"

#define BOARD_OSC_HZ	20000000u

/* 20 MHz x20 = 400 MHz: DSP/ARM 400, EMIFB 133.3, RMII 50 MHz */
#define BOARD_PLLM		19
#define BOARD_POSTDIV		0
#define BOARD_PLLDIV3		3
#define BOARD_PLLDIV5		3	/* EMIFB = 400 / 4 = 100 MHz */
#define BOARD_PLLDIV7		7
#define BOARD_EMIFA_DIV4P5	1	/* EMIFA = PLL0 / 4.5 = 88.9 MHz */

#define BOARD_CONSOLE_UART	1

/* 32-bit, CAS 3, 4 banks, 512-word pages; 7.65 us refresh at 133.3 MHz */
#define BOARD_SDCR	0x00018621u
#define BOARD_SDTIMR	0x10913208u
#define BOARD_SDTIMR2	0x68090006u
#define BOARD_SDRCR	0x000002FDu	/* 7.65 us at 100 MHz */

#define BOARD_NAND_CE3CFG	0x3FFFFFFCu /* slowest async timing, 8-bit */
