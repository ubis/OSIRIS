# SPDX-License-Identifier: BSD-2-Clause
CC	?= gcc
CFLAGS	?= -Os
override CFLAGS += -Wall -Wextra -Wno-unused-parameter -std=gnu99
LDLIBS	+= -lubus -lubox -lrt

mcud: mcud.o frames.o methods.o link.o proto.o display.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c cmd.h display.h link.h log.h mcud.h proto.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: mcud
	install -D -m 0755 mcud $(DESTDIR)/usr/sbin/mcud

clean:
	rm -f mcud *.o

.PHONY: install clean
