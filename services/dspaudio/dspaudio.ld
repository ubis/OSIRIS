/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * dspaudio runs from DSP L2 RAM at 0x11800000, where dspd loads it. Image,
 * bss and stack stay below 0x1181F000, the block shared with dspd; more
 * code and data go above it, up to the L2 cache at 0x11830000. The
 * reverb's delay lines are in the on-chip shared RAM, which Linux does not
 * use; dspaudio clears them itself. With the AC-3 decoder, liba52 and its
 * heap are in SDRAM above the A/V sync line: dspd loads that part from the
 * image's .ddr file.
 */
OUTPUT_FORMAT("elf32-tic6x-le")
ENTRY(_start)

MEMORY
{
	L2 (rwx) : ORIGIN = 0x11800000, LENGTH = 0x1F000
	L2_HI (rwx) : ORIGIN = 0x11820000, LENGTH = 0x10000	/* between the shared block and the cache */
	SHRAM (rw) : ORIGIN = 0x80000000, LENGTH = 0x20000
	DDR (rwx) : ORIGIN = 0xC3C00000, LENGTH = 0x100000
}

STACK_SIZE = 0x1400;		/* the frame chain < 1 KiB (-fstack-usage), liba52's init ~2.3 KiB */

SECTIONS
{
	/* the decoder: code, tables and data, first so nothing else takes them */
	.ddr : ALIGN(8) {
		KEEP(*(.ddr_magic))
		*libs/liba52/*(.text .text.* .const .const.* .rodata .rodata.* .switch .switch.*)
		*lib.o(.text .text.*)
		*libs/liba52/*(.fardata .fardata.* .neardata .neardata.* .data .data.*)
	} > DDR

	.ddr_bss (NOLOAD) : ALIGN(16) {
		__ddr_bss_start = .;
		*libs/liba52/*(.far .far.* .bss .bss.* COMMON)
		*(.ddr_bss .ddr_bss.*)
		. = ALIGN(8);
		__ddr_bss_end = .;
	} > DDR

	.text : {
		KEEP(*(.text.start))
		*(.text .text.*)
	} > L2

	.rodata : ALIGN(8) {
		*(.const .const.* .rodata .rodata.* .switch .switch.*)
	} > L2

	.data : ALIGN(8) {
		__c6xabi_DSBT_BASE = .;
		*(.neardata .neardata.* .data .data.*)
		*(.fardata .fardata.*)
	} > L2

	.bss (NOLOAD) : ALIGN(8) {
		__bss_start = .;
		*(.bss .bss.* .far .far.* COMMON)
		. = ALIGN(8);
		__bss_end = .;
	} > L2

	.stack (NOLOAD) : ALIGN(8) {
		. += STACK_SIZE;
		__stack_top = .;
	} > L2

	__image_end = .;
	ASSERT(__image_end <= 0x1181F000, "the image runs into the shared block")

	/*
	 * Above the shared block: loaded with the image, which then spans the
	 * block (dspd clears it after loading). The data is not cleared:
	 * whoever uses it clears it.
	 */
	.text_hi : ALIGN(32) {
		*(.text_hi .text_hi.*)
	} > L2_HI

	.l2_hi (NOLOAD) : ALIGN(8) {
		*(.l2_hi .l2_hi.*)
	} > L2_HI

	.shram (NOLOAD) : ALIGN(8) {
		__shram_start = .;
		*(.shram .shram.*)
		. = ALIGN(8);
		__shram_end = .;
	} > SHRAM
	ASSERT(__shram_end <= 0x80014000, "the reverb runs into the ring of audio from Linux")

	/DISCARD/ : {
		*(.c6xabi.exidx* .c6xabi.extab* .comment .note* .ARM.attributes)
	}
}
