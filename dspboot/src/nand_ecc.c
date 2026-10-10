/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ROM-compatible 4-bit ECC layout helpers (kept free of hardware access so
 * the host test can check them against real NAND dumps).
 *
 * Layout used by the OMAP-L137/DA830 boot ROM, TI's NAND writer and, as
 * verified against raw dumps of a production board, vendor first and second
 * stages: per 512-byte page, OOB bytes 0..5 are 0xFF (byte 5
 * is the bad-block marker) and OOB bytes 6..15 hold eight 10-bit Reed-Solomon
 * parity symbols packed little-endian (symbol i = bits 10*i .. 10*i+9).
 */
#include "nand.h"

void nand_ecc_unpack(const u8 *b, u16 *out)
{
	int i, bit;

	for (i = 0, bit = 0; i < 8; i++, bit += 10) {
		u32 byte = bit >> 3, sh = bit & 7;
		u32 v = (u32)b[byte] | ((u32)b[byte + 1] << 8);
		out[i] = (u16)((v >> sh) & 0x3FF);
	}
}
