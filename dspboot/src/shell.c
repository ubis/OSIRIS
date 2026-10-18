/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The "dspboot>" console: line input and a table of commands.
 */
#include "board.h"
#include "boot.h"
#include "crc.h"
#include "env.h"
#include "errcodes.h"
#include "hw.h"
#include "lib.h"
#include "nand.h"
#include "sdram.h"
#include "shell.h"
#include "soc.h"
#include "uart.h"

#define MAX_ARGS	6

static int readline(char *buf, int max)
{
	int n = 0, c;

	for (;;) {
		c = con_getc_timeout(0xFFFFFFFF);
		if (c == '\r' || c == '\n') {
			con_puts("\n");
			buf[n] = 0;
			return n;
		}

		if ((c == 8 || c == 127) && n > 0) {
			n--;
			con_puts("\b \b");
		} else if (c == 3) {
			con_puts("^C\n");
			buf[0] = 0;
			return 0;
		} else if (c >= 32 && c < 127 && n < max - 1) {
			buf[n++] = (char)c;
			con_putc(c);
		}
	}
}

static int split(char *s, char **argv)
{
	int argc = 0;

	while (*s && argc < MAX_ARGS) {
		while (*s == ' ') {
			*s++ = 0;
		}

		if (!*s) {
			break;
		}

		argv[argc++] = s;
		while (*s && *s != ' ') {
			s++;
		}
	}

	return argc;
}

/* Commands */

static void cmd_help(int argc, char **argv);

static void cmd_info(int argc, char **argv)
{
	printf("dspboot %s (%s - %s), board %s\n", DSPBOOT_VERSION, __DATE__,
	       __TIME__, BOARD_NAME);
	soc_report();
	pll0_report();
	printf("UART:   UART%d, %u baud (divisor %u, actual %u), %u receive errors\n",
	       uart_index(), BOARD_BAUD, uart_divisor(), uart_actual_baud(),
	       uart_errors());
	printf("SDRAM:  %u MiB at 0x%08x, SDCR %08x, SDTIMR %08x, SDTIMR2 %08x, SDRCR %08x\n",
	       sdram_size >> 20, SDRAM_BASE, EMIFB_SDCR, EMIFB_SDTIMR,
	       EMIFB_SDTIMR2, EMIFB_SDRCR);
	nand_report();
	printf("Layout: env 0x%06x + 0x%06x (%u KiB), slot a 0x%06x, slot b 0x%06x (%u KiB each)\n",
	       BOARD_ENV_OFS, BOARD_ENV_REDUND_OFS, BOARD_ENV_SIZE >> 10,
	       BOARD_UBOOT_A_OFS, BOARD_UBOOT_B_OFS,
	       BOARD_UBOOT_SLOT_LEN >> 10);
}

static void cmd_boot(int argc, char **argv)
{
	if (argc < 2) {
		boot_slots();
	} else if (env_slot(argv[1], strlen(argv[1]))) {
		boot_slot(argv[1][0]);
	} else {
		con_puts("usage: boot [a|b]\n");
	}
}

static void cmd_loady(int argc, char **argv)
{
	u32 a = BOARD_UBOOT_ADDR;

	if (argc > 1 && !parse_u32(argv[1], &a)) {
		con_puts("bad address\n");
	} else {
		boot_loady(a);
	}
}

static void cmd_bootram(int argc, char **argv)
{
	u32 a = boot_last_rx();

	if (argc > 1 && !parse_u32(argv[1], &a)) {
		con_puts("bad address\n");
	} else {
		boot_ram(a, argc > 2 && !strcmp(argv[2], "force"));
	}
}

static void cmd_md(int argc, char **argv)
{
	u32 a, len = 256;

	if (argc < 2 || !parse_u32(argv[1], &a) ||
	    (argc > 2 && !parse_u32(argv[2], &len))) {
		con_puts("usage: md <addr> [len]\n");
		return;
	}

	a &= ~3u;
	hexdump(a, (const void *)a, len);
}

static void cmd_memtest(int argc, char **argv)
{
	u32 a = SDRAM_BASE, len = sdram_size;

	if (argc > 2 &&
	    (!parse_u32(argv[1], &a) || !parse_u32(argv[2], &len))) {
		con_puts("usage: memtest [addr len]\n");
		return;
	}

	if (argc <= 2 && !sdram_size) {
		con_puts("SDRAM was not detected, give a range: "
			 "memtest 0xc0000000 0x4000000\n");
		return;
	}

	/* undetected SDRAM can still be tested by hand, in the EMIFB window */
	if (!len || (sdram_size ? !sdram_contains(a, len) :
		     a < SDRAM_BASE || len > SDRAM_MAX ||
		     a - SDRAM_BASE > SDRAM_MAX - len)) {
		con_puts("range must be inside SDRAM\n");
		return;
	}

	sdram_memtest(a & ~3u, len & ~3u);
}

static void cmd_reset(int argc, char **argv)
{
	con_puts("resetting...\n");
	uart_flush();
	soc_reset();
}

#define RAW_REC	(nand.page_size + nand.oob_size)

static u8 rawbuf[NAND_MAX_PAGE + NAND_MAX_OOB] __attribute__((aligned(8)));

/* Checks shared by writeraw/crcraw: first page and page count of the range */
static int raw_range(u32 ofs, u32 len, u32 *first, u32 *pages)
{
	u32 total = nand.blocks * nand.pages_per_block;

	if ((ofs & (nand.block_size - 1)) || !len || (len % RAW_REC)) {
		printf("offset must be block aligned (0x%x) and len a multiple of %u\n",
		       nand.block_size, RAW_REC);
		return -EINVAL;
	}

	*first = ofs / nand.page_size;
	*pages = len / RAW_REC;
	if (*first + *pages > total) {
		con_puts("range past the end of the flash\n");
		return -ERANGE;
	}

	return 0;
}

/*
 * Where a range written with 'skip' has to end: inside the part of the flash
 * layout it starts in, dspboot's blocks or a U-Boot slot, as the boot ROM and
 * dspboot read them; else at the end of the flash.
 */
static u32 skip_end(u32 ofs)
{
	static const u32 area[][2] = {
		{ 0, BOARD_ENV_REDUND_OFS },
		{ BOARD_UBOOT_A_OFS, BOARD_UBOOT_A_OFS + BOARD_UBOOT_SLOT_LEN },
		{ BOARD_UBOOT_B_OFS, BOARD_UBOOT_B_OFS + BOARD_UBOOT_SLOT_LEN },
	};
	u32 i;

	for (i = 0; i < ARRAY_SIZE(area); i++) {
		if (ofs >= area[i][0] && ofs < area[i][1]) {
			return area[i][1] / nand.block_size;
		}
	}

	return nand.blocks;
}

/*
 * The block the next block of a range goes to: the following one, or with
 * 'skip' the next good one, as the boot ROM, dspboot and nandwrite read and
 * write images. -ERANGE at block 'end'.
 */
static int next_block(u32 *blk, int skip, u32 end)
{
	while (skip && *blk < end && nand_block_is_bad(*blk)) {
		(*blk)++;
	}

	return *blk < end ? 0 : -ERANGE;
}

static void do_writeraw(u32 addr, u32 ofs, u32 len, int skip)
{
	u8 *rb = rawbuf;
	const u8 *src = (const u8 *)addr;
	u32 first, pages, i, page, blk, nblk, end, bad_verify = 0, skipped = 0;
	int rc;

	if (raw_range(ofs, len, &first, &pages)) {
		return;
	}

	if (!sdram_contains(addr, len)) {
		con_puts("source must be inside SDRAM\n");
		return;
	}

	for (i = 0; !skip && i < pages; i += nand.pages_per_block) {
		blk = (first + i) / nand.pages_per_block;
		if (nand_block_is_bad(blk)) {
			printf("block %u (0x%08x) is marked bad - nothing written\n",
			       blk, blk * nand.block_size);
			return;
		}
	}

	nblk = (pages + nand.pages_per_block - 1) / nand.pages_per_block;
	printf("writeraw: %u pages (%u blocks) from 0x%08x to NAND 0x%08x\n",
	       pages, nblk, addr, ofs);
	blk = first / nand.pages_per_block;
	end = skip ? skip_end(ofs) : nand.blocks;
	for (i = 0; i < pages; i++, src += RAW_REC) {
		if (!(i % nand.pages_per_block)) {
			if (i) {
				blk++;
			}

			page = blk;
			if (next_block(&blk, skip, end)) {
				printf("\nran past 0x%08x - stopped\n",
				       end * nand.block_size);
				return;
			}

			skipped += blk - page;
			rc = nand_erase_block(blk);
			if (rc) {
				printf("\nerase block %u: %s - stopped\n", blk,
				       nand_strerror(rc));
				return;
			}

			printf("\r  block %u/%u", i / nand.pages_per_block + 1,
			       nblk);
		}

		page = blk * nand.pages_per_block + i % nand.pages_per_block;
		rc = nand_program_raw(page, src);
		if (rc) {
			printf("\nprogram page %u: %s - stopped\n", page,
			       nand_strerror(rc));
			return;
		}

		nand_read_raw(page, rb, rb + nand.page_size);
		if (memcmp(rb, src, RAW_REC)) {
			bad_verify++;
		}
	}

	if (skipped) {
		printf("\n  %u bad block%s skipped, ends at 0x%08x", skipped,
		       skipped == 1 ? "" : "s", (blk + 1) * nand.block_size);
	}

	printf("\nwriteraw: done, %u pages differ on raw read-back%s\n",
	       bad_verify,
	       bad_verify ? " (bit flips? check with crcraw/nand dump)" : "");
}

static void do_crcraw(u32 ofs, u32 len, int skip)
{
	u8 *rb = rawbuf;
	u32 first, pages, i, blk, end, crc = 0;

	if (raw_range(ofs, len, &first, &pages)) {
		return;
	}

	blk = first / nand.pages_per_block;
	end = skip ? skip_end(ofs) : nand.blocks;
	for (i = 0; i < pages; i++) {
		if (!(i % nand.pages_per_block)) {
			if (i) {
				blk++;
			}

			if (next_block(&blk, skip, end)) {
				printf("crcraw: ran past 0x%08x\n",
				       end * nand.block_size);
				return;
			}
		}

		nand_read_raw(blk * nand.pages_per_block +
				      i % nand.pages_per_block,
			      rb, rb + nand.page_size);
		crc = crc32_update(crc, rb, RAW_REC);
	}

	printf("crcraw 0x%08x +%u: CRC32 %08x\n", ofs, len, crc);
}

static void cmd_nand(int argc, char **argv)
{
	u32 page, addr, ofs, len, p, n, errs = 0;
	int rc;

	if (argc >= 2 && !strcmp(argv[1], "info")) {
		nand_report();
		return;
	}

	if (!nand.present) {
		con_puts("no supported NAND\n");
		return;
	}

	if (argc >= 5 && !strcmp(argv[1], "writeraw") &&
	    parse_u32(argv[2], &addr) && parse_u32(argv[3], &ofs) &&
	    parse_u32(argv[4], &len)) {
		do_writeraw(addr, ofs, len,
			    argc >= 6 && !strcmp(argv[5], "skip"));
		return;
	}

	if (argc >= 4 && !strcmp(argv[1], "crcraw") &&
	    parse_u32(argv[2], &ofs) && parse_u32(argv[3], &len)) {
		do_crcraw(ofs, len, argc >= 5 && !strcmp(argv[4], "skip"));
		return;
	}

	if (argc >= 3 && !strcmp(argv[1], "dump") &&
	    parse_u32(argv[2], &page)) {
		if (page >= nand.blocks * nand.pages_per_block) {
			con_puts("page out of range\n");
			return;
		}

		nand_read_raw(page, nand_pagebuf, nand_oobbuf);
		printf("page %u (offset 0x%08x, block %u) raw:\n", page,
		       page * nand.page_size, page / nand.pages_per_block);
		hexdump(page * nand.page_size, nand_pagebuf, nand.page_size);
		con_puts("spare:");
		for (n = 0; n < nand.oob_size; n++) {
			printf("%s%02x", n % 16 ? " " : "\n  ", nand_oobbuf[n]);
		}

		rc = nand_read_page(page, nand_pagebuf, nand_oobbuf);
		printf("\nECC (ROM 4-bit layout): %s", nand_strerror(rc));
		if (rc > 0) {
			printf(" (%d symbol errors fixed)", rc);
		}

		con_puts("\n");
		return;
	}

	if (argc >= 5 && !strcmp(argv[1], "read") &&
	    parse_u32(argv[2], &addr) && parse_u32(argv[3], &ofs) &&
	    parse_u32(argv[4], &len)) {
		if ((ofs & (nand.page_size - 1)) ||
		    !sdram_contains(addr, len)) {
			con_puts(
				"offset must be page aligned and the target inside SDRAM\n");
			return;
		}

		for (p = ofs / nand.page_size; len; p++) {
			n = len > nand.page_size ? nand.page_size : len;
			rc = nand_read_page(p, nand_pagebuf, nand_oobbuf);
			if (rc < 0 && rc != -ENODATA) {
				printf("page %u: %s\n", p, nand_strerror(rc));
				errs++;
			}

			memcpy((void *)addr, nand_pagebuf, n);
			addr += n;
			len -= n;
		}

		printf("done, %u pages with errors\n", errs);
		return;
	}

	con_puts(
		"usage: nand info | nand dump <page> | nand read <addr> <ofs> <len>\n"
		"       nand writeraw <addr> <ofs> <len> [skip] | nand crcraw <ofs> <len> [skip]\n");
}

struct cmd {
	const char *name;
	void (*fn)(int argc, char **argv);
	const char *help;
};

static const struct cmd cmds[] = {
	{ "help", cmd_help, "help                          this text" },
	{ "info", cmd_info, "info                          hardware report" },
	{ "boot", cmd_boot,
	  "boot [a|b]                    start U-Boot from NAND (default: bootslot,\n"
	  "                                then the other slot)" },
	{ "loady", cmd_loady,
	  "loady [addr]                  YMODEM receive to SDRAM (default 0xC1080000)" },
	{ "bootram", cmd_bootram,
	  "bootram [addr] [force]        check an image in SDRAM and start it" },
	{ "nand", cmd_nand,
	  "nand info                     NAND geometry and bad blocks\n"
	  "  nand dump <page>              raw page + spare, with ECC check\n"
	  "  nand read <addr> <ofs> <len>  ECC-checked read to SDRAM (no bad block skip)\n"
	  "  nand writeraw <addr> <ofs> <len> [skip]\n"
	  "                                raw restore of page + spare records (528, 2112\n"
	  "                                or 4224 bytes per page) from SDRAM; erases each\n"
	  "                                block first, verifies every page, refuses bad\n"
	  "                                blocks, or with skip goes on in the next good one,\n"
	  "                                within dspboot's blocks or the U-Boot slot\n"
	  "  nand crcraw <ofs> <len> [skip]\n"
	  "                                CRC32 of raw page + spare records" },
	{ "md", cmd_md, "md <addr> [len]               memory dump" },
	{ "memtest", cmd_memtest,
	  "memtest [addr len]            SDRAM test (destroys SDRAM contents)" },
	{ "reset", cmd_reset,
	  "reset                         chip reset via the watchdog" },
};

static void cmd_help(int argc, char **argv)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(cmds); i++) {
		printf("  %s\n", cmds[i].help);
	}
}

void shell_run(void)
{
	static char line[96];
	char *argv[MAX_ARGS];
	u32 i;
	int argc;

	for (;;) {
		con_puts("dspboot> ");
		readline(line, sizeof(line));
		argc = split(line, argv);
		if (!argc) {
			continue;
		}

		if (!strcmp(argv[0], "?")) {
			argv[0] = "help";
		}

		for (i = 0; i < ARRAY_SIZE(cmds); i++) {
			if (!strcmp(argv[0], cmds[i].name)) {
				break;
			}
		}

		if (i < ARRAY_SIZE(cmds)) {
			cmds[i].fn(argc, argv);
		} else {
			printf("unknown command '%s', try 'help'\n", argv[0]);
		}
	}
}
