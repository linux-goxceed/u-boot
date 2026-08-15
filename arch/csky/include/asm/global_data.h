/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __ASM_CSKY_GLOBALDATA_H_
#define __ASM_CSKY_GLOBALDATA_H_

#include <linux/types.h>
#include <asm/u-boot.h>

struct arch_global_data {
};

#include <asm-generic/global_data.h>

/*
 * ABIV1 reserves r8 for gd in the toolchain, but newlib printf paths do not
 * preserve r8 across calls.  Keep gd in memory (like ARM) so clobbered r8 does
 * not lose the global-data pointer mid board_init_f().
 */
#define DECLARE_GLOBAL_DATA_PTR     extern gd_t *gd

#endif
