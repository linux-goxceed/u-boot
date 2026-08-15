/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __ASM_CSKY_U_BOOT_H_
#define __ASM_CSKY_U_BOOT_H_

#include <asm-generic/u-boot.h>

#define IH_ARCH_DEFAULT IH_ARCH_CSKY

struct global_data;
void csky_board_init_r(struct global_data *new_gd);

#endif /* __ASM_CSKY_U_BOOT_H_ */
