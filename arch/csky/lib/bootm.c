// SPDX-License-Identifier: GPL-2.0+
/*
 * C-SKY Linux kernel boot support for U-Boot bootm.
 *
 * Linux entry (arch/csky/kernel/head.S → csky_start):
 *   a0 = unused
 *   a1 = flattened device tree pointer
 */

#include <bootm.h>
#include <bootstage.h>
#include <cpu_func.h>
#include <image.h>
#include <irq_func.h>
#include <asm/cache.h>
#include <asm/global_data.h>

DECLARE_GLOBAL_DATA_PTR;

static void announce_and_cleanup(int fake)
{
	printf("\nStarting kernel ...%s\n\n", fake ?
	       "(fake run for tracing)" : "");
	bootstage_mark_name(BOOTSTAGE_ID_BOOTM_HANDOFF, "start_kernel");

	/*
	 * CK610: flush I/D caches so the kernel image and FDT are visible
	 * in DRAM for instruction fetch / early DT walk.
	 */
	disable_interrupts();
	flush_cache(0, 0);
}

static int boot_prep_linux(struct bootm_headers *images)
{
	if (CONFIG_IS_ENABLED(OF_LIBFDT) && IS_ENABLED(CONFIG_LMB) &&
	    images->ft_len) {
		if (image_setup_linux(images)) {
			printf("FDT creation failed\n");
			return -1;
		}
	} else if (!images->ft_len) {
		printf("Device tree required for C-SKY Linux boot\n");
		return -1;
	}

	return 0;
}

static void boot_jump_linux(struct bootm_headers *images, int flag)
{
	typedef void (*kernel_entry_t)(unsigned long unused, void *dtb);
	kernel_entry_t kernel = (kernel_entry_t)images->ep;
	int fake = (flag & BOOTM_STATE_OS_FAKE_GO);
	void *dtb = images->ft_addr;

	announce_and_cleanup(fake);

	if (!fake)
		kernel(0, dtb);
}

int do_bootm_linux(int flag, struct bootm_info *bmi)
{
	struct bootm_headers *images = bmi->images;

	if (flag & (BOOTM_STATE_OS_BD_T | BOOTM_STATE_OS_CMDLINE))
		return -1;

	if (flag & BOOTM_STATE_OS_PREP)
		return boot_prep_linux(images);

	if (flag & (BOOTM_STATE_OS_GO | BOOTM_STATE_OS_FAKE_GO)) {
		boot_jump_linux(images, flag);
		return 0;
	}

	return -1;
}
