/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Second-stage image formats understood by dspboot.
 *
 * 1. dspboot image ("DBI1"), created by tools/mkimage.py:
 *      0x00 magic   0x31494244 ("DBI1" in memory)
 *      0x04 hdr_size 64
 *      0x08 size    payload bytes (payload starts at offset hdr_size)
 *      0x0C load    load address (SDRAM)
 *      0x10 entry   ARM entry point
 *      0x14 dcrc    CRC-32 (zlib) of the payload
 *      0x18 flags   0
 *      0x1C name    32 bytes, NUL padded
 *      0x3C hcrc    CRC-32 (zlib) of header bytes 0x00..0x3B
 *    all little endian.
 *
 * 2. TI UBL NAND header (stretch goal, not tested): magic 0xA1ACEDxx,
 *    entry, numPage, block, page, ldAddress; data at (block, page).
 */
#ifndef DSPBOOT_IMAGE_H
#define DSPBOOT_IMAGE_H

#include "types.h"

#define DBI_MAGIC	0x31494244u
#define DBI_HDR_SIZE	64u
#define TIUBL_MAGIC	0xA1ACED00u
#define UIMAGE_MAGIC	0x27051956u	/* U-Boot legacy image, big endian */
#define TIUBL_MASK	0xFFFFFF00u

enum img_type { IMG_NONE, IMG_DBI, IMG_TIUBL, IMG_UIMAGE, IMG_RAW_ARM };

struct img_info {
	enum img_type type;
	u32 hdr_size; /* payload offset from header start */
	u32 size;
	u32 load;
	u32 entry;
	u32 dcrc;
	int has_crc;
	u32 ubl_block, ubl_page, ubl_pages; /* TI UBL only */
	char name[33];
};

/*
 * Inspect 'buf' (at least 64 bytes available). Returns 0 and fills 'out' if a
 * bootable header is found and its fields are sane, else -ENOEXEC (nothing
 * dspboot can start; out->type says IMG_UIMAGE for a uImage), -EBADMSG
 * (header CRC wrong) or -ERANGE (bad addresses / size).
 * 'mem_lo'/'mem_hi' bound where images may be loaded.
 */
int img_parse(const u8 *buf, u32 mem_lo, u32 mem_hi, struct img_info *out);
const char *img_type_name(enum img_type t);
/* Heuristic: does this look like the start of ARM code (vector table)? */
int img_looks_like_arm(const u8 *buf);

#endif
