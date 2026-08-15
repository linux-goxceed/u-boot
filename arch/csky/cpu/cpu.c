// SPDX-License-Identifier: GPL-2.0+

#include <cpu_func.h>
#include <init.h>
#include <configs/gx6702.h>
#include <asm/global_data.h>

gd_t *gd;

int print_cpuinfo(void)
{
	printf("CPU:   C-SKY CK610 (ABIV1, LE)\n");
	return 0;
}

int dram_init(void)
{
	gd->ram_size = CFG_SYS_SDRAM_SIZE;
	return 0;
}

/* The driver-model sysreset watchdog supplies reset_cpu() and do_reset(). */
#ifndef CONFIG_SYSRESET
#error "GX6702 requires CONFIG_SYSRESET for a working hardware reset"
#endif
