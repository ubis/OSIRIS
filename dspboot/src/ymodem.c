/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * YMODEM batch receiver (CRC-16 mode, 128- and 1024-byte blocks). Also
 * accepts a plain XMODEM-CRC transfer (first block numbered 1).
 *
 * Protocol per Chuck Forsberg's "XMODEM/YMODEM PROTOCOL REFERENCE" (1988):
 *   rx: 'C' ... tx: block 0 (name\0 size ...)  rx: ACK 'C'
 *   tx: blocks 1..n  rx: ACK each (NAK on error)
 *   tx: EOT rx: NAK  tx: EOT rx: ACK 'C'  tx: empty block 0  rx: ACK
 */
#include "crc.h"
#include "ymodem.h"

#define SOH	0x01
#define STX	0x02
#define EOT	0x04
#define ACK	0x06
#define NAK	0x15
#define CAN	0x18
#define CTRLC	0x03

#define CHAR_TIMEOUT_MS	1000
#define MAX_ERRORS	16

static u8 blkbuf[1024 + 4];

static void purge(const struct ym_io *io)
{
	while (io->getc(io->ctx, 100) >= 0)
		;
}

static void cancel(const struct ym_io *io)
{
	int i;

	for (i = 0; i < 4; i++) {
		io->putc(io->ctx, CAN);
	}
}

static u32 parse_dec(const u8 *p, const u8 *end)
{
	u32 v = 0;

	while (p < end && *p >= '0' && *p <= '9') {
		v = v * 10 + (*p++ - '0');
	}

	return v;
}

/*
 * Receive one block after its SOH/STX. Returns block number (0..255),
 * -1 on timeout/framing error, -2 on CRC error.
 */
static int recv_block(const struct ym_io *io, u32 len)
{
	u32 i;
	int c, blk, nblk;
	u16 crc;

	blk = io->getc(io->ctx, CHAR_TIMEOUT_MS);
	nblk = io->getc(io->ctx, CHAR_TIMEOUT_MS);
	if (blk < 0 || nblk < 0) {
		return -ETIMEDOUT;
	}

	for (i = 0; i < len + 2; i++) {
		c = io->getc(io->ctx, CHAR_TIMEOUT_MS);
		if (c < 0) {
			return -ETIMEDOUT;
		}

		blkbuf[i] = (u8)c;
	}

	if ((blk ^ nblk) != 0xFF) {
		return -EPROTO;
	}

	crc = crc16_xmodem(blkbuf, len);
	if (blkbuf[len] != (crc >> 8) || blkbuf[len + 1] != (crc & 0xFF)) {
		return -EBADMSG;
	}

	return blk;
}

int ymodem_receive(const struct ym_io *io, u8 *dst, u32 max,
		   struct ym_result *res)
{
	u32 ofs = 0, len, i, waited = 0;
	/* next block number expected (0 = YMODEM header) */
	int expect = 0;
	int started = 0, errors = 0, eots = 0, can = 0, c, blk;

	res->len = res->file_size = res->blocks = res->retries = 0;
	res->name[0] = 0;

	io->putc(io->ctx, 'C');
	for (;;) {
		c = io->getc(io->ctx, CHAR_TIMEOUT_MS);
		if (c < 0) {
			if (!started) {
				if (++waited >= io->start_timeout_s) {
					return -ETIMEDOUT;
				}

				io->putc(io->ctx, 'C');
				continue;
			}

			if (eots && expect == 0) {
				/*
				 * XMODEM-style senders send no batch-end block
				 * 0
				 */
				if (++errors >= 3) {
					break;
				}

				io->putc(io->ctx, 'C');
				continue;
			}

			if (++errors > MAX_ERRORS) {
				cancel(io);
				return -EPROTO;
			}

			res->retries++;
			io->putc(io->ctx, expect == 0 && !eots ? 'C' : NAK);
			continue;
		}

		if (c == CAN) {
			if (++can >= 2) {
				return -ECANCELED;
			}

			continue;
		}

		can = 0;
		if (c == CTRLC && !started) {
			return -ECANCELED;
		}

		if (c == EOT && started) {
			if (eots++ == 0) {
				/* make the sender confirm the EOT */
				io->putc(io->ctx, NAK);
				continue;
			}

			io->putc(io->ctx, ACK);
			errors = 0;
			/* YMODEM: next is the batch-end block 0 */
			expect = 0;
			io->putc(io->ctx, 'C');
			continue;
		}

		if (c != SOH && c != STX) {
			/* line noise / console echo */
			continue;
		}

		len = (c == STX) ? 1024 : 128;
		blk = recv_block(io, len);
		if (blk < 0) {
			purge(io);
			if (++errors > MAX_ERRORS) {
				cancel(io);
				return -EPROTO;
			}

			res->retries++;
			io->putc(io->ctx, NAK);
			continue;
		}

		if (eots) {
			/*
			 * After EOT only the (empty) batch terminator block 0
			 * is expected
			 */
			io->putc(io->ctx, ACK);
			if (blk == 0 && blkbuf[0] == 0) {
				break;
			}

			/* Another file in the batch: we only take one */
			cancel(io);
			break;
		}

		if (!started && blk == 0 && expect == 0) {
			/* YMODEM header block: "name\0size [mtime mode ...]" */
			const u8 *end = blkbuf + len;
			const u8 *p = blkbuf;
			started = 1;
			if (blkbuf[0] == 0) {
				io->putc(io->ctx, ACK);
				return -ENOENT;
			}

			for (i = 0; i < sizeof(res->name) - 1 && p < end && *p;
			     i++) {
				res->name[i] = (char)*p++;
			}

			res->name[i] = 0;
			while (p < end && *p) {
				p++;
			}

			if (p < end) {
				res->file_size = parse_dec(p + 1, end);
			}

			if (res->file_size > max) {
				cancel(io);
				return -EFBIG;
			}

			io->putc(io->ctx, ACK);
			io->putc(io->ctx, 'C');
			expect = 1;
			errors = 0;
			continue;
		}

		if (!started && blk == 1) {
			started = 1; /* XMODEM-CRC sender */
			expect = 1;
		}

		if (blk == (expect & 0xFF)) {
			if (ofs + len > max) {
				/*
				 * the last block may be padding beyond the real
				 * file size
				 */
				if (!(res->file_size && ofs < res->file_size &&
				      res->file_size <= max)) {
					cancel(io);
					return -EFBIG;
				}

				len = max - ofs;
			}

			for (i = 0; i < len; i++) {
				dst[ofs + i] = blkbuf[i];
			}

			ofs += len;
			expect++;
			res->blocks++;
			errors = 0;
			io->putc(io->ctx, ACK);
		} else if (blk == ((expect - 1) & 0xFF)) {
			/* duplicate (our ACK was lost) */
			io->putc(io->ctx, ACK);
		} else {
			cancel(io);
			return -EPROTO;
		}
	}

	res->len = (res->file_size && res->file_size <= ofs) ? res->file_size
							     : ofs;
	return 0;
}

const char *ymodem_strerror(int rc)
{
	switch (rc) {
	case 0: return "ok";
	case -ETIMEDOUT: return "timeout waiting for sender";
	case -ECANCELED: return "cancelled";
	case -EFBIG: return "file too large for destination";
	case -EPROTO: return "protocol error / too many retries";
	case -ENOENT: return "sender sent no file";
	default: return "?";
	}
}
