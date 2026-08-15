/* SPDX-License-Identifier: GPL-2.0+ */

	.section .rodata.gxopen_fw,"a",@progbits
	.balign 4
	.global gxopen_fw
gxopen_fw:
	.incbin "../lib/nationalchip-lpc/gx6702-lpc.bin"
	.global gxopen_fw_end
gxopen_fw_end:
	.balign 4
