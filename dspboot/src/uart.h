/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef DSPBOOT_UART_H
#define DSPBOOT_UART_H

#include "types.h"

/*
 * Select and program UART n (0..2) for 8N1 at 'baud' from 'clk_hz'. Returns the
 * divisor.
 */
u32 uart_init(int n, u32 clk_hz, u32 baud);
int uart_index(void);
/* wait until the transmitter is completely idle */
void uart_flush(void);
u32 uart_divisor(void);
u32 uart_actual_baud(void);
/* count of overrun/framing/parity errors seen */
u32 uart_errors(void);

#endif
