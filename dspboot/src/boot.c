/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Starting U-Boot: pick the slot from "bootslot" in the U-Boot environment,
 * load and check a dspboot image from NAND, leave the handoff record, and
 * release the ARM. Also images received over YMODEM ("loady", "bootram").
 */
#include "board.h"
#include "boot.h"
#include "crc.h"
#include "env.h"
#include "errcodes.h"
#include "hw.h"
#include "image.h"
#include "lib.h"
#include "nand.h"
#include "sdram.h"
#include "soc.h"
#include "timer.h"
#include "uart.h"
#include "ymodem.h"

static u32 last_rx_addr = BOARD_UBOOT_ADDR;

/* Starting the ARM */

/* Release the ARM at 'entry' and idle; returns only if the release failed. */
static int start_arm(u32 entry)
{
	int rc;

	printf("Starting ARM at 0x%08x\n\n", entry);
	uart_flush();
	rc = arm_release(entry);
	if (rc) {
		printf("ARM release failed: %s\n", errstr(rc));
		return rc;
	}

	for (;;) {
		__asm__ volatile("idle");
	}
}

/*
 * Check an image that sits in SDRAM at 'addr' (received over YMODEM or
 * loaded with "nand read"), move its payload into place and start it.
 * Without a header it must look like ARM code, unless 'force' is set.
 */
int boot_ram(u32 addr, int force)
{
	const u8 *p = (const u8 *)addr;
	struct img_info ii;
	u32 crc;
	int rc;

	if (!sdram_contains(addr, 64)) {
		printf("bootram: 0x%08x is not in SDRAM\n", addr);
		return -ERANGE;
	}

	rc = img_parse(p, SDRAM_BASE, SDRAM_BASE + sdram_size, &ii);
	if (ii.type == IMG_UIMAGE) {
		printf("bootram: this is a uImage (Linux); dspboot starts U-Boot, not kernels\n");
		return -ENOEXEC;
	}

	if (rc == -EBADMSG || rc == -ERANGE) {
		printf("Image: %s header invalid: %s\n", img_type_name(ii.type),
		       rc == -EBADMSG ? "header CRC" : "addresses/size");
		return rc;
	}

	if (!rc && ii.type == IMG_DBI) {
		crc = crc32_update(0, p + ii.hdr_size, ii.size);
		printf("Image: %s '%s', %u bytes, load 0x%08x, entry 0x%08x, CRC %08x %s\n",
		       img_type_name(ii.type), ii.name, ii.size, ii.load,
		       ii.entry, crc, crc == ii.dcrc ? "OK" : "MISMATCH");
		if (crc != ii.dcrc) {
			return -EBADMSG;
		}

		if (addr + ii.hdr_size != ii.load) {
			memmove((void *)ii.load, p + ii.hdr_size, ii.size);
		}

		return start_arm(ii.entry);
	}

	if (!img_looks_like_arm(p) && !force) {
		printf("Image: no header and the first word %08x is not an ARM vector;\n"
		       "       use 'bootram 0x%08x force' to start it anyway\n",
		       *(const u32 *)p, addr);
		return -ENOEXEC;
	}

	printf("Image: raw ARM binary at 0x%08x (no integrity check)\n", addr);
	return start_arm(addr);
}

/* NAND boot */

struct load_stats {
	u32 pages, corrected, erased, bad_skipped;
};

/*
 * Copy 'len' bytes starting at byte 'skip' of page 'page' to 'dst', skipping
 * bad blocks (checked when a block is entered) and refusing to go past
 * 'end_block'. ECC problems are reported, never hidden.
 */
static int nand_load(u32 page, u32 skip, u8 *dst, u32 len, u32 end_block,
		     struct load_stats *st)
{
	u32 ppb = nand.pages_per_block;

	memset(st, 0, sizeof(*st));
	while (len) {
		u32 n;
		int rc;

		if (!(page % ppb) || !st->pages) {
			while (page / ppb < end_block &&
			       nand_block_is_bad(page / ppb)) {
				;
				page = (page / ppb + 1) * ppb;
				st->bad_skipped++;
			}
		}

		if (page / ppb >= end_block) {
			printf("\n        ran past the end of the slot (block %u)\n",
			       end_block);
			return -ERANGE;
		}

		rc = nand_read_page(page, nand_pagebuf, nand_oobbuf);
		if (rc < 0 && rc != -ENODATA) {
			printf("\n        page %u (0x%08x): %s\n", page,
			       page * nand.page_size, nand_strerror(rc));
			return rc;
		}

		if (rc == -ENODATA) {
			st->erased++;
		} else if (rc > 0) {
			st->corrected += rc;
		}

		n = nand.page_size - skip;
		if (n > len) {
			n = len;
		}

		memcpy(dst, nand_pagebuf + skip, n);
		dst += n;
		len -= n;
		skip = 0;
		page++;
		st->pages++;
	}

	return 0;
}

/*
 * Handoff record: tells U-Boot (and from there Linux) which slot dspboot
 * started, so a failed slot can be noticed and the environment fixed.
 */
#define HANDOFF_MAGIC	0x42505344u	/* "DSPB" in memory */
struct dspboot_handoff {
	u32 magic;
	/* 'a' or 'b': the slot that was started */
	u8 slot;
	/* 'a' or 'b': what the environment asked for */
	u8 requested;
	u8 fallback; /* 1 if slot != requested */
	u8 env; /* 0 no valid env, 1 env without bootslot, 2 bootslot used,
					 * 3 bootslot value invalid */
	/* CRC-32 of the U-Boot payload that was started */
	u32 crc;
};

static char boot_requested = 'a';
static u8 boot_env_state;

static u32 slot_ofs(char name)
{
	return name == 'b' ? BOARD_UBOOT_B_OFS : BOARD_UBOOT_A_OFS;
}

static int env_read(u32 ofs, u8 *dst)
{
	u32 p;
	int rc;

	if (nand_block_is_bad(ofs / nand.block_size)) {
		return -EIO;
	}

	for (p = 0; p < BOARD_ENV_SIZE / nand.page_size; p++) {
		rc = nand_read_page(ofs / nand.page_size + p,
				    dst + p * nand.page_size, nand_oobbuf);
		if (rc < 0 && rc != -ENODATA) {
			return rc;
		}
	}

	return 0;
}

/* Read "bootslot" from the U-Boot environment; 'a' unless it says "b". */
static char env_bootslot(void)
{
	u8 *buf = (u8 *)BOARD_ENV_SCRATCH;
	struct env_copy c[2];
	const char *v;
	u32 n = 0;
	int pick;
	char s;

	memset(c, 0, sizeof(c));
	boot_env_state = 0;
	if (!nand.present) {
		return 'a';
	}

	if (!env_read(BOARD_ENV_OFS, buf)) {
		env_check(buf, BOARD_ENV_SIZE, 1, &c[0]);
	}

	if (!env_read(BOARD_ENV_REDUND_OFS, buf + BOARD_ENV_SIZE)) {
		env_check(buf + BOARD_ENV_SIZE, BOARD_ENV_SIZE, 1, &c[1]);
	}

	pick = env_pick(&c[0], &c[1]);
	if (pick < 0) {
		/*
		 * a U-Boot built without CONFIG_ENV_OFFSET_REDUND: single copy,
		 * no flag byte
		 */
		env_check(buf, BOARD_ENV_SIZE, 0, &c[0]);
		pick = c[0].valid ? 0 : -1;
	}

	if (pick < 0) {
		printf("Env:    no valid U-Boot environment, using slot a\n");
		return 'a';
	}

	v = env_get(&c[pick], "bootslot", &n);
	s = env_slot(v, n);
	boot_env_state = !v ? 1 : s ? 2 : 3;
	if (!v) {
		printf("Env:    bootslot not set, using slot a\n");
	} else if (!s) {
		printf("Env:    bootslot invalid, using slot a\n");
	} else {
		printf("Env:    bootslot=%c\n", s);
	}

	return s ? s : 'a';
}

static void handoff_write(char slot, u32 crc)
{
	volatile struct dspboot_handoff *h =
		(volatile struct dspboot_handoff *)BOARD_HANDOFF_ADDR;

	h->magic = HANDOFF_MAGIC;
	h->slot = slot;
	h->requested = boot_requested;
	h->fallback = slot != boot_requested;
	h->env = boot_env_state;
	h->crc = crc;
}

/*
 * Load the image in one slot, check it and start it; returns only on failure.
 */
static int boot_nand(char slot)
{
	u32 ofs = slot_ofs(slot), first, last, blk, ppb, crc;
	struct img_info ii;
	struct load_stats st;
	int rc;

	printf("Slot %c: ", slot);
	if (!nand.present) {
		printf("no NAND\n");
		return -ENODEV;
	}

	ppb = nand.pages_per_block;
	first = ofs / nand.block_size;
	last = (ofs + BOARD_UBOOT_SLOT_LEN) / nand.block_size;
	for (blk = first; blk < last; blk++) {
		if (nand_block_is_bad(blk)) {
			continue;
		}

		rc = nand_read_page(blk * ppb, nand_pagebuf, nand_oobbuf);
		if (rc == -ENODATA) {
			/* look a bit further, the image may follow a gap */
			continue;
		}

		if (rc < 0) {
			printf("header page %u: %s\n", blk * ppb,
			       nand_strerror(rc));
			return rc;
		}

		break;
	}

	if (blk >= last) {
		printf("empty\n");
		return -ENOENT;
	}

	rc = img_parse(nand_pagebuf, SDRAM_BASE, SDRAM_BASE + sdram_size, &ii);
	if (!rc && ii.type != IMG_DBI) {
		rc = -ENOEXEC; /* slots only start dspboot images */
	}

	if (rc) {
		printf("no valid image (%s, first word %08x)\n",
		       rc == -EBADMSG	     ? "header CRC"
		       : rc == -ERANGE	     ? "bad addresses/size"
		       : ii.type != IMG_NONE ? img_type_name(ii.type)
					     : "unknown header",
		       *(u32 *)nand_pagebuf);
		return rc;
	}

	printf("\"%s\", %u KiB", ii.name, (ii.size + 1023) >> 10);
	rc = nand_load(blk * ppb, ii.hdr_size, (u8 *)ii.load, ii.size, last,
		       &st);
	if (rc) {
		printf("        read failed\n");
		return rc;
	}

	crc = crc32_update(0, (void *)ii.load, ii.size);
	if (crc != ii.dcrc) {
		printf(", CRC %08x, expected %08x: invalid\n", crc, ii.dcrc);
		return -EBADMSG;
	}

	printf(", CRC OK");
	if (st.corrected) {
		printf(", %u ECC corrections", st.corrected);
	}

	if (st.bad_skipped) {
		printf(", %u bad block%s skipped", st.bad_skipped,
		       st.bad_skipped == 1 ? "" : "s");
	}

	printf("\n");
	handoff_write(slot, crc);
	return start_arm(ii.entry);
}

/* The slot the environment asks for, then the other one. */
void boot_slots(void)
{
	boot_requested = env_bootslot();
	boot_nand(boot_requested);
	boot_nand(boot_requested == 'a' ? 'b' : 'a');
}

/* YMODEM */

static int ym_getc(void *ctx, u32 timeout_ms)
{
	(void)ctx;
	return con_getc_timeout(timeout_ms);
}

static void ym_putc(void *ctx, int c)
{
	(void)ctx;
	con_putc(c);
}

int boot_loady(u32 addr)
{
	struct ym_io io = { ym_getc, ym_putc, NULL, 60 };
	struct ym_result res;
	int rc;

	if (!sdram_contains(addr, 1024)) {
		printf("loady: 0x%08x is not in SDRAM\n", addr);
		return -ERANGE;
	}

	printf("ymodem: ready, send the file now (YMODEM/XMODEM, CRC) -> 0x%08x, max %u bytes\n",
	       addr, SDRAM_BASE + sdram_size - addr);
	uart_flush();
	rc = ymodem_receive(&io, (u8 *)addr, SDRAM_BASE + sdram_size - addr,
			    &res);
	/* let the sender's terminal settle */
	mdelay(200);
	while (con_getc_timeout(20) >= 0)
		;
	if (rc) {
		printf("\nymodem: %s\n", ymodem_strerror(rc));
		return rc;
	}

	printf("\nymodem: received '%s', %u bytes (%u blocks, %u retries), CRC32 %08x\n",
	       res.name, res.len, res.blocks, res.retries,
	       crc32_update(0, (void *)addr, res.len));
	last_rx_addr = addr;
	return 0;
}

/* Start one slot on request, without falling back to the other one. */
int boot_slot(char slot)
{
	boot_requested = slot;
	boot_env_state = 0;
	return boot_nand(slot);
}

/* Where the last YMODEM transfer went, for "bootram" without an address */
u32 boot_last_rx(void)
{
	return last_rx_addr;
}
