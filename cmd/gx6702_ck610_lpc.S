/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * CK610 soft-standby helpers for the GX6702 LPC path (hd2015 -S …).
 *
 * Architectural STOP only holds when executed from on-chip ISRAM.  U-Boot
 * lives in DDR, so these short sequences are assembled here as readable
 * source, then copied to 0x00100400 (the eCos low-power entry) before the
 * CR18 / jsr handoff in gx6702_ck610_enter_isram(). Stock eCos 0x900f15b0
 * performs the same handoff; U-Boot installs STOP at that address.
 */

	.section .rodata.gx6702_ck610_lpc, "a"
	.balign 4

	/*
	 * Soft standby / discriminators: enter CK610 STOP, then spin if it
	 * ever returns (it should not while bit1 keeps the domain asleep).
	 */
	.global gx6702_ck610_stop_blob
	.global gx6702_ck610_stop_blob_end
gx6702_ck610_stop_blob:
	stop
1:
	br	1b
gx6702_ck610_stop_blob_end:

	.section .text.gx6702_ck610_lpc, "ax"
	.balign 4

	/*
	 * void gx6702_ck610_enter_isram(void) — does not return.
	 * Caller must have installed the STOP blob at 0x00100400 and flushed
	 * D-cache for that range (and preferably whole-cache wbinv).
	 */
	.global gx6702_ck610_enter_isram
gx6702_ck610_enter_isram:
	psrclr	ie, fe, ee
	mfcr	r7, cr18
	bclri	r7, r7, 0
	bclri	r7, r7, 1
	mtcr	r7, cr18
	/* r7 = 0x00100400 (eCos low-power entry / our ISRAM stub). */
	bgeni	r7, 10
	bseti	r7, r7, 20
	jsr	r7
2:
	br	2b
