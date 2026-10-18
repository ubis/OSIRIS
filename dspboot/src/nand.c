/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * dspboot - NAND driver for EMIFA CS3 with the ROM compatible 4-bit hardware
 * ECC, for small-page (512 + 16) and large-page (2/4 KiB, 16 spare bytes per
 * 512) parts. The geometry comes from the ONFI parameter page or the READ ID
 * bytes (nand_id.c), as the boot ROM takes it.
 *
 * The 4-bit ECC read/correct sequence follows the DA830 EMIFA documentation as
 * implemented by TI's device_nand.c (FlashAndBootUtils 2.40, BSD-3-Clause) and
 * the Linux davinci_nand driver. The engine checks one 512-byte step at a time,
 * so large pages are read spare area first, then step by step with random data
 * output (05h/E0h), like Linux's "OOB first" page read.
 *
 * Writing is raw only (nand_program_raw: page + spare bytes exactly as given,
 * no ECC generated), meant for byte-exact restores and for images from
 * mknandimg.py.
 */
#include "board.h"
#include "hw.h"
#include "lib.h"
#include "nand.h"
#include "soc.h"
#include "timer.h"

struct nand_info nand;
u8 nand_pagebuf[NAND_MAX_PAGE] __attribute__((aligned(8)));
u8 nand_oobbuf[NAND_MAX_OOB] __attribute__((aligned(8)));
static u32 rb_timeouts;
static u8 spare[NAND_MAX_OOB] __attribute__((aligned(8)));

#define NAND_DATA	REG8(NAND_BASE)
#define NAND_CMD(c)	(REG8(NAND_BASE + NAND_CLE_OFS) = (u8)(c))
#define NAND_ADDR(a)	(REG8(NAND_BASE + NAND_ALE_OFS) = (u8)(a))

#define CMD_READ0	0x00
#define CMD_READSTART	0x30	/* large page: second cycle of READ */
#define CMD_RNDOUT	0x05	/* large page: random data output */
#define CMD_RNDOUTSTART	0xE0
#define CMD_READOOB	0x50	/* small page: read area C (spare) */
#define CMD_READID	0x90
#define CMD_READPARAM	0xEC
#define CMD_STATUS	0x70
#define CMD_RESET	0xFF
#define CMD_SEQIN	0x80
#define CMD_PAGEPROG	0x10
#define CMD_ERASE1	0x60
#define CMD_ERASE2	0xD0

#define STATUS_FAIL	0x01
#define STATUS_NOT_WP	0x80

static const char *vendor_name(u8 id)
{
	switch (id) {
	case 0x20: return "Numonyx";
	case 0x98: return "Toshiba";
	case 0xEC: return "Samsung";
	case 0xAD: return "Hynix";
	case 0x2C: return "Micron";
	case 0x01: return "Spansion";
	case 0x04: return "Fujitsu";
	case 0xC2: return "Macronix";
	case 0xEF: return "Winbond";
	case 0xC8: return "ESMT";
	case 0x92: return "EON";
	default: return "unknown";
	}
}

static int wait_ready_us(u32 us)
{
	u32 t0 = (u32)timer_ticks();
	u32 limit = (cpu_hz / 1000000) * us;

	/* tWB: R/B goes low within 100 ns */
	udelay(1);
	while (!(EMIFA_NANDFSR & NANDFSR_READY)) {
		if ((u32)timer_ticks() - t0 > limit) {
			rb_timeouts++;
			return -ETIMEDOUT;
		}
	}

	return 0;
}

/* 1 ms covers tR (12-25 us) and tRST (< 500 us) */
static int wait_ready(void)
{
	return wait_ready_us(1000);
}

static void send_col(u32 col)
{
	NAND_ADDR(col);
	if (nand.col_cycles > 1) {
		NAND_ADDR(col >> 8);
	}
}

static void send_row(u32 page)
{
	u32 i;

	for (i = 0; i < nand.row_cycles; i++) {
		NAND_ADDR(page >> (8 * i));
	}
}

/*
 * Start a page read at column 'col' and wait for the data. On small-page parts
 * the spare area is reached with READOOB (column relative to it) instead.
 */
static int start_read(u32 page, u32 col)
{
	if (nand.large) {
		NAND_CMD(CMD_READ0);
		send_col(col);
		send_row(page);
		NAND_CMD(CMD_READSTART);
	} else if (col >= nand.page_size) {
		NAND_CMD(CMD_READOOB);
		send_col(col - nand.page_size);
		send_row(page);
	} else {
		NAND_CMD(CMD_READ0);
		send_col(col);
		send_row(page);
	}

	return wait_ready();
}

/*
 * Large page: move the read pointer within the page register (tCCS/tWHR < 1
 * us).
 */
static void read_column(u32 col)
{
	NAND_CMD(CMD_RNDOUT);
	send_col(col);
	NAND_CMD(CMD_RNDOUTSTART);
	udelay(1);
}

static void read_buf(u8 *buf, u32 len)
{
	while (len--) {
		*buf++ = NAND_DATA;
	}
}

static void skip_buf(u32 len)
{
	while (len--) {
		(void)NAND_DATA;
	}
}

/*
 * Geometry from the ONFI parameter page, the first of its copies with a
 * valid CRC: -ENODEV no ONFI signature, -EINVAL no valid copy.
 */
static int read_onfi(void)
{
	u8 sig[4];
	u32 i;
	int rc;

	NAND_CMD(CMD_READID);
	NAND_ADDR(0x20);
	udelay(1);
	read_buf(sig, 4);
	if (memcmp(sig, "ONFI", 4)) {
		return -ENODEV;
	}

	NAND_CMD(CMD_READPARAM);
	NAND_ADDR(0);
	if (wait_ready()) {
		return -ETIMEDOUT;
	}

	for (i = 0; i < 3; i++) {
		read_buf(nand_pagebuf, 256);
		rc = nand_decode_onfi(nand_pagebuf, &nand);
		if (rc != -EINVAL) {
			return rc;
		}
	}

	return -EINVAL;
}

int nand_init(void)
{
	int rc;

	memset(&nand, 0, sizeof(nand));
	if (psc_set(0, LPSC0_EMIFA, PSC_STATE_ENABLE)) {
		return -ETIMEDOUT;
	}

	pinmux_nand();
	EMIFA_CE3CFG = BOARD_NAND_CE3CFG;
	EMIFA_NANDFCR |= 1u << (NAND_CS - 2); /* CS3 in NAND mode */

	NAND_CMD(CMD_RESET);
	udelay(10);
	wait_ready();
	NAND_CMD(CMD_READID);
	NAND_ADDR(0);
	udelay(1);
	read_buf(nand.id, 5);
	nand.vendor = vendor_name(nand.id[0]);
	rc = read_onfi();
	if (rc == -ENODEV || rc == -EINVAL) {
		rc = nand_decode_id(nand.id, &nand);
	}

	return rc;
}

int nand_read_raw(u32 page, u8 *data, u8 *oob)
{
	int rc;

	rc = start_read(page, 0);
	if (data) {
		read_buf(data, nand.page_size);
	} else {
		skip_buf(nand.page_size);
	}

	if (oob) {
		read_buf(oob, nand.oob_size);
	}

	if (!nand.large) {
		NAND_CMD(CMD_READ0);
	}

	return rc ? -ETIMEDOUT : 0;
}

static int read_oob_only(u32 page, u8 *oob)
{
	int rc;

	rc = start_read(page, nand.page_size);
	read_buf(oob, nand.oob_size);
	if (!nand.large) {
		/* back to area A for the next access */
		NAND_CMD(CMD_READ0);
	}

	return rc;
}

static int all_ff(const u8 *p, u32 n)
{
	while (n--) {
		if (*p++ != 0xFF) {
			return 0;
		}
	}

	return 1;
}

/*
 * The boot ROM's rule: a block is bad unless the first 6 spare bytes of every
 * 512-byte step are 0xFF in pages 0 and 1, or in the first and last page of an
 * ONFI part. The factory marker (spare byte 5 or 0) is among them.
 */
static int page_marked_bad(u32 page)
{
	u32 s;

	read_oob_only(page, spare);
	for (s = 0; s < nand.steps; s++) {
		if (!all_ff(spare + 16 * s, 6)) {
			return 1;
		}
	}

	return 0;
}

int nand_block_is_bad(u32 block)
{
	u32 p = block * nand.pages_per_block;

	return page_marked_bad(p) ||
	       page_marked_bad(nand.onfi ? p + nand.pages_per_block - 1 : p + 1);
}

int nand_block_is_bbt(u32 block)
{
	read_oob_only(block * nand.pages_per_block, spare);
	return !memcmp(spare, "Bbt0", 4) || !memcmp(spare, "1tbB", 4);
}

/* Read 512 bytes from the current column with the 4-bit ECC engine running. */
static void ecc_read_step(u8 *data)
{
	u32 fcr;

	/* reset the ECC engine */
	(void)EMIFA_NAND4BITECC(0);
	fcr = EMIFA_NANDFCR & ~(3u << NANDFCR_4BITECC_SEL_SHIFT);
	EMIFA_NANDFCR = fcr | ((NAND_CS - 2) << NANDFCR_4BITECC_SEL_SHIFT) |
			NANDFCR_4BITECC_START;
	(void)EMIFA_NANDFCR;
	read_buf(data, 512);
	/* terminate the calculation */
	(void)EMIFA_NAND4BITECC(0);
}

/*
 * Feed the stored parity of the step just read to the ECC engine and fix up
 * 'data'. Returns errors fixed or -EBADMSG.
 */
static int ecc_correct(u8 *data, const u8 *ecc)
{
	u16 e[8];
	u32 syn, fsr, state, t0, limit, i, nerr;
	int k;

	nand_ecc_unpack(ecc, e);
	/* clear a previous address calculation */
	(void)EMIFA_NANDERRADD1;
	for (k = 7; k >= 0; k--) {
		EMIFA_NAND4BITECCLOAD = e[k];
	}

	/* give the syndrome logic a cycle */
	(void)EMIFA_NANDFSR;
	syn = 0;
	for (i = 0; i < 4; i++) {
		syn |= EMIFA_NAND4BITECC(i) & 0x03FF03FF;
	}

	if (!syn) {
		return 0;
	}

	(void)EMIFA_NANDERRADD1;
	EMIFA_NANDFCR |= NANDFCR_4BITECC_ADD_CALC;
	/*
	 * ECC_STATE reads 3 right after the start; wait until the engine moved
	 * past it
	 */
	t0 = (u32)timer_ticks();
	limit = (cpu_hz / 1000000) * 100; /* 100 us */
	do {
		state = (EMIFA_NANDFSR >> 8) & 0xF;
	} while (state < 4 && (u32)timer_ticks() - t0 < limit);

	t0 = (u32)timer_ticks();
	for (;;) {
		fsr = EMIFA_NANDFSR;
		state = (fsr >> 8) & 0xF;
		if (!state) { /* no error (should not happen here) */
			(void)EMIFA_NANDERRVAL1;
			return 0;
		}

		if (state == 1) { /* more than 4 errors */
			(void)EMIFA_NANDERRVAL1;
			return -EBADMSG;
		}

		if (state == 2 || state == 3) {
			break;
		}

		if ((u32)timer_ticks() - t0 > limit * 10) {
			return -EBADMSG;
		}
	}

	nerr = 1 + ((fsr >> 16) & 3);
	for (i = 0; i < nerr; i++) {
		u32 addr = (i > 1) ? EMIFA_NANDERRADD2 : EMIFA_NANDERRADD1;
		u32 val = (i > 1) ? EMIFA_NANDERRVAL2 : EMIFA_NANDERRVAL1;
		if (i & 1) {
			addr >>= 16;
			val >>= 16;
		}

		addr = (512 + 7) - (addr & 0x3FF);
		if (addr < 512) {
			data[addr] ^= (u8)(val & 0xFF);
		}
	}

	return (int)nerr;
}

/* ECC bytes of every step blank: erased page, or data written without ECC */
static int check_blank(const u8 *data, const u8 *oob)
{
	u32 s, ff = 0;

	for (s = 0; s < nand.steps; s++) {
		ff += all_ff(oob + 16 * s + 6, 10);
	}

	if (!ff) {
		return 0;
	}

	if (ff == nand.steps && all_ff(data, nand.page_size) &&
	    all_ff(oob, nand.oob_size)) {
		return -ENODATA;
	}

	return -ENOMSG;
}

int nand_read_page(u32 page, u8 *data, u8 *oob)
{
	u32 s;
	int rc, total = 0;

	if (!oob) {
		oob = spare;
	}

	if (!nand.large) {
		rc = start_read(page, 0);
		ecc_read_step(data);
		read_buf(oob, nand.oob_size);
		if (rc) {
			return -ETIMEDOUT;
		}

		rc = check_blank(data, oob);
		return rc ? rc : ecc_correct(data, oob + 6);
	}

	if (start_read(page, nand.page_size)) {
		return -ETIMEDOUT;
	}

	read_buf(oob, nand.oob_size);
	for (s = 0; s < nand.steps; s++) {
		read_column(512 * s);
		ecc_read_step(data + 512 * s);
		if (all_ff(oob + 16 * s + 6, 10)) {
			/* sorted out by check_blank() below */
			continue;
		}

		rc = ecc_correct(data + 512 * s, oob + 16 * s + 6);
		if (rc < 0) {
			return rc;
		}

		total += rc;
	}

	rc = check_blank(data, oob);
	return rc ? rc : total;
}

static int read_status(void)
{
	NAND_CMD(CMD_STATUS);
	udelay(1);
	return NAND_DATA;
}

static int check_status(int timed_out)
{
	int st;

	if (timed_out) {
		return -ETIMEDOUT;
	}

	st = read_status();
	NAND_CMD(CMD_READ0);
	if (!(st & STATUS_NOT_WP)) {
		return -EROFS;
	}

	return (st & STATUS_FAIL) ? -EIO : 0;
}

int nand_erase_block(u32 block)
{
	NAND_CMD(CMD_ERASE1);
	send_row(block * nand.pages_per_block);
	NAND_CMD(CMD_ERASE2);
	/* tBERS is 2-3 ms typ */
	return check_status(wait_ready_us(20000));
}

int nand_program_raw(u32 page, const u8 *buf)
{
	u32 i, n = nand.page_size + nand.oob_size;

	if (!nand.large) {
		/* point at area A before SEQIN */
		NAND_CMD(CMD_READ0);
	}

	NAND_CMD(CMD_SEQIN);
	send_col(0);
	send_row(page);
	for (i = 0; i < n; i++) {
		NAND_DATA = buf[i];
	}

	NAND_CMD(CMD_PAGEPROG);
	/* tPROG is 200-700 us */
	return check_status(wait_ready_us(5000));
}

const char *nand_strerror(int rc)
{
	switch (rc) {
	case -ENODATA: return "erased";
	case -EBADMSG: return "UNCORRECTABLE ECC error";
	case -ENOMSG: return "no ECC bytes (written without ROM ECC?)";
	case -ETIMEDOUT: return "R/B timeout";
	case -EROFS: return "write protected";
	case -EIO: return "program/erase FAILED (status)";
	default: return rc > 0 ? "corrected" : "ok";
	}
}

static void print_size(u32 bytes)
{
	if (bytes >= 1024 && !(bytes & 1023)) {
		printf("%u KiB", bytes >> 10);
	} else {
		printf("%u B", bytes);
	}
}

/* bad blocks, not counting the ones that hold the bad block table */
u32 nand_count_bad(void)
{
	u32 b, n = 0;

	for (b = 0; b < nand.blocks; b++) {
		n += nand_block_is_bad(b) && !nand_block_is_bbt(b) ? 1 : 0;
	}

	return n;
}

/* One line for the boot banner */
void nand_summary(void)
{
	u32 nbad;

	if (!nand.present) {
		if (nand.id[0] == 0x00 || nand.id[0] == 0xFF) {
			printf("NAND:   none found\n");
		} else {
			printf("NAND:   %s (ID %02x %02x %02x %02x) not supported: %s\n",
			       nand.vendor, nand.id[0], nand.id[1], nand.id[2],
			       nand.id[3],
			       nand.why ? nand.why : "unknown device");
		}

		return;
	}

	nbad = nand_count_bad();
	printf("NAND:   %u MiB %s, ",
	       nand.blocks * (nand.block_size >> 10) >> 10, nand.vendor);
	print_size(nand.page_size);
	printf(" pages, ");
	print_size(nand.block_size);
	printf(" blocks, %u bad block%s\n", nbad, nbad == 1 ? "" : "s");
	if (rb_timeouts) {
		printf("        WARNING: %u R/B (EMA_WAIT0) timeouts - is R/B wired to WAIT0?\n",
		       rb_timeouts);
	}
}

/* Everything, for "info" and "nand info" */
void nand_report(void)
{
	u32 b, nbad = 0;

	printf("NAND:   ID %02x %02x %02x %02x %02x, %s", nand.id[0],
	       nand.id[1], nand.id[2], nand.id[3], nand.id[4], nand.vendor);
	if (!nand.present) {
		printf(", not supported (%s)\n",
		       nand.why ? nand.why : "no response");
		return;
	}

	printf(", %u MiB, %s page%s\n",
	       nand.blocks * (nand.block_size >> 10) >> 10,
	       nand.large ? "large" : "small", nand.onfi ? ", ONFI" : "");
	printf("        page %u + %u spare, block %u KiB (%u pages), %u blocks, %u+%u address cycles\n",
	       nand.page_size, nand.oob_size, nand.block_size >> 10,
	       nand.pages_per_block, nand.blocks, nand.col_cycles,
	       nand.row_cycles);
	printf("        CE3CFG %08x, NANDFCR %08x\n", EMIFA_CE3CFG,
	       EMIFA_NANDFCR);
	printf("        bad blocks (spare bytes 0-5 not FF in the %s page):",
	       nand.onfi ? "first or last" : "first or second");
	for (b = 0; b < nand.blocks; b++) {
		if (nand_block_is_bad(b) && !nand_block_is_bbt(b)) {
			if (nbad < 16) {
				printf(" %u", b);
			}

			nbad++;
		}
	}

	if (nbad > 16) {
		printf(" ...");
	}

	printf("%s (%u total)\n", nbad ? "" : " none", nbad);
	con_puts("        bad block table:");
	nbad = 0;
	for (b = nand.blocks; b-- > 0 && b + 4 >= nand.blocks;) {
		if (nand_block_is_bbt(b)) {
			printf(" %u", b);
			nbad++;
		}
	}

	con_puts(nbad ? "\n" : " none\n");
	if (rb_timeouts) {
		printf("        WARNING: %u R/B (EMA_WAIT0) timeouts - is R/B wired to WAIT0?\n",
		       rb_timeouts);
	}
}
