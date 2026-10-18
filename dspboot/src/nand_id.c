/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * NAND geometry from the READ ID bytes or the ONFI parameter page (pure,
 * host-testable).
 *
 * As the DA830 boot ROM does (SPRAB04G appendix B): an ONFI part with a valid
 * parameter page is described by that page; any other part must be in the
 * ROM's device table, which gives the geometry of parts up to 128 MiB, while
 * larger large-page parts are decoded from the 4th ID byte.
 *
 * Only what the ROM and our ECC can handle is accepted: 8-bit bus, pages of
 * 512 B, 2 or 4 KiB with 16 spare bytes per 512-byte step (the ROM keeps each
 * step's 10 ECC bytes at spare offset 16 * step + 6) and blocks of at most 256
 * KiB (the flash layout aligns everything after block 1 to 256 KiB).
 */
#include "nand.h"

struct chip_id {
	u8 dev;
	u16 mib;
	u16 page;
	u8 ppb;
};

/* the 8-bit parts of the ROM's device table (SPRAB04G table 7) */
static const struct chip_id chips[] = {
	{ 0xE3, 4, 512, 16 },	  { 0xE5, 4, 512, 16 },
	{ 0x39, 8, 512, 16 },	  { 0x6B, 8, 512, 16 },
	{ 0xE6, 8, 512, 16 },	  { 0x33, 16, 512, 32 },
	{ 0x73, 16, 512, 32 },	  { 0x35, 32, 512, 32 },
	{ 0x75, 32, 512, 32 },	  { 0xF5, 32, 512, 32 },
	{ 0x36, 64, 512, 32 },	  { 0x76, 64, 512, 32 },
	{ 0x78, 128, 512, 32 },	  { 0x79, 128, 512, 32 },
	{ 0x71, 256, 512, 32 },	  { 0xA1, 128, 2048, 64 },
	{ 0xF1, 128, 2048, 64 },  { 0xAA, 256, 2048, 64 },
	{ 0xDA, 256, 2048, 64 },  { 0xAC, 512, 2048, 64 },
	{ 0xDC, 512, 2048, 64 },  { 0xA3, 1024, 2048, 64 },
	{ 0xD3, 1024, 2048, 64 }, { 0xA5, 2048, 2048, 64 },
	{ 0xD5, 2048, 2048, 64 },
};

static u32 row_cycles(u32 pages)
{
	return pages > 65536 ? 3 : 2;
}

/* The checks every part goes through, then the derived fields */
static int finish(struct nand_info *n, int bus16)
{
	if (bus16) {
		n->why = "16-bit bus";
		return -EOPNOTSUPP;
	}

	if (n->page_size != 512 && n->page_size != 2048 &&
	    n->page_size != 4096) {
		n->why = "page size not 512 B, 2 or 4 KiB";
		return -EOPNOTSUPP;
	}

	n->steps = n->page_size / 512;
	if (n->oob_size != 16 * n->steps) {
		n->why = n->oob_size == 8 * n->steps ?
			"8 spare bytes per 512 (ROM ECC needs 16)" :
			"spare bytes per 512 not 16 (ROM ECC)";
		return -EOPNOTSUPP;
	}

	if (n->block_size > NAND_MAX_BLOCK) {
		n->why = "block larger than 256 KiB";
		return -EOPNOTSUPP;
	}

	n->large = n->page_size > 512;
	n->why = 0;
	n->present = 1;
	return 0;
}

int nand_decode_id(const u8 *id, struct nand_info *n)
{
	const struct chip_id *c = 0;
	u32 i, ext;
	int bus16 = 0;

	n->present = 0;
	n->onfi = 0;
	for (i = 0; i < ARRAY_SIZE(chips); i++) {
		if (chips[i].dev == id[1]) {
			c = &chips[i];
		}
	}

	if (!c) {
		n->why = "unknown device code";
		return -ENODEV;
	}

	n->page_size = c->page;
	n->pages_per_block = c->ppb;
	n->oob_size = c->page / 32;
	if (c->page > 512 && c->mib > 128) {
		ext = id[3];
		n->page_size = 1024u << (ext & 3);
		n->oob_size = (8u << ((ext >> 2) & 1)) * (n->page_size / 512);
		n->pages_per_block = ((64u << 10) << ((ext >> 4) & 3)) /
				     n->page_size;
		bus16 = ext & 0x40;
	}

	n->block_size = n->page_size * n->pages_per_block;
	n->blocks = ((u32)c->mib << 20) / n->block_size;
	n->col_cycles = n->page_size > 512 ? 2 : 1;
	n->row_cycles = row_cycles(n->blocks * n->pages_per_block);
	return finish(n, bus16);
}

/* CRC-16 of the parameter page: polynomial 0x8005, initial value 0x4F4E */
static u16 onfi_crc(const u8 *p, u32 len)
{
	u16 crc = 0x4F4E;
	u32 i;

	while (len--) {
		crc ^= (u16)(*p++ << 8);
		for (i = 0; i < 8; i++) {
			crc = crc & 0x8000 ? (u16)(crc << 1) ^ 0x8005 :
					     (u16)(crc << 1);
		}
	}

	return crc;
}

static u32 le32(const u8 *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

int nand_decode_onfi(const u8 *pp, struct nand_info *n)
{
	n->present = 0;
	n->onfi = 0;
	if (pp[0] != 'O' || pp[1] != 'N' || pp[2] != 'F' || pp[3] != 'I' ||
	    onfi_crc(pp, 254) != (pp[254] | pp[255] << 8)) {
		n->why = "no valid ONFI parameter page";
		return -EINVAL;
	}

	n->onfi = 1;
	n->page_size = le32(pp + 80);
	n->oob_size = pp[84] | pp[85] << 8;
	n->pages_per_block = le32(pp + 92);
	n->block_size = n->page_size * n->pages_per_block;
	n->blocks = le32(pp + 96) * pp[100];
	n->row_cycles = pp[101] & 15;
	n->col_cycles = pp[101] >> 4;
	return finish(n, pp[6] & 1);
}
