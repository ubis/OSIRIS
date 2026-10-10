/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Run the dspboot YMODEM receiver on a tty/pty: ymodem_rx TTY OUTFILE MAXBYTES
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include "ymodem.h"

static int fd;

static int t_getc(void *ctx, u32 ms)
{
	fd_set s;
	struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	unsigned char c;
	FD_ZERO(&s);
	FD_SET(fd, &s);
	if (select(fd + 1, &s, NULL, NULL, &tv) <= 0) {
		return -1;
	}

	return read(fd, &c, 1) == 1 ? c : -1;
}

static void t_putc(void *ctx, int c)
{
	unsigned char b = c;
	if (write(fd, &b, 1) != 1) {
		exit(3);
	}
}

int main(int argc, char **argv)
{
	struct termios t;
	struct ym_io io = { t_getc, t_putc, NULL, 10 };
	struct ym_result r;
	unsigned long max;
	unsigned char *buf;
	int rc;
	FILE *f;

	if (argc != 4) {
		return 2;
	}

	fd = open(argv[1], O_RDWR | O_NOCTTY);
	if (fd < 0) {
		perror(argv[1]);
		return 2;
	}

	tcgetattr(fd, &t);
	cfmakeraw(&t);
	tcsetattr(fd, TCSANOW, &t);
	max = strtoul(argv[3], NULL, 0);
	buf = malloc(max + 1024);
	rc = ymodem_receive(&io, buf, max, &r);
	fprintf(stderr,
		"rc=%d (%s) name=%s len=%u size=%u blocks=%u retries=%u\n", rc,
		ymodem_strerror(rc), r.name, r.len, r.file_size, r.blocks,
		r.retries);
	if (!rc) {
		f = fopen(argv[2], "wb");
		fwrite(buf, 1, r.len, f);
		fclose(f);
	}

	return rc ? 1 : 0;
}
