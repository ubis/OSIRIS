/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * SDRAM on EMIFB: controller set-up from the board values, size probe, a
 * quick bus test with retries while a cold board settles, and the full
 * memory test behind "memtest".
 */
#include "board.h"
#include "errcodes.h"
#include "hw.h"
#include "lib.h"
#include "sdram.h"
#include "soc.h"
#include "timer.h"

u32 sdram_size;

static int sdram_init(void)
{
	int i;

	/* EMIFB pins (TI device.c, 32-bit bus) */
	/* nEMB_WE/RAS/CAS/CS0, EMB_CLK, EMB_SDCKE */
	pinmux_set(0, 0xFFFFFF00, 0x11111100);
	/* EMB_A[0..5], EMB_BA[0..1] */
	pinmux_set(1, 0xFFFFFFFF, 0x11111111);
	pinmux_set(2, 0x0FFFFFFF, 0x01111111); /* EMB_A[6..12] */
	pinmux_set(5, 0xFFFFFFF0, 0x11111110); /* EMB_D[0..6] */
	pinmux_set(6, 0xFFFFFFFF, 0x11111111); /* EMB_D[7..14] */
	/* EMB_D[15], nEMB_WE_DQM[0..1] */
	pinmux_set(7, 0x00000FFF, 0x00000111);
	if (!(BOARD_SDCR & (1u << 14))) { /* NM = 0: 32-bit */
		pinmux_set(2, 0xF0000000, 0x10000000); /* EMB_D[31] */
		pinmux_set(3, 0xFFFFFFFF, 0x11111111); /* EMB_D[23..30] */
		/* EMB_D[16..22], nEMB_WE_DQM[3] */
		pinmux_set(4, 0xFFFFFFFF, 0x11111111);
		pinmux_set(5, 0x0000000F, 0x00000001); /* nEMB_WE_DQM[2] */
	}

	if (psc_set(1, LPSC1_EMIFB, PSC_STATE_ENABLE)) {
		return -ETIMEDOUT;
	}

	/*
	 * EMB_CLK runs from here. The SDRAM wants 100-200 us of stable clock
	 * before its init sequence, and the EMIFB starts that sequence on every
	 * SDCR write, so wait first. Then load the timings with TIMUNLOCK set
	 * and write SDCR once more with it cleared: that last write runs the
	 * init sequence (precharge, refreshes, mode register) with the real
	 * timings and refresh rate instead of the reset defaults. Starting the
	 * SDRAM right after its clock came up gave errors on cold boots.
	 */
	udelay(200);
	EMIFB_SDCR = BOARD_SDCR | SDCR_TIMUNLOCK;
	EMIFB_SDTIMR = BOARD_SDTIMR;
	EMIFB_SDTIMR2 = BOARD_SDTIMR2;
	EMIFB_SDRCR = BOARD_SDRCR;
	EMIFB_SDCR = BOARD_SDCR & ~SDCR_TIMUNLOCK;
	/* init sequence: well under 100 us */
	udelay(100);
	for (i = 0; i < 8; i++) { /* settle the bus before testing */
		(void)*(volatile u32 *)(SDRAM_BASE + 4 * i);
	}

	return 0;
}

/* Size by address wrap-around: write at growing offsets until base changes. */
static u32 sdram_probe(void)
{
	volatile u32 *base = (volatile u32 *)SDRAM_BASE;
	u32 size;

	base[0] = 0x12345678;
	base[1] = 0xEDCBA987;
	if (base[0] != 0x12345678 || base[1] != 0xEDCBA987) {
		return 0;
	}

	for (size = 1u << 20; size < SDRAM_MAX; size <<= 1) {
		volatile u32 *p = (volatile u32 *)(SDRAM_BASE + size);
		base[0] = 0x12345678;
		*p = 0xA5A50000u | (size >> 20);
		if (base[0] != 0x12345678) {
			/* address wrapped: chip ends here */
			return size;
		}
	}

	return SDRAM_MAX;
}

/* Data bus, byte lanes, address bus. Returns 0 on success. */
static int sdram_quick_test(u32 size)
{
	volatile u32 *w = (volatile u32 *)SDRAM_BASE;
	volatile u8 *b = (volatile u8 *)SDRAM_BASE;
	u32 bit, ofs, i;

	for (bit = 0; bit < 32; bit++) {
		w[0] = 1u << bit;
		/* drive the bus to the opposite value */
		w[1] = ~(1u << bit);
		if (w[0] != (1u << bit)) {
			printf("FAILED: data bus bit %u, wrote %08x read %08x\n",
			       bit, 1u << bit, w[0]);
			return -EIO;
		}
	}

	w[0] = 0;
	b[0] = 0x11;
	b[1] = 0x22;
	b[2] = 0x33;
	b[3] = 0x44;
	if (w[0] != 0x44332211) {
		printf("FAILED: byte lanes, wrote 11 22 33 44 read %08x\n",
		       w[0]);
		return -EIO;
	}

	for (ofs = 4; ofs < size; ofs <<= 1) {
		w[ofs >> 2] = 0x55AA0000u ^ ofs;
	}

	w[0] = 0x55AA0000u;
	for (ofs = 4; ofs < size; ofs <<= 1) {
		if (w[ofs >> 2] != (0x55AA0000u ^ ofs)) {
			printf("FAILED: address bus, offset %08x reads %08x\n",
			       ofs, w[ofs >> 2]);
			return -EIO;
		}
	}

	/* a short pattern run over the first 64 KiB */
	for (i = 0; i < 16384; i++) {
		w[i] = i * 0x9E3779B9u;
	}

	for (i = 0; i < 16384; i++) {
		if (w[i] != i * 0x9E3779B9u) {
			printf("FAILED: pattern at %08x, read %08x\n",
			       SDRAM_BASE + 4 * i, w[i]);
			return -EIO;
		}
	}

	return 0;
}

int sdram_memtest(u32 start, u32 len)
{
	volatile u32 *p = (volatile u32 *)start;
	u32 n = len >> 2, i, pass, errs = 0;
	static const u32 seed[3] = { 0x00000000, 0xFFFFFFFF, 0x9E3779B9 };

	printf("memtest 0x%08x..0x%08x\n", start, start + len - 1);
	for (pass = 0; pass < 4 && errs < 10; pass++) {
		printf("  pass %u: %s ", pass + 1,
		       pass == 0   ? "address as data"
		       : pass == 1 ? "inverted address"
		       : pass == 2 ? "zeros/ones"
				   : "pseudo random");
		for (i = 0; i < n; i++) {
			u32 v = pass == 0   ? start + 4 * i
				: pass == 1 ? ~(start + 4 * i)
				: pass == 2 ? seed[i & 1]
					    : (i + 1) * seed[2];
			p[i] = v;
			if ((i & 0x3FFFF) == 0) {
				con_putc('.');
			}
		}

		con_putc(' ');
		for (i = 0; i < n && errs < 10; i++) {
			u32 v = pass == 0   ? start + 4 * i
				: pass == 1 ? ~(start + 4 * i)
				: pass == 2 ? seed[i & 1]
					    : (i + 1) * seed[2];
			if (p[i] != v) {
				printf("\n    FAIL at %08x: wrote %08x read %08x",
				       start + 4 * i, v, p[i]);
				errs++;
			}

			if ((i & 0x3FFFF) == 0) {
				con_putc('+');
			}
		}

		printf("\n");
	}

	printf("memtest %s\n", errs ? "FAILED" : "PASS");
	return errs ? -EIO : 0;
}

/*
 * Bring up and check the SDRAM, retrying while a cold board settles. 0 =
 * usable.
 */
int sdram_bringup(void)
{
	u32 tries;
	int rc;

	printf("DRAM:   ");
	for (tries = 1;; tries++) {
		if (sdram_init()) {
			printf("SDRAM controller did not power up\n");
			return -ETIMEDOUT;
		}

		sdram_size = sdram_probe();
		if (!sdram_size) {
			printf("not responding\n");
			rc = -ENODEV;
		} else {
			printf("%u MiB, quick test ", sdram_size >> 20);
			rc = sdram_quick_test(sdram_size);
			if (!rc) {
				printf("OK\n");
			}
		}

		if (!rc || tries == BOARD_SDRAM_TRIES) {
			return rc;
		}

		/* cold SDRAM: give it time, then start over */
		mdelay(1000);
		printf("DRAM:   retry %u/%u: ", tries + 1, BOARD_SDRAM_TRIES);
	}
}

int sdram_contains(u32 addr, u32 len)
{
	return addr >= SDRAM_BASE && len <= sdram_size &&
	       addr - SDRAM_BASE <= sdram_size - len;
}
