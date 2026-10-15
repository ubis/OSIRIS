# SPDX-License-Identifier: BSD-2-Clause
CC	?= gcc
CFLAGS	?= -Os
DSPD	?= ../dspd
override CFLAGS += -Wall -Wextra -std=gnu99 -I$(DSPD)
LDLIBS	+= -lm

pcmd: pcmd.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c $(DSPD)/dspaudio.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: pcmd
	install -D -m 0755 pcmd $(DESTDIR)/usr/sbin/pcmd

clean:
	rm -f pcmd *.o

.PHONY: install clean
