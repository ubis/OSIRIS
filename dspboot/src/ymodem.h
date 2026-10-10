/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * YMODEM (and XMODEM-CRC) receiver. Hardware independent: the caller provides
 * byte I/O callbacks, so the same code is unit-tested on the build host.
 */
#ifndef DSPBOOT_YMODEM_H
#define DSPBOOT_YMODEM_H

#include "errcodes.h"
#include "types.h"

struct ym_io {
	int (*getc)(void *ctx, u32 timeout_ms); /* byte, or -1 on timeout */
	void (*putc)(void *ctx, int c);
	void *ctx;
	/* how long to keep sending 'C' */
	u32 start_timeout_s;
};

struct ym_result {
	/* bytes stored (trimmed to the YMODEM file size if known) */
	u32 len;
	u32 file_size; /* from block 0, 0 if unknown */
	u32 blocks;
	u32 retries;
	char name[64];
};

/*
 * 0 on success, else -ETIMEDOUT (sender never started), -ECANCELED (CAN CAN
 * from the sender or ^C), -EFBIG (file too large), -EPROTO (too many errors,
 * sequence error) or -ENOENT (batch ended without a file).
 */
int ymodem_receive(const struct ym_io *io, u8 *dst, u32 max,
		   struct ym_result *res);
const char *ymodem_strerror(int rc);

#endif
