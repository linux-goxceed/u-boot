// SPDX-License-Identifier: GPL-2.0+

#include <init.h>
#include <spi_flash.h>
#include <usb.h>
#include <asm/io.h>
#include <asm/global_data.h>
#include "gx6702_usb.h"

DECLARE_GLOBAL_DATA_PTR;

/*
 * U-Boot is XIP-ish in DDR at CONFIG_TEXT_BASE (SKIP_RELOCATE).  Video reserves
 * ~13 MiB downward from ram_top; if ram_top is the true DRAM end (0x94000000)
 * that reservation overlaps the U-Boot image and gx6702_fill_black() wipes us.
 * Clip usable top to TEXT_BASE so FB/malloc sit below the monitor.
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

	printf("Board: NationalChip GX6702 (Gemini 6702H5)\n");
	printf("SPI HW ID: %08x %08x %08x (cfg=%08x)\n",
	       readl(0xA0F80050), readl(0xA0F80054), readl(0xA0F80058),
	       readl(0xA0F80044));
	printf("gxflash: ctrl=%08x stat=%08x data=%08x\n",
	       readl(0xA0302000), readl(0xA0302004), readl(0xA030200C));

	flash = spi_flash_probe(CONFIG_SF_DEFAULT_BUS, CONFIG_SF_DEFAULT_CS,
				CONFIG_SF_DEFAULT_SPEED, CONFIG_SF_DEFAULT_MODE);
	if (flash) {
		printf("SPI flash: %s, size %u MiB\n", flash->name,
		       (unsigned int)(flash->size >> 20));
		spi_flash_free(flash);
	} else {
		printf("SPI flash probe failed (flash=%08x hwid=%08x fifo=%08x)\n",
		       readl(0xA0302000), readl(0xA0F80050),
		       readl(0xA0400004));
	}

	return 0;
}

int board_usb_init(int index, enum usb_init_type init)
{
	if (init != USB_INIT_HOST)
		return 0;

	return gx6702_usb_enable();
}

int board_init(void)
{
	return 0;
}
