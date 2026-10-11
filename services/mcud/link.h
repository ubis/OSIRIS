/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MCUD_LINK_H
#define MCUD_LINK_H

#include <stdint.h>

/* every SPI transfer is this long; a frame is 6 bytes plus its data */
#define LINK_XFER_LEN	1032
#define LINK_MAX_DATA	(LINK_XFER_LEN - 6)

struct link_frame {
	uint8_t cmd;
	uint16_t cnt;
	uint16_t len;
	const uint8_t *data;
	int csum_ok;
};

typedef void (*link_rx_fn)(const struct link_frame *f);

/* log every frame with a timestamp on stderr */
extern int link_trace;

/* Open SPI and the GPIOs and start polling; 'rx' gets every frame from the MCU. */
int link_open(uint32_t spi_hz, link_rx_fn rx);
/* Queue a frame for the MCU; -ENOSPC when the queue is full. */
int link_send(uint8_t cmd, const void *data, uint16_t len);
unsigned int link_queued(void);
void link_close(void);

#endif
