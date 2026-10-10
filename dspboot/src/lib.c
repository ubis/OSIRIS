/* SPDX-License-Identifier: BSD-2-Clause */
/* dspboot - minimal freestanding C library */
#include "errcodes.h"
#include "lib.h"

void *memcpy(void *d, const void *s, unsigned long n)
{
	u8 *dp = d;
	const u8 *sp = s;

	if ((((u32)dp | (u32)sp | n) & 3) == 0) {
		u32 *dw = d;
		const u32 *sw = s;
		for (n >>= 2; n; n--) {
			*dw++ = *sw++;
		}

		return d;
	}
	while (n--) {
		*dp++ = *sp++;
	}

	return d;
}

void *memmove(void *d, const void *s, unsigned long n)
{
	u8 *dp = d;
	const u8 *sp = s;

	if (dp <= sp || dp >= sp + n) {
		return memcpy(d, s, n);
	}

	dp += n;
	sp += n;
	while (n--) {
		*--dp = *--sp;
	}

	return d;
}

void *memset(void *d, int c, unsigned long n)
{
	u8 *dp = d;

	while (n--) {
		*dp++ = (u8)c;
	}

	return d;
}

int memcmp(const void *a, const void *b, unsigned long n)
{
	const u8 *x = a, *y = b;

	for (; n; n--, x++, y++) {
		if (*x != *y) {
			return *x - *y;
		}
	}

	return 0;
}

unsigned long strlen(const char *s)
{
	const char *p = s;

	while (*p) {
		p++;
	}

	return p - s;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++, b++;
	}

	return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, unsigned long n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b) {
			return (u8)*a - (u8)*b;
		}

		if (!*a) {
			break;
		}
	}

	return 0;
}

int parse_u32(const char *s, u32 *out)
{
	u32 v = 0, base = 10;
	int digits = 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}

	for (; *s; s++) {
		u32 d;
		char c = *s;
		if (c >= '0' && c <= '9') {
			d = c - '0';
		} else if (base == 16 && c >= 'a' && c <= 'f') {
			d = c - 'a' + 10;
		} else if (base == 16 && c >= 'A' && c <= 'F') {
			d = c - 'A' + 10;
		} else {
			break;
		}

		v = v * base + d;
		digits++;
	}

	if (!digits) {
		return 0;
	}

	if (*s == 'k' || *s == 'K') {
		v <<= 10;
		s++;
	} else if (*s == 'm' || *s == 'M') {
		v <<= 20;
		s++;
	}

	if (*s) {
		return 0;
	}

	*out = v;
	return 1;
}

void con_puts(const char *s)
{
	while (*s) {
		if (*s == '\n') {
			con_putc('\r');
		}

		con_putc(*s++);
	}
}

static void put_num(u32 v, u32 base, int width, int zero, int upper)
{
	char buf[12];
	const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	int n = 0;

	do {
		buf[n++] = dig[v % base];
		v /= base;
	} while (v);
	while (n < width--) {
		con_putc(zero ? '0' : ' ');
	}
	while (n) {
		con_putc(buf[--n]);
	}
}

int vprintf(const char *fmt, va_list ap)
{
	for (; *fmt; fmt++) {
		int width = 0, zero = 0, left = 0;

		if (*fmt != '%') {
			if (*fmt == '\n') {
				con_putc('\r');
			}

			con_putc(*fmt);
			continue;
		}

		fmt++;
		if (*fmt == '-') {
			left = 1;
			fmt++;
		}

		if (*fmt == '0') {
			zero = 1;
			fmt++;
		}
		while (*fmt >= '0' && *fmt <= '9') {
			width = width * 10 + (*fmt++ - '0');
		}

		if (*fmt == 'l') {
			fmt++;
		}

		switch (*fmt) {
		case 'd': {
			s32 v = va_arg(ap, s32);
			if (v < 0) {
				con_putc('-');
				v = -v;
				if (width) {
					width--;
				}
			}

			put_num((u32)v, 10, width, zero, 0);
			break;
		}
		case 'u': put_num(va_arg(ap, u32), 10, width, zero, 0); break;
		case 'x':
		case 'X':
			put_num(va_arg(ap, u32), 16, width, zero, *fmt == 'X');
			break;
		case 'p':
			con_puts("0x");
			put_num(va_arg(ap, u32), 16, 8, 1, 0);
			break;
		case 'c': con_putc(va_arg(ap, int)); break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			int len = strlen(s);
			if (!left) {
				while (len < width--) {
					con_putc(' ');
				}
			}

			con_puts(s);
			if (left) {
				while (len < width--) {
					con_putc(' ');
				}
			}

			break;
		}
		case '%': con_putc('%'); break;
		default:
			con_putc('%');
			con_putc(*fmt);
			break;
		}
	}

	return 0;
}

int printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	return 0;
}

void hexdump(u32 addr, const void *buf, u32 len)
{
	const u8 *p = buf;
	u32 i, j;

	for (i = 0; i < len; i += 16) {
		printf("%08x: ", addr + i);
		for (j = 0; j < 16; j++) {
			if (i + j < len) {
				printf("%02x ", p[i + j]);
			} else {
				con_puts("   ");
			}
		}

		con_putc(' ');
		for (j = 0; j < 16 && i + j < len; j++) {
			u8 c = p[i + j];
			con_putc(c >= 32 && c < 127 ? c : '.');
		}

		con_puts("\n");
	}
}

const char *errstr(int err)
{
	switch (-err) {
	case 0: return "ok";
	case ENOENT: return "not found";
	case EIO: return "I/O error";
	case ENOEXEC: return "not a bootable image";
	case ENODEV: return "no device";
	case EINVAL: return "invalid argument";
	case EFBIG: return "too large";
	case EROFS: return "write protected";
	case ERANGE: return "out of range";
	case ENOMSG: return "no ECC bytes";
	case ENODATA: return "erased";
	case EPROTO: return "protocol error";
	case EBADMSG: return "checksum error";
	case EOPNOTSUPP: return "not supported";
	case ETIMEDOUT: return "timeout";
	case ECANCELED: return "cancelled";
	default: return "error";
	}
}
