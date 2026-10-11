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

CFLAGS	:= -march=c674x -mlittle-endian -O2 -std=gnu99 -Wall -Wextra \
	   -ffreestanding -fno-builtin -ffunction-sections -fdata-sections \
	   -fno-asynchronous-unwind-tables -fno-unwind-tables -msdata=default \
	   -I../dspd -DDSPAUDIO_VERSION='"$(VERSION)"'
LDFLAGS	:= -nostdlib -nostartfiles -Wl,--gc-sections -Wl,--no-warn-rwx-segments \
	   -Wl,-T,dspaudio.ld -Wl,-Map,$(O)/dspaudio.map

OBJS	:= $(O)/start.o $(O)/dspaudio.o

all: $(O)/dspaudio.bin $(O)/dspaudio.lst
	@$(SIZE) $(O)/dspaudio.elf

$(O)/%.o: %.c ../dspd/dspaudio.h | $(O)
	$(CC) $(CFLAGS) -c $< -o $@

$(O)/%.o: %.S | $(O)
	$(CC) $(CFLAGS) -c $< -o $@

$(O)/dspaudio.elf: $(OBJS) dspaudio.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -lgcc -o $@

# raw image from 0x11800000; bss and stack are not part of it
$(O)/dspaudio.bin: $(O)/dspaudio.elf
	$(OBJCOPY) -O binary $< $@

$(O)/dspaudio.lst: $(O)/dspaudio.elf
	$(OBJDUMP) -d -h $< > $@

$(O):
	mkdir -p $@

clean:
	rm -rf $(O)

.PHONY: all clean
