# SPDX-License-Identifier: BSD-2-Clause
CC	?= gcc
CFLAGS	?= -Os
override CFLAGS += -Wall -Wextra -Wno-unused-parameter -std=gnu99
LDLIBS	+= -lubus -lubox -lm

dspd: dspd.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

dspd.o: dspd.c dspaudio.h log.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: dspd
	install -D -m 0755 dspd $(DESTDIR)/usr/sbin/dspd

clean:
	rm -f dspd *.o

.PHONY: install clean
