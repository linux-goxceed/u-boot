// SPDX-License-Identifier: GPL-2.0+
/*
 * gxloader - load the on-flash GxLoader (BOOT partition) into RAM.
 *
 * BOOT.bin layout (linked at 0x931693e0):
 *   +0x0000  header (NOT executable — jumping here hangs the CPU)
 *   +0x0044  early CPU/MMU init, then jmp 0x9316b400
 *   +0x2020  main GxLoader entry @ 0x9316b400 (BSS clear, full boot)
 *   +0x7344  padmux @ 0x93170724 — hangs in upload boot (conflict-check loop)
 *   +0x819C  full display @ 0x9317157C — hangs (70C08 writes 0xA0305000)
 *
 * "gxloader disp" only loads the 128 KiB image for inspection; use FOSS
 * "hd2015 boot" for the panel (ported 70724 shadow table + TM1650).
 */

#include <command.h>
#include <cpu_func.h>
#include <spi_flash.h>
#include <vsprintf.h>

#define GXLOADER_LOAD	0x931693e0UL
#define GXLOADER_SIZE	0x20000UL			/* full BOOT-128k */
#define GXLOADER_MAIN	(GXLOADER_LOAD + 0x2020UL)	/* 0x9316b400 */

/* CK610 cachev1: full I/D writeback+invalidate (from gxtest/stage2load.c). */
static void gx6702_cache_wbinv_all(void)
{
	unsigned int op = (1 << 0) | (1 << 1) | (1 << 4) | (1 << 5);

	__asm__ __volatile__("mtcr %0, cr17" : : "r"(op) : "memory");
	__asm__ __volatile__("idly4" ::: "memory");
}

static int gxloader_load(ulong flash_off)
{
	unsigned int bus = CONFIG_SF_DEFAULT_BUS;
	unsigned int cs = CONFIG_SF_DEFAULT_CS;
	struct spi_flash *flash;
	int ret;

	flash = spi_flash_probe(bus, cs, CONFIG_SF_DEFAULT_SPEED,
				CONFIG_SF_DEFAULT_MODE);
	if (!flash) {
		printf("gxloader: SPI flash probe failed\n");
		return CMD_RET_FAILURE;
	}

	printf("gxloader: sf read 0x%x @0x%lx -> 0x%lx\n",
	       (unsigned int)GXLOADER_SIZE, flash_off, GXLOADER_LOAD);

	ret = spi_flash_read(flash, flash_off, GXLOADER_SIZE,
			     (void *)GXLOADER_LOAD);
	spi_flash_free(flash);
	if (ret) {
		printf("gxloader: SPI read failed (%d)\n", ret);
		return CMD_RET_FAILURE;
	}

	gx6702_cache_wbinv_all();
	return CMD_RET_SUCCESS;
}

static int do_gxloader(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	ulong flash_off = 0;
	const char *sub = "boot";
	int ret;

	if (argc > 1) {
		if (!strcmp(argv[1], "disp") || !strcmp(argv[1], "boot") ||
		    !strcmp(argv[1], "help"))
			sub = argv[1];
		else
			flash_off = hextoul(argv[1], NULL);
	}
	if (argc > 2 && !strcmp(sub, "boot"))
		flash_off = hextoul(argv[2], NULL);

	if (!strcmp(sub, "help")) {
		printf("gxloader disp  - load 128 KiB BOOT (no vendor call; use hd2015 boot)\n");
		printf("gxloader boot  - run GxLoader @0x%08x (needs power-cycle)\n",
		       (unsigned int)GXLOADER_MAIN);
		printf("Do NOT jump to 0x%08x (BOOT header).\n",
		       (unsigned int)GXLOADER_LOAD);
		return CMD_RET_SUCCESS;
	}

	ret = gxloader_load(flash_off);
	if (ret)
		return ret;

	if (!strcmp(sub, "disp")) {
		printf("gxloader: BOOT loaded @0x%08x (%u bytes)\n",
		       (unsigned int)GXLOADER_LOAD,
		       (unsigned int)GXLOADER_SIZE);
		printf("gxloader: NOT calling vendor 0x9317157C/0x93170724 — both hang in upload boot\n");
		printf("gxloader: use FOSS panel path instead: hd2015 boot\n");
		return CMD_RET_SUCCESS;
	}

	printf("gxloader: jumping to 0x%08x (NOT 0x%08x header)\n",
	       (unsigned int)GXLOADER_MAIN, (unsigned int)GXLOADER_LOAD);
	printf("gxloader: UART upload dead until FULL power-cycle + gxupload\n");

	flush();
	((void (*)(void))GXLOADER_MAIN)();

	printf("gxloader: returned unexpectedly\n");
	return CMD_RET_FAILURE;
}

U_BOOT_CMD(gxloader, 3, 0, do_gxloader,
	   "load/run on-flash GxLoader or vendor display init",
	   "[disp|boot] [flash_off]\n"
	   "    disp  - load BOOT-128k for RE; FOSS display is: hd2015 boot\n"
	   "    boot  - jump to GxLoader main @0x9316b400 (needs power-cycle)\n"
	   "    default = boot. flash_off defaults to 0 (BOOT partition)");
