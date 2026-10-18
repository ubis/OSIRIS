# SPDX-License-Identifier: BSD-2-Clause
#
# Normally run from the top of the repository: "make dspboot" builds the
# toolchain container when needed and runs this Makefile inside it.
#
#   make [BOARD=<name>]   build one board (default: every board in boards/)
#   make test             host-side unit tests
#   make dist DIST=<dir>  the images of every board into <dir>
#
# O=<dir> puts the build output there instead of build/.
#
CROSS   ?= tic6x-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size
PYTHON  ?= python3
HOSTCC  ?= gcc

O       ?= build
BOARDS  := $(filter-out template,$(basename $(notdir $(wildcard boards/*.h))))

# Version and build date come from git; the top-level Makefile passes them in
# because the container has no git.
ifeq ($(origin VERSION),undefined)
VERSION_EPOCH := $(shell sh tools/version.sh)
VERSION := $(word 1,$(VERSION_EPOCH))
SOURCE_DATE_EPOCH ?= $(word 2,$(VERSION_EPOCH))
endif
export SOURCE_DATE_EPOCH

CFLAGS  := -march=c674x -mlittle-endian -Os -std=gnu99 -Wall -Wextra -Wno-unused-parameter \
           -ffreestanding -fno-builtin -ffunction-sections -fdata-sections \
           -fno-asynchronous-unwind-tables -fno-unwind-tables -msdata=default \
           -DDSPBOOT_VERSION='"$(VERSION)"' $(EXTRA_CFLAGS)

SRCS := src/start.S src/main.c src/soc.c src/sdram.c src/boot.c src/shell.c src/uart.c src/timer.c \
        src/nand.c src/nand_ecc.c src/nand_id.c src/ymodem.c src/crc.c src/image.c src/lib.c src/env.c

ifeq ($(BOARD),)

all: $(addprefix board-,$(BOARDS))
board-%:
	@$(MAKE) --no-print-directory BOARD=$* VERSION=$(VERSION) SOURCE_DATE_EPOCH=$(SOURCE_DATE_EPOCH)

else

ifeq ($(wildcard boards/$(BOARD).h),)
$(error unknown board '$(BOARD)', have: $(BOARDS))
endif

B       := $(O)/$(BOARD)
OBJS    := $(patsubst src/%,$(B)/%.o,$(SRCS))
BCFLAGS := $(CFLAGS) -DBOARD_FILE='"../boards/$(BOARD).h"'
LDFLAGS := -nostdlib -nostartfiles -Wl,--gc-sections -Wl,--no-warn-rwx-segments \
           -Wl,-T,dspboot.ld -Wl,-Map,$(B)/dspboot.map

all: $(B)/dspboot.ais $(B)/dspboot.lst
	@echo "$(BOARD): dspboot $(VERSION)"
	@$(SIZE) $(B)/dspboot.elf | tail -1
	@$(PYTHON) tools/mkais.py --info $(B)/dspboot.elf

# rebuild when the version or the board file changes
$(B)/.config: FORCE | $(B)
	@echo '$(VERSION) $(SOURCE_DATE_EPOCH) $(BCFLAGS)' | cmp -s - $@ || \
		echo '$(VERSION) $(SOURCE_DATE_EPOCH) $(BCFLAGS)' > $@

$(B)/%.c.o: src/%.c $(wildcard src/*.h) boards/$(BOARD).h $(B)/.config | $(B)
	$(CC) $(BCFLAGS) -c $< -o $@
$(B)/%.S.o: src/%.S $(B)/.config | $(B)
	$(CC) $(BCFLAGS) -c $< -o $@

$(B)/dspboot.elf: $(OBJS) dspboot.ld
	$(CC) $(BCFLAGS) $(LDFLAGS) $(OBJS) -lgcc -o $@

$(B)/dspboot.ais: $(B)/dspboot.elf tools/mkais.py
	$(PYTHON) tools/mkais.py --crc $< -o $@

$(B)/dspboot.lst: $(B)/dspboot.elf
	$(OBJDUMP) -d -h $< > $@

$(B):
	mkdir -p $@

endif

DIST ?= dist
dist: all
	rm -rf $(DIST)
	mkdir -p $(DIST)
	for b in $(if $(BOARD),$(BOARD),$(BOARDS)); do \
		mkdir -p $(DIST)/$$b && \
		cp $(O)/$$b/dspboot.ais $(DIST)/$$b/ || exit 1; \
	done
	cd $(DIST) && sha256sum */dspboot.ais > SHA256SUMS
	echo "dspboot $(VERSION)" > $(DIST)/VERSION

# Host-side unit tests (native gcc + python3)
TB := $(O)/test
test:
	mkdir -p $(TB)
	$(HOSTCC) -O2 -Wall -Isrc -o $(TB)/test_host tests/test_host.c src/ymodem.c src/crc.c \
		src/image.c src/nand_ecc.c src/nand_id.c src/env.c
	$(HOSTCC) -O2 -Wall -Isrc -o $(TB)/ymodem_rx tests/ymodem_rx.c src/ymodem.c src/crc.c
	DSPBOOT_BUILD=$(O) $(PYTHON) tests/run_tests.py

clean:
	rm -rf $(O)

FORCE:
.PHONY: all test clean dist FORCE
