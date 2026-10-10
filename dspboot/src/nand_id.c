/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * NAND geometry from the READ ID bytes (pure, host-testable).
 *
 * Small-page parts (512 + 16) are looked up by device code, as the DA830 boot
 * ROM does. Large-page parts (2 or 4 KiB pages) are decoded from the classic
 * 4th ID byte (page size, spare bytes per 512, block size, bus width) plus a
 * device code -> size table.
 *
 * Only what the ROM and our ECC can handle is accepted: 8-bit bus, 16 spare
 * bytes per 512-byte step (the ROM keeps each step's 10 ECC bytes at spare
 * offset 16 * step + 6) and blocks of at most 256 KiB (the flash layout aligns
 * everything after block 1 to 256 KiB).
 */
#include "nand.h"

struct small_id {
	u8 dev;
	u8 mib;
	u8 ppb;
};

/* 8-bit small-page (512 + 16) parts, from TI's ROM device table */
static const struct small_id small_chips[] = {
	{ 0xE3, 4, 16 },   { 0xE5, 4, 16 },   { 0x39, 8, 16 },
	{ 0xE6, 8, 16 },   { 0x6B, 8, 16 },   { 0x33, 16, 32 },
	{ 0x73, 16, 32 },  { 0x35, 32, 32 },  { 0x75, 32, 32 },
	{ 0xF5, 32, 32 },  { 0x36, 64, 32 },  { 0x76, 64, 32 },
	{ 0x78, 128, 32 }, { 0x79, 128, 32 },
};

struct large_id {
	u8 dev;
	u16 mib;
};

/*
 * 8-bit large-page parts, 3.3 V and 1.8 V (16-bit parts are rejected by the ID
 * byte)
 */
static const struct large_id large_chips[] = {
	{ 0xF1, 128 },	{ 0xD1, 128 },	{ 0xA1, 128 },	{ 0xDA, 256 },
	{ 0xAA, 256 },	{ 0xDC, 512 },	{ 0xAC, 512 },	{ 0xD3, 1024 },
	{ 0xA3, 1024 }, { 0xD5, 2048 }, { 0xA5, 2048 },
};

static u32 row_cycles(u32 pages)
{
	return pages > 65536 ? 3 : 2;
}

int nand_decode_id(const u8 *id, struct nand_info *n)
{
	u32 i, ext, spare;

	n->present = 0;
	n->why = "unknown device code";
	for (i = 0; i < ARRAY_SIZE(small_chips); i++) {
		if (small_chips[i].dev != id[1]) {
			continue;
		}

		n->large = 0;
		n->page_size = 512;
		n->oob_size = 16;
		n->pages_per_block = small_chips[i].ppb;
		n->block_size = 512 * n->pages_per_block;
		n->blocks = ((u32)small_chips[i].mib << 20) / n->block_size;
		n->col_cycles = 1;
		n->row_cycles = row_cycles(n->blocks * n->pages_per_block);
		n->bbm_ofs = 5;
		goto ok;
	}

	for (i = 0; i < ARRAY_SIZE(large_chips); i++) {
		if (large_chips[i].dev != id[1]) {
			continue;
		}

		ext = id[3];
		n->large = 1;
		n->page_size = 1024u << (ext & 3);
		spare = 8u << ((ext >> 2) & 1);
		n->oob_size = spare * (n->page_size / 512);
		n->block_size = (64u << 10) << ((ext >> 4) & 3);
		n->pages_per_block = n->block_size / n->page_size;
		n->blocks = ((u32)large_chips[i].mib << 20) / n->block_size;
		n->col_cycles = 2;
		n->row_cycles = row_cycles(n->blocks * n->pages_per_block);
		n->bbm_ofs = 0;
		if (ext & 0x40) {
			n->why = "16-bit bus";
			return -EOPNOTSUPP;
		}

		if (n->page_size < 2048 || n->page_size > NAND_MAX_PAGE) {
			n->why = "page size not 2 or 4 KiB";
			return -EOPNOTSUPP;
		}

		if (spare != 16) {
			n->why = "8 spare bytes per 512 (ROM ECC needs 16)";
			return -EOPNOTSUPP;
		}

		if (n->block_size > NAND_MAX_BLOCK) {
			n->why = "block larger than 256 KiB";
			return -EOPNOTSUPP;
		}

		goto ok;
	}

	return -ENODEV;
ok:
	n->steps = n->page_size / 512;
	n->why = 0;
	n->present = 1;
	return 0;
}
