/* SPDX-License-Identifier: BSD-2-Clause */
/* CRC helpers shared by the target and the host-side tests. */
#ifndef DSPBOOT_CRC_H
#define DSPBOOT_CRC_H

#include "types.h"

/* CRC-32/IEEE 802.3 as used by zlib/Python zlib.crc32(); pass 0 to start. */
u32 crc32_update(u32 crc, const void *buf, u32 len);

/*
 * CRC-16/XMODEM (poly 0x1021, init 0, no reflection), as used by X/YMODEM-CRC.
 */
u16 crc16_xmodem(const void *buf, u32 len);

#endif
