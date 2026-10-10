/* SPDX-License-Identifier: BSD-2-Clause */
/* Polled 16550-style UART driver for the console. */
#include "hw.h"
#include "lib.h"
#include "timer.h"
#include "uart.h"

static const u32 uart_bases[3] = { UART0_BASE, UART1_BASE, UART2_BASE };
static u32 ubase = UART1_BASE;
static int uidx = 1;
static u32 uerr;
static u32 udiv, uclk;

#define UREG(o)	REG32(ubase + (o))

u32 uart_init(int n, u32 clk_hz, u32 baud)
{
	/* 16x oversampling; round to nearest */
	u32 div = (clk_hz + 8 * baud) / (16 * baud);

	uidx = n;
	ubase = uart_bases[n];
	udiv = div;
	uclk = clk_hz;

	UREG(UART_PWREMU) = 0; /* hold TX and RX in reset */
	UREG(UART_MDR) = 0; /* 16x oversampling */
	UREG(UART_IER) = 0;
	UREG(UART_LCR) = 0x83; /* DLAB=1, 8N1 */
	UREG(UART_DLL) = div & 0xFF;
	UREG(UART_DLH) = (div >> 8) & 0xFF;
	UREG(UART_LCR) = 0x03; /* DLAB=0, 8N1 */
	/* FIFO on, clear RX and TX FIFOs */
	UREG(UART_FCR) = 0x07;
	UREG(UART_MCR) = 0;
	UREG(UART_PWREMU) = PWREMU_UTRST | PWREMU_URRST | PWREMU_FREE;
	return div;
}

int uart_index(void)
{
	return uidx;
}

u32 uart_divisor(void)
{
	return udiv;
}

u32 uart_actual_baud(void)
{
	return udiv ? uclk / (16 * udiv) : 0;
}

void con_putc(int c)
{
	while (!(UREG(UART_LSR) & LSR_THRE))
		;
	UREG(UART_THR) = (u8)c;
}

void uart_flush(void)
{
	u32 t0 = timer_ms();

	while (!(UREG(UART_LSR) & LSR_TEMT)) {
		if (timer_ms() - t0 > 100) {
			break;
		}
	}
}

int con_tstc(void)
{
	return (UREG(UART_LSR) & LSR_DR) != 0;
}

u32 uart_errors(void)
{
	return uerr;
}

int con_getc_timeout(u32 timeout_ms)
{
	u32 t0 = timer_ms();

	for (;;) {
		u32 lsr = UREG(UART_LSR);
		if (lsr & (LSR_OE | LSR_PE | LSR_FE | LSR_BI)) {
			uerr++;
		}

		if (lsr & LSR_DR) {
			return UREG(UART_RBR) & 0xFF;
		}

		if (timer_ms() - t0 >= timeout_ms) {
			return -1;
		}
	}
}
