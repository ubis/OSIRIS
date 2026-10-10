/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef DSPBOOT_BOOT_H
#define DSPBOOT_BOOT_H

#include "types.h"

/* The slot "bootslot" names, then the other one. Returns only if both fail. */
void boot_slots(void);
/*
 * One slot ('a' or 'b'), no fallback. Returns only on failure, with the reason.
 */
int boot_slot(char slot);
/*
 * Check and start an image in SDRAM; 'force' starts a headerless one that
 * does not look like ARM code. Returns only on failure, with the reason.
 */
int boot_ram(u32 addr, int force);
/* Receive a file over YMODEM to 'addr'. 0 or a ymodem_receive() error. */
int boot_loady(u32 addr);
u32 boot_last_rx(void);

#endif
