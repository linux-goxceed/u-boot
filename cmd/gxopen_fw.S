/* SPDX-License-Identifier: GPL-2.0+ */

#include <linux/kconfig.h>

	.section .rodata.gxopen_fw,"a",@progbits
	.balign 4
	.global gxopen_fw
gxopen_fw:
#ifdef CONFIG_TARGET_GX6706
	.incbin "../lib/nationalchip-lpc/gx6706-lpc.bin"
#else
	.incbin "../lib/nationalchip-lpc/gx6702-lpc.bin"
#endif
	.global gxopen_fw_end
gxopen_fw_end:
	.balign 4
