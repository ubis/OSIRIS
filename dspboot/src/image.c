/* SPDX-License-Identifier: BSD-2-Clause */
#include "crc.h"
#include "errcodes.h"
#include "image.h"

static u32 rd32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
	       ((u32)p[3] << 24);
}

static u32 rd32_be(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) |
	       (u32)p[3];
}

static int range_ok(u32 load, u32 size, u32 entry, u32 lo, u32 hi)
{
	if (!size || load < lo || load >= hi || size > hi - load) {
		return 0;
	}

	return entry >= load && entry < load + size;
}

int img_parse(const u8 *buf, u32 lo, u32 hi, struct img_info *o)
{
	u32 magic = rd32(buf);
	int i;

	o->type = IMG_NONE;
	o->has_crc = 0;
	o->name[0] = 0;
	o->ubl_block = o->ubl_page = o->ubl_pages = 0;

	if (magic == DBI_MAGIC) {
		o->type = IMG_DBI;
		if (rd32(buf + 4) != DBI_HDR_SIZE) {
			return -ERANGE;
		}

		if (crc32_update(0, buf, 0x3C) != rd32(buf + 0x3C)) {
			return -EBADMSG;
		}

		o->hdr_size = DBI_HDR_SIZE;
		o->size = rd32(buf + 0x08);
		o->load = rd32(buf + 0x0C);
		o->entry = rd32(buf + 0x10);
		o->dcrc = rd32(buf + 0x14);
		o->has_crc = 1;
		for (i = 0; i < 32 && buf[0x1C + i]; i++) {
			o->name[i] = (char)(buf[0x1C + i] >= 32 &&
							    buf[0x1C + i] < 127
						    ? buf[0x1C + i]
						    : '?');
		}

		o->name[i] = 0;
		return range_ok(o->load, o->size, o->entry, lo, hi) ? 0
								    : -ERANGE;
	}

	if ((magic & TIUBL_MASK) == TIUBL_MAGIC) {
		o->type = IMG_TIUBL;
		o->hdr_size = 0;
		o->entry = rd32(buf + 0x04);
		o->ubl_pages = rd32(buf + 0x08);
		o->ubl_block = rd32(buf + 0x0C);
		o->ubl_page = rd32(buf + 0x10);
		o->load = rd32(buf + 0x14);
		o->size = o->ubl_pages * 512; /* small-page NAND */
		if (!o->ubl_pages || o->ubl_pages > 0x10000) {
			return -ERANGE;
		}

		return range_ok(o->load, o->size, o->entry, lo, hi) ? 0
								    : -ERANGE;
	}

	if (rd32_be(buf) == UIMAGE_MAGIC) {
		o->type = IMG_UIMAGE;
	}

	return -ENOEXEC;
}

const char *img_type_name(enum img_type t)
{
	switch (t) {
	case IMG_DBI: return "dspboot image";
	case IMG_TIUBL: return "TI UBL NAND header";
	case IMG_UIMAGE: return "uImage";
	case IMG_RAW_ARM: return "raw ARM binary";
	default: return "none";
	}
}

int img_looks_like_arm(const u8 *buf)
{
	u32 w = rd32(buf);

	/*
	 * b <x>, or ldr pc, [pc, #x] - the usual first word of U-Boot/ARM
	 * vectors
	 */
	return (w & 0xFF000000u) == 0xEA000000u ||
	       (w & 0xFFFFF000u) == 0xE59FF000u;
}
