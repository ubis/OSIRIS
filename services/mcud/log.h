/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * log_msg(): to the terminal when run from one, to syslog when run as a
 * daemon (stderr is then not a terminal).
 */
#ifndef SERVICE_LOG_H
#define SERVICE_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>
#include <unistd.h>

static inline void log_msg(int prio, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (isatty(STDERR_FILENO)) {
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
	} else {
		vsyslog(prio, fmt, ap);
	}

	va_end(ap);
}

#endif
