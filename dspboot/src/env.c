/* SPDX-License-Identifier: BSD-2-Clause */
/* Read-only U-Boot environment parsing (see env.h). */
#include "crc.h"
#include "env.h"

void env_check(const u8 *buf, u32 size, int redundant, struct env_copy *out)
{
	u32 hdr = redundant ? 5 : 4;
	u32 crc;

	out->valid = 0;
	out->flags = 0;
	out->data = buf;
	out->len = 0;
	if (size <= hdr) {
		return;
	}

	out->flags = redundant ? buf[4] : 0;
	out->data = buf + hdr;
	out->len = size - hdr;
	crc = buf[0] | buf[1] << 8 | buf[2] << 16 | (u32)buf[3] << 24;
	out->valid = crc32_update(0, out->data, out->len) == crc;
}

/*
 * Same rule as U-Boot 2009.11 env_nand.c: the larger flag wins, except across
 * the 255 -> 0 wrap.
 */
int env_pick(const struct env_copy *a, const struct env_copy *b)
{
	if (!a->valid && !b->valid) {
		return -1;
	}

	if (!b->valid) {
		return 0;
	}

	if (!a->valid) {
		return 1;
	}

	if (a->flags == 255 && b->flags == 0) {
		return 1;
	}

	if (b->flags == 255 && a->flags == 0) {
		return 0;
	}

	return a->flags >= b->flags ? 0 : 1;
}

const char *env_get(const struct env_copy *env, const char *name, u32 *vlen)
{
	const u8 *p = env->data, *end = env->data + env->len;
	u32 nlen = 0;

	if (!env->valid) {
		return NULL;
	}
	while (name[nlen]) {
		nlen++;
	}
	while (p < end && *p) {
		const u8 *e = p;
		u32 i;

		while (e < end && *e) {
			e++;
		}

		for (i = 0; i < nlen && p + i < e && p[i] == (u8)name[i]; i++)
			;
		if (i == nlen && p + i < e && p[i] == '=') {
			if (vlen) {
				*vlen = (u32)(e - (p + i + 1));
			}

			return (const char *)(p + i + 1);
		}

		p = e + 1;
	}

	return NULL;
}

char env_slot(const char *v, u32 vlen)
{
	if (v && vlen == 1 && (v[0] == 'a' || v[0] == 'b')) {
		return v[0];
	}

	return 0;
}
