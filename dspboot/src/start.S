; SPDX-License-Identifier: BSD-2-Clause
; dspboot entry point. The boot ROM jumps here (AIS JUMP_CLOSE) with
; interrupts disabled; we set up the C environment and call dspboot_main().

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

	callp	.s2	dspboot_main, b3
1:
	b	.s2	1b
	nop	5
