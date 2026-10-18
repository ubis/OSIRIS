/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef DSPBOOT_NAND_H
#define DSPBOOT_NAND_H

#include "errcodes.h"
#include "types.h"

/* Largest supported geometry (buffers are sized for it) */
#define NAND_MAX_PAGE	4096u
#define NAND_MAX_OOB	256u
#define NAND_MAX_BLOCK	(256u << 10)

struct nand_info {
	int present;
	u8 id[5];
	/* 2/4 KiB pages: READ 00h/30h, 2 column cycles */
	int large;
	u32 page_size; /* data bytes per page */
	u32 oob_size;
	u32 steps; /* 512-byte ECC steps per page */
	u32 pages_per_block;
	u32 blocks;
	u32 block_size; /* bytes */
	u32 col_cycles;
	u32 row_cycles;
	/*
	 * geometry from the ONFI parameter page: the boot ROM then checks the
	 * first and last page of a block for bad block marks, else pages 0, 1
	 */
	int onfi;
	const char *vendor;
	const char *why; /* reason the device is not supported */
};

extern struct nand_info nand;
/* page and spare scratch buffers for callers (boot, shell) */
extern u8 nand_pagebuf[NAND_MAX_PAGE];
extern u8 nand_oobbuf[NAND_MAX_OOB];

int nand_init(void); /* 0, -ETIMEDOUT, -ENODEV, -EOPNOTSUPP */
void nand_summary(void); /* one banner line */
void nand_report(void); /* full report */
u32 nand_count_bad(void);
/* bad as the boot ROM sees it, the bad block table's blocks included */
int nand_block_is_bad(u32 block);
/* holds a flash bad block table ("Bbt0" or its mirror "1tbB") */
int nand_block_is_bbt(u32 block);
/*
 * Reads return the number of corrected symbols (>= 0) or -ENODATA (erased:
 * all 0xFF, no ECC), -ENOMSG (data but no ECC bytes: written without ECC),
 * -EBADMSG (uncorrectable) or -ETIMEDOUT (R/B). Program and erase return 0,
 * -ETIMEDOUT, -EROFS (write protected) or -EIO (status FAIL).
 */
/*
 * Raw read: 'data' gets page_size bytes, 'oob' oob_size bytes (either may be
 * NULL).
 */
int nand_read_raw(u32 page, u8 *data, u8 *oob);
/*
 * ECC-checked read using the ROM layout: 4-bit HW ECC per 512-byte step, its
 * 10 bytes at spare offset 16 * step + 6. 'data' gets page_size bytes, 'oob'
 * (may be NULL) oob_size bytes.
 */
int nand_read_page(u32 page, u8 *data, u8 *oob);
const char *nand_strerror(int rc);
/* Erase one block (caller must check it is not bad). */
int nand_erase_block(u32 block);
/*
 * Program one page raw: page_size data + oob_size spare bytes from 'buf', no
 * ECC generated.
 */
int nand_program_raw(u32 page, const u8 *buf);

/* Pure helpers (host-testable) */
/*
 * Fill the geometry fields of 'n' from READ ID bytes 0..3: 0, -ENODEV unknown,
 * -EOPNOTSUPP unsupported (n->why).
 */
int nand_decode_id(const u8 *id, struct nand_info *n);
/*
 * The same from a 256-byte ONFI parameter page: -EINVAL if its signature or
 * CRC is wrong.
 */
int nand_decode_onfi(const u8 *pp, struct nand_info *n);
/* Unpack 10 stored ECC bytes into eight 10-bit values */
void nand_ecc_unpack(const u8 *ecc10_bytes, u16 *out8);

#endif
