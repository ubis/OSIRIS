/* SPDX-License-Identifier: BSD-2-Clause */
#include "crc.h"

/* 4-bit table driven CRC-32 (reflected poly 0xEDB88320): 64 bytes of table. */
static const u32 crc32_nibble[16] = {
	0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
	0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
	0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

u32 crc32_update(u32 crc, const void *buf, u32 len)
{
	const u8 *p = buf;

	crc = ~crc;
	while (len--) {
		crc ^= *p++;
		crc = (crc >> 4) ^ crc32_nibble[crc & 15];
		crc = (crc >> 4) ^ crc32_nibble[crc & 15];
	}

	return ~crc;
}

u16 crc16_xmodem(const void *buf, u32 len)
{
	const u8 *p = buf;
	u32 crc = 0;
	int i;

	while (len--) {
		crc ^= (u32)*p++ << 8;
		for (i = 0; i < 8; i++) {
			crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
		}
	}

	return (u16)crc;
}
