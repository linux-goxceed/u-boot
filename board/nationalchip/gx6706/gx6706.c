// SPDX-License-Identifier: GPL-2.0+

#include <init.h>
#include <gx_sysinfo.h>
#include <spi_flash.h>
#include <time.h>
#include <usb.h>
#include <asm/io.h>
#include "gx6706_usb.h"

/*
 * USB clock programming can stop the free-running counter.  udelay() would
 * then spin forever inside the EHCI reset handshake.  After that programming,
 * gx6706_timer_check() records whether the counter still moves.
 */
static int ticks_frozen = -1;

void gx6706_timer_check(void)
{
	u32 before, after;
	volatile u32 i;

	before = readl(0xa020a044);
	for (i = 0; i < 200000; i++)
		;
	after = readl(0xa020a044);
	ticks_frozen = (before == after);
}

void __udelay(unsigned long usec)
{
	uint64_t start, target;
	volatile u32 i;

	if (!usec)
		return;
	if (ticks_frozen < 0)
		gx6706_timer_check();
	if (ticks_frozen) {
		for (i = 0; i < usec * 400; i++)
			;
		return;
	}
	start = get_ticks();
	target = start + usec_to_tick(usec);
	while (get_ticks() < target + 1)
		;
}

/*
 * U-Boot stays at CONFIG_TEXT_BASE (SKIP_RELOCATE). Clip usable RAM there so
 * later allocations cannot overwrite the monitor.
 */
phys_addr_t board_get_usable_ram_top(phys_size_t total_size)
{
	return CONFIG_TEXT_BASE;
}

int board_early_init_f(void)
{
	return 0;
}

int board_late_init(void)
{
	struct spi_flash *flash;

	gx_sysinfo_print_board();

	flash = spi_flash_probe(CONFIG_SF_DEFAULT_BUS, CONFIG_SF_DEFAULT_CS,
				CONFIG_SF_DEFAULT_SPEED, CONFIG_SF_DEFAULT_MODE);
	if (flash) {
		printf("SPI flash: %s, size %u MiB\n", flash->name,
		       (unsigned int)(flash->size >> 20));
		spi_flash_free(flash);
	} else {
		printf("SPI flash probe failed\n");
	}

	return 0;
}

int board_usb_init(int index, enum usb_init_type init)
{
	if (init != USB_INIT_HOST)
		return 0;

	return gx6706_usb_enable();
}

int board_init(void)
{
	return 0;
}
