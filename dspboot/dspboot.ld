/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * dspboot runs from DSP L2 RAM. TI's sft runs at 0x11800000..0x11809000 and
 * production first stages load up to 0x1182A600, so the boot ROM leaves at
 * least those ranges alone. We start at 0x11800000 and keep the whole image
 * + stack below 0x1181A000.
 */
OUTPUT_FORMAT("elf32-tic6x-le")
ENTRY(_start)

MEMORY
{
	L2 (rwx) : ORIGIN = 0x11800000, LENGTH = 0x1A000
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

	/DISCARD/ : {
		*(.c6xabi.exidx* .c6xabi.extab* .comment .note* .ARM.attributes)
	}
}
