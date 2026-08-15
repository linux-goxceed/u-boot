// SPDX-License-Identifier: GPL-2.0+

#include <init.h>
#include <asm/u-boot.h>
#include <asm/global_data.h>

void csky_board_init_r(gd_t *new_gd)
{
	arch_setup_gd(new_gd);
	new_gd->flags |= GD_FLG_RELOC | GD_FLG_FULL_MALLOC_INIT;
}
