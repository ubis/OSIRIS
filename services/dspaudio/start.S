; SPDX-License-Identifier: BSD-2-Clause
; dspaudio entry point: dspd points the DSP boot address here and releases
; local reset. Set up the C environment, clear .bss, call dspaudio_main().

	.section .text.start, "ax"
	.global	_start
_start:
	mvc	.s2	csr, b0
	and	.s2	-2, b0, b0		; clear GIE
	mvc	.s2	b0, csr
	zero	.s2	b0
	mvc	.s2	b0, amr			; linear addressing for all registers

	mvkl	.s2	__stack_top - 8, b15	; stack: 8-byte aligned, grows down
	mvkh	.s2	__stack_top - 8, b15
	mvkl	.s2	__c6xabi_DSBT_BASE, b14	; data page pointer for near data
	mvkh	.s2	__c6xabi_DSBT_BASE, b14

	mvkl	.s1	__bss_start, a4		; clear .bss, 8 bytes at a time
	mvkh	.s1	__bss_start, a4
	mvkl	.s1	__bss_end, a5
	mvkh	.s1	__bss_end, a5
	zero	.l1	a6
	zero	.l1	a7
1:
	cmpltu	.l1	a4, a5, a0
  [!a0]	b	.s1	2f
	nop	5
	stdw	.d1t1	a7:a6, *a4++
	b	.s1	1b
	nop	5
2:
	callp	.s2	dspaudio_main, b3
3:
	b	.s2	3b
	nop	5
