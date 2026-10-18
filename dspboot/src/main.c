/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * dspboot - first-stage boot loader for the OMAP-L137 / DA830, running on the
 * C674x DSP (the boot master).
 */
#include "board.h"
#include "boot.h"
#include "hw.h"
#include "lib.h"
#include "nand.h"
#include "sdram.h"
#include "shell.h"
#include "soc.h"
#include "timer.h"
#include "uart.h"

extern u8 __bss_start[], __bss_end[];

static u32 mhz(u32 hz)
{
	return (hz + 500000) / 1000000;
}

/*
 * Booted over a UART: keep talking on it; otherwise the board's console UART.
 */
static int console_init(int pll_ok)
{
	enum bootmode bm = soc_bootmode();
	int un = bm == BM_UART0	  ? 0
		 : bm == BM_UART1 ? 1
		 : bm == BM_UART2 ? 2
				  : BOARD_CONSOLE_UART;
	int rc;

	if (un == 0) {
		rc = psc_set(0, LPSC0_UART0, PSC_STATE_ENABLE);
	} else {
		rc = psc_set(1, un == 1 ? LPSC1_UART1 : LPSC1_UART2,
			     PSC_STATE_ENABLE);
	}

	pinmux_uart(un);
	uart_init(un, pll_ok ? BOARD_SYSCLK2_HZ : BOARD_OSC_HZ, BOARD_BAUD);
	return rc;
}

static void banner(int pll_ok, int psc_ok)
{
	struct soc_clocks clk;

	soc_clocks(&clk);
	printf("\n\ndspboot %s (%s - %s)\n\n", DSPBOOT_VERSION, __DATE__,
	       __TIME__);
	printf("SoC:    %s rev %s, ROM %s\n", soc_name(), soc_revision(),
	       soc_rom_id());
	printf("Board:  %s\n", BOARD_NAME);
	printf("Boot:   %s\n", soc_bootmode_name(soc_bootmode()));
	if (pll_ok) {
		printf("Clocks: DSP/ARM %u MHz, SDRAM %u MHz, EMIFA %u MHz (%u MHz crystal)\n",
		       mhz(clk.cpu), mhz(clk.emifb), mhz(clk.emifa),
		       mhz(clk.osc));
	} else {
		printf("Clocks: PLL0 did not lock, running from the %u MHz crystal\n",
		       mhz(clk.osc));
	}

	if (!psc_ok) {
		printf("Power:  PSC timeout, some on-chip modules may still be off\n");
	}
}

/* U-Boot style countdown; 1 if a key stopped it */
static int autoboot_stop(void)
{
	int left = BOARD_BOOTDELAY, i;

	printf("Hit any key to stop autoboot: %2d", left);
	for (;;) {
		for (i = 0; i < 100; i++) {
			if (con_getc_timeout(10) >= 0) {
				printf("\b\b\b 0\n");
				return 1;
			}

			if (!left) {
				break;
			}
		}

		if (left <= 0) {
			break;
		}

		left--;
		printf("\b\b\b%2d ", left);
	}

	printf("\n");
	return 0;
}

void dspboot_main(void)
{
	int pll_ok, psc_ok, sdram_ok;

	memset(__bss_start, 0, __bss_end - __bss_start);
	soc_unlock();
	timer_init();
	soc_init_cache();

	psc_ok = !psc_enable_all();
	pll_ok = !pll0_init();
	psc_ok &= !console_init(pll_ok);
	banner(pll_ok, psc_ok);

	sdram_ok = !sdram_bringup();
	nand_init();
	nand_summary();
	if (!sdram_ok) {
		printf("SDRAM is not usable - staying in the console.\n");
		shell_run();
	}

	if (autoboot_stop()) {
		shell_run();
	}

	boot_slots();
	printf("No bootable U-Boot in NAND, waiting for one over YMODEM.\n");
	if (!boot_loady(BOARD_UBOOT_ADDR)) {
		boot_ram(BOARD_UBOOT_ADDR, 0);
	}

	shell_run();
}
