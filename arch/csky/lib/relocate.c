// SPDX-License-Identifier: GPL-2.0+

#include <init.h>
#include <asm/global_data.h>
#include <asm/sections.h>

DECLARE_GLOBAL_DATA_PTR;

int copy_uboot_to_ram(void)
{
	if (gd->flags & GD_FLG_SKIP_RELOC)
		return 0;

	return 0;
}

int clear_bss(void)
{
	if (gd->flags & GD_FLG_SKIP_RELOC)
		return 0;

	return 0;
}
