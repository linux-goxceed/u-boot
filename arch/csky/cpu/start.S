/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * GX6702 / C-SKY CK610 (ABIV1) startup.
 *
 * U-Boot is loaded into DDR by a GX6702 IPL (the open gxipl implementation or
 * the legacy vendor stage-1).  The IPL has already brought up clocks, cache,
 * MMU and DDR before it jumps to us, so we only need a stack and a call into
 * the generic board_init_f() path.
 *
 * ABIV1 register convention (csky-linux-gcc):
 *   r0  = stack pointer
 *   r2  = first function argument
 *   r15 = link register
 */

#include <asm-offsets.h>
#include <config.h>
#include <configs/gx6702.h>

	.section .text.start, "ax"
	.global _start
_start:
	/*
	 * Stage-2 UART load copies the image but does not clear .bss.  Garbage
	 * here breaks malloc/env/stdio after initr_env().
	 */
	lrw   r5, __bss_start
	lrw   r6, __bss_end
	movi  r1, 0
0:
	cmpne r5, r6
	bf    1f
	st.w  r1, (r5, 0)
	addi  r5, 4
	br    0b
1:
	lrw   r1, CFG_SYS_INIT_SP_ADDR
	mov   r0, r1                    /* SP = r0 */

	/* board_init_f_alloc_reserve(ulong top) -> new SP in r2 */
	mov   r2, r0
	lrw   r5, board_init_f_alloc_reserve
	jsr   r5
	mov   r0, r2

	mov   r2, r0
	lrw   r5, board_init_f_init_reserve
	jsr   r5

	/* gd = r8 must be set after init_reserve (epilogue restores r8) */
	mov   r2, r0
	lrw   r5, arch_setup_gd
	jsr   r5

	movi  r2, 0
	lrw   r5, board_init_f
	jsr   r5

#ifdef CONFIG_SKIP_RELOCATE
	/*
	 * MONITOR_IS_IN_RAM + SKIP_RELOC: board_init_f returns here (like ARC).
	 * Switch to the final stack/gd and enter board_init_r().
	 */
	lrw   r1, gd
	ld    r6, (r1, 0)
	ld    r2, (r6, GD_NEW_GD)
	ld    r0, (r2, GD_START_ADDR_SP)
	ld    r3, (r2, GD_RELOCADDR)
	st    r2, (r1, 0)
	mov   r8, r2
	lrw   r5, board_init_r
	jsr   r5
#endif

1:
	br    1b

	.text
	.global arch_setup_gd
/*
 * void arch_setup_gd(gd_t *gd_ptr) — gd lives in r8 (ABIV1).
 * csky-linux-gcc 4.5.1 cannot assign a global register variable from C
 * (the function epilogue restores the old r8), so this must be asm-only.
 */
arch_setup_gd:
	lrw   r1, gd
	st    r2, (r1, 0)
	mov   r8, r2
	jmp   r15

	.text
	.global relocate_code
relocate_code:
	/*
	 * void relocate_code(ulong sp, gd_t *gd, ulong dest)
	 *   r2 = sp, r3 = gd, r4 = dest  (CK610 first args in r2,r3,r4)
	 */
	mov   r0, r2                    /* SP = r0 */

#ifndef CONFIG_SKIP_RELOCATE
	lrw   r5, __bss_start
	lrw   r6, __bss_end
	movi  r1, 0
2:
	cmpne r5, r6
	bf    3f
	st.w  r1, (r5, 0)               /* zero .bss word */
	addi  r5, r5, 4
	br    2b
3:
#endif
	mov   r8, r3                    /* gd = new_gd (r8 is not set by C) */
	lrw   r1, gd
	st    r3, (r1, 0)
	mov   r2, r3                    /* board_init_r(new_gd, dest) */
	mov   r3, r4
	lrw   r5, board_init_r
	jsr   r5
4:
	br    4b
