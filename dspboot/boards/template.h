/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Board template: copy to boards/<vendor>-<board>.h, fill in, then
 *   make dspboot BOARD=<vendor>-<board>
 *
 * Everything a board does not set falls back to the defaults in src/board.h.
 * The values a board has to get right are the ones its stock first stage
 * hands the boot ROM: dump block 1 of the stock NAND and decode its AIS
 * (tools/aisdump.py); the PLL0 and EMIFB "function execute" calls and the
 * SET of the EMIFA CS3 config register carry them.
 */
#define BOARD_NAME	"vendor-board"

/* Crystal on OSCIN */
#define BOARD_OSC_HZ	24000000u

/*
 * PLL0. Register RATIO values: the hardware divides/multiplies by value + 1.
 *   PLL     = OSC * (PLLM + 1) / (POSTDIV + 1)
 *   DSP/ARM = PLL               (PLLDIV1 and PLLDIV6 are fixed /1)
 *   SYSCLK2 = PLL / 2           UARTs, most peripherals (fixed)
 *   EMIFA   = PLL / (PLLDIV3+1), or PLL / 4.5 with BOARD_EMIFA_DIV4P5
 *   EMIFB   = PLL / (PLLDIV5+1) SDRAM clock, at most 133 MHz
 *   RMII    = PLL / (PLLDIV7+1) Ethernet reference, 50 MHz
 * src/board.h refuses values outside the SoC's limits at build time.
 */
#define BOARD_PLLM		24	/* 24 MHz x25 = 600 MHz ... */
#define BOARD_POSTDIV		1	/* ... /2 = 300 MHz */
#define BOARD_PLLDIV3		2	/* EMIFA 100 MHz */
#define BOARD_PLLDIV5		2	/* EMIFB 100 MHz */
#define BOARD_PLLDIV7		5	/* RMII 50 MHz */
#define BOARD_EMIFA_DIV4P5	0

/*
 * Console for NAND boot: 0, 1 or 2. Booted over a UART, dspboot keeps using
 * the UART the ROM booted from.
 */
#define BOARD_CONSOLE_UART	2

/*
 * EMIFB SDRAM controller: SDCR (bus width, CAS, banks, page size), timings,
 * refresh
 */
#define BOARD_SDCR	0x00018621u
#define BOARD_SDTIMR	0x10913208u
#define BOARD_SDTIMR2	0x68090006u
#define BOARD_SDRCR	0x000003FCu

/* EMIFA CS3 async config for the NAND (the ROM boots NAND from CS3 only) */
#define BOARD_NAND_CE3CFG	0x3FFFFFFCu

/*
 * Optional, see src/board.h for the defaults: BOARD_BAUD, BOARD_BOOTDELAY,
 * BOARD_SDRAM_TRIES, and the flash layout (BOARD_ENV_*, BOARD_UBOOT_*),
 * which has to match the board's U-Boot.
 */
