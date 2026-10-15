# SPDX-License-Identifier: BSD-2-Clause
#
# The DSP firmware, built with the C6000 toolchain of dspboot's container
# ("make dspaudio" at the top of the repository). O= sets the build directory.
#
CROSS	?= tic6x-elf-
CC	:= $(CROSS)gcc
OBJCOPY	:= $(CROSS)objcopy
OBJDUMP	:= $(CROSS)objdump
SIZE	:= $(CROSS)size
O	?= build
VERSION	?= unknown
DMA	?= 1

CFLAGS	:= -march=c674x -mlittle-endian -O2 -std=gnu99 -Wall -Wextra \
	   -ffreestanding -fno-builtin -ffunction-sections -fdata-sections \
	   -fno-asynchronous-unwind-tables -fno-unwind-tables -msdata=default \
	   -I../dspd -DDSPAUDIO_VERSION='"$(VERSION)"'
ifeq ($(DMA),1)
CFLAGS	+= -DDSPAUDIO_DMA
endif

# the AC-3 decoder (liba52, GPL-2.0-or-later) needs the blocks DMA brings
AC3	?= $(DMA)
A52	:= bitstream bit_allocate crc downmix imdct parse
ifeq ($(AC3),1)
ifneq ($(DMA),1)
$(error AC3=1 needs DMA=1)
endif
CFLAGS	+= -DDSPAUDIO_AC3
# liba52 is in SDRAM, beyond the reach of a PC-relative call from L2
$(O)/dspaudio.o $(O)/lib.o: CFLAGS += -mlong-calls
OBJS_AC3 := $(O)/iec61937.o $(O)/lib.o $(addprefix $(O)/libs/liba52/,$(addsuffix .o,$(A52)))
endif
LDFLAGS	:= -nostdlib -nostartfiles -Wl,--gc-sections -Wl,--no-warn-rwx-segments \
	   -Wl,-T,dspaudio.ld -Wl,-Map,$(O)/dspaudio.map

OBJS	:= $(O)/start.o $(O)/dspaudio.o $(OBJS_AC3)

all: $(O)/dspaudio.bin $(O)/dspaudio.lst
	@$(SIZE) $(O)/dspaudio.elf

$(O)/%.o: %.c ../dspd/dspaudio.h | $(O)
	$(CC) $(CFLAGS) -c $< -o $@

# liba52 as it is: its data far from DP, long calls to libgcc in L2
$(O)/libs/liba52/%.o: libs/liba52/%.c config.h | $(O)
	@mkdir -p $(O)/libs/liba52
	$(CC) $(filter-out -Wextra -msdata=default,$(CFLAGS)) -msdata=none -mlong-calls -w -I. -Ilibs/liba52 -c $< -o $@

$(O)/iec61937.o $(O)/lib.o: iec61937.h

$(O)/%.o: %.S | $(O)
	$(CC) $(CFLAGS) -c $< -o $@

$(O)/dspaudio.elf: $(OBJS) dspaudio.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -lgcc -o $@

# raw image from 0x11800000; bss and stack are not part of it
$(O)/dspaudio.bin: $(O)/dspaudio.elf
	$(OBJCOPY) -O binary -R .ddr -R .ddr_bss $< $@
ifeq ($(AC3),1)
	$(OBJCOPY) -O binary -j .ddr $< $(O)/dspaudio.ddr
else
	rm -f $(O)/dspaudio.ddr
endif

$(O)/dspaudio.lst: $(O)/dspaudio.elf
	$(OBJDUMP) -d -h $< > $@

# the version is compiled in: rebuild when it changes, not only the sources
$(O)/version: FORCE | $(O)
	@echo '$(VERSION)' | cmp -s - $@ || echo '$(VERSION)' > $@

# and so are the build options
$(O)/options: FORCE | $(O)
	@echo 'DMA=$(DMA) AC3=$(AC3)' | cmp -s - $@ || echo 'DMA=$(DMA) AC3=$(AC3)' > $@

$(O)/dspaudio.o: $(O)/version $(O)/options

$(O):
	mkdir -p $@

clean:
	rm -rf $(O)

.PHONY: all clean FORCE

FORCE:
