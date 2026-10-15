/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * dspaudio runs from DSP L2 RAM at 0x11800000, where dspd loads it. Image,
 * bss and stack stay below 0x1181F000, the block shared with dspd. The
 * reverb's delay lines are in the on-chip shared RAM, which Linux does not
 * use; dspaudio clears them itself.
 */
OUTPUT_FORMAT("elf32-tic6x-le")
ENTRY(_start)

MEMORY
{
	L2 (rwx) : ORIGIN = 0x11800000, LENGTH = 0x1F000
	SHRAM (rw) : ORIGIN = 0x80000000, LENGTH = 0x20000
}

STACK_SIZE = 0x1000;		/* worst-case frame chain measured with -fstack-usage: < 1 KiB */

SECTIONS
{
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
