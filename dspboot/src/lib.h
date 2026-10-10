/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef DSPBOOT_LIB_H
#define DSPBOOT_LIB_H

#include <stdarg.h>
#include "types.h"

void *memcpy(void *d, const void *s, unsigned long n);
void *memmove(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);
int memcmp(const void *a, const void *b, unsigned long n);
unsigned long strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, unsigned long n);

/*
 * Parse an unsigned number (0x.. hex, else decimal; k/K and m/M suffixes).
 * Returns 0 on error.
 */
int parse_u32(const char *s, u32 *out);

/* Console output (provided by uart.c) */
void con_putc(int c);
int con_getc_timeout(u32 timeout_ms); /* -1 on timeout */
int con_tstc(void);

void con_puts(const char *s);
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vprintf(const char *fmt, va_list ap);

void hexdump(u32 addr, const void *buf, u32 len);

#endif
