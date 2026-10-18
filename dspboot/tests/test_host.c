/* SPDX-License-Identifier: BSD-2-Clause */
/* Native test driver for the hardware-independent dspboot modules. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crc.h"
#include "env.h"
#include "image.h"
#include "nand.h"

static unsigned char *slurp(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	unsigned char *b;
	if (!f) {
		perror(path);
		exit(2);
	}

	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc(*len + 64);
	memset(b, 0, *len + 64);
	if (fread(b, 1, *len, f) != (size_t)*len) {
		exit(2);
	}

	fclose(f);
	return b;
}

static void print_geom(int rc, const struct nand_info *n)
{
	if (rc) {
		printf("rc=%d %s\n", rc, n->why);
	} else {
		printf("page=%u spare=%u steps=%u ppb=%u block=%uK blocks=%u cyc=%u+%u onfi=%d\n",
		       n->page_size, n->oob_size, n->steps, n->pages_per_block,
		       n->block_size >> 10, n->blocks, n->col_cycles,
		       n->row_cycles, n->onfi);
	}
}

int main(int argc, char **argv)
{
	long len;
	unsigned char *b;

	if (argc == 3 && !strcmp(argv[1], "crc32")) {
		b = slurp(argv[2], &len);
		printf("%08x\n", crc32_update(0, b, len));
	} else if (argc == 3 && !strcmp(argv[1], "crc16")) {
		b = slurp(argv[2], &len);
		printf("%04x\n", crc16_xmodem(b, len));
	} else if (argc == 3 && !strcmp(argv[1], "unpack")) {
		unsigned char e[10];
		u16 v[8];
		int i;
		for (i = 0; i < 10; i++) {
			sscanf(argv[2] + 2 * i, "%2hhx", &e[i]);
		}

		nand_ecc_unpack(e, v);
		for (i = 0; i < 8; i++) {
			printf("%03x%s", v[i], i == 7 ? "\n" : " ");
		}
	} else if (argc == 3 && !strcmp(argv[1], "img")) {
		struct img_info ii;
		int rc;
		b = slurp(argv[2], &len);
		rc = img_parse(b, 0xC0000000u, 0xC4000000u, &ii);
		printf("rc=%d type=%d hdr=%u size=%u load=%08x entry=%08x crc=%08x name=%s",
		       rc, ii.type, ii.hdr_size, ii.size, ii.load, ii.entry,
		       ii.dcrc, ii.name);
		if (!rc && ii.has_crc) {
			printf(" payload=%s", crc32_update(0, b + ii.hdr_size,
							   ii.size) == ii.dcrc
						      ? "OK"
						      : "BAD");
		}

		printf("\n");
	} else if (argc == 3 && !strcmp(argv[1], "geom")) {
		/* geom HEX8: READ ID bytes 0..3 */
		struct nand_info n;
		unsigned char id[5] = { 0 };
		int i, rc;

		memset(&n, 0, sizeof(n));
		for (i = 0; i < 4; i++) {
			sscanf(argv[2] + 2 * i, "%2hhx", &id[i]);
		}

		rc = nand_decode_id(id, &n);
		print_geom(rc, &n);
	} else if (argc == 3 && !strcmp(argv[1], "onfi")) {
		/* onfi FILE: a 256-byte ONFI parameter page */
		struct nand_info n;
		unsigned char pp[256] = { 0 };
		FILE *f = fopen(argv[2], "rb");

		if (!f || fread(pp, 1, sizeof(pp), f) != sizeof(pp)) {
			fprintf(stderr, "cannot read %s\n", argv[2]);
			return 1;
		}

		fclose(f);
		memset(&n, 0, sizeof(n));
		print_geom(nand_decode_onfi(pp, &n), &n);
	} else if ((argc == 4 || argc == 5) && !strcmp(argv[1], "env")) {
		/*
		 * env NAME FILE [FILE2]: one file = single copy, two =
		 * redundant pair
		 */
		struct env_copy c[2];
		const char *v;
		long len2;
		unsigned char *b2;
		u32 n = 0;
		int pick;

		memset(c, 0, sizeof(c));
		b = slurp(argv[3], &len);
		env_check(b, len, argc == 5, &c[0]);
		if (argc == 5) {
			b2 = slurp(argv[4], &len2);
			env_check(b2, len2, 1, &c[1]);
		}

		pick = env_pick(&c[0], &c[1]);
		v = pick < 0 ? NULL : env_get(&c[pick], argv[2], &n);
		printf("pick=%d value=%.*s slot=%c\n", pick, v ? (int)n : 6,
		       v ? v : "(none)", env_slot(v, n) ? env_slot(v, n) : '-');
	} else {
		fprintf(stderr,
			"usage: test_host crc32|crc16|img FILE | unpack HEX20 | geom HEX8 | onfi FILE | env NAME FILE [FILE2]\n");
		return 2;
	}

	return 0;
}
