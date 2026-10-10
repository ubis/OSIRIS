/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Read-only access to a U-Boot environment, as U-Boot stores it:
 *   single copy:     [u32 crc32][data ...]
 *   redundant copy:  [u32 crc32][u8 flags][data ...]
 * crc32 is zlib CRC-32 over data (the rest of the area); data is a list of
 * "name=value\0" entries ending with an empty string.
 */
#ifndef DSPBOOT_ENV_H
#define DSPBOOT_ENV_H

#include "types.h"

struct env_copy {
	int valid; /* CRC matched */
	/* redundant copies: the newer copy has the larger flag */
	u8 flags;
	const u8 *data;
	u32 len;
};

/*
 * Check one copy of 'size' bytes; 'redundant' selects the format with a flag
 * byte.
 */
void env_check(const u8 *buf, u32 size, int redundant, struct env_copy *out);
/*
 * U-Boot's choice between two redundant copies: 0 (a), 1 (b) or -1 (neither
 * valid).
 */
int env_pick(const struct env_copy *a, const struct env_copy *b);
/* Value of 'name' or NULL; *vlen gets its length. */
const char *env_get(const struct env_copy *env, const char *name, u32 *vlen);
/* "a" or "b" -> 'a' / 'b'; anything else -> 0 */
char env_slot(const char *v, u32 vlen);

#endif
