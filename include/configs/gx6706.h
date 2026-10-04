/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * NationalChip GX6706 (Cygnus) board configuration.
 *
 * Open boot path: BootROM loads gxipl into SRAM. Stage-1 trains DDR, enables
 * the shared CK610 MMU, and hands a GXBC bootcode record to DDR. The bootcode
 * loads a static CK610 ELF (start6706.elf on USB, or the U-Boot ELF copied to
 * that name) or accepts a UART RUNGET of the raw image at 0x93CE8420.
 * Copy the built "u-boot" ELF to start6706.elf. u-boot.bin is the raw RUNGET
 * payload. Do not write this image to flash from this port.
 *
 * UART: ns16550a, 32-bit register access (reg-shift 2), uncached 0xA0402000.
 * The IPL leaves it at 115200 8N1 from a 29.4912 MHz clock.
 */

#ifndef __GX6706_CONFIG_H__
#define __GX6706_CONFIG_H__

#define CFG_SYS_SDRAM_BASE		0x90000000
#define CFG_SYS_SDRAM_SIZE		(64 * 1024 * 1024)

/* Below U-Boot @ 0x93CE8420; stack grows down into free DRAM. */
#define CFG_SYS_INIT_SP_ADDR		0x93CE8420

#define CFG_SYS_NS16550_CLK		29491200
#define CFG_SYS_NS16550_COM1		0xA0402000

/* ns16550a: 32-bit registers (reg-shift 2), uncached after stage-1 MMU */
#define CFG_SYS_NS16550_REG_SIZE	(-4)

/* The IPL leaves the UART at 115200 8N1; do not reprogram it in U-Boot. */
#define CFG_SYS_NS16550_SKIP_INIT

#define CFG_SYS_BAUDRATE_TABLE	{ 115200, 57600, 38400, 19200, 9600 }

#define CFG_EXTRA_ENV_SETTINGS \
	"stdout=serial,vidconsole\0" \
	"stderr=serial,vidconsole\0" \
	"bootargs=earlycon=gxuart keep_bootcon console=ttyS0,115200n8 rdinit=/init\0" \
	"loadaddr=0x92000000\0" \
	"fdt_addr_r=0x91f00000\0" \
	"ramdisk_addr_r=0x90c00000\0" \
	"initrd_high=0xffffffff\0" \
	"ipaddr=192.168.120.3\0" \
	"netmask=255.255.255.0\0" \
	"serverip=192.168.120.100\0" \
	"ethaddr=02:67:06:00:00:01\0" \
	"boot_usb=" \
		"usb start; " \
		"fatload usb 0:1 ${loadaddr} uImage; " \
		"fatload usb 0:1 ${fdt_addr_r} gx6706.dtb; " \
		"fatload usb 0:1 ${ramdisk_addr_r} initramfs.uImage; " \
		"bootm ${loadaddr} ${ramdisk_addr_r} ${fdt_addr_r}\0" \
	"boot_uart=" \
		"wdt dev watchdog@a020b000; " \
		"wdt stop; " \
		"echo 'host: sb uImage (ymodem) after loady'; " \
		"loady ${loadaddr}; " \
		"echo 'host: sb gx6706.dtb'; " \
		"loady ${fdt_addr_r}; " \
		"echo 'host: sb initramfs.uImage'; " \
		"loady ${ramdisk_addr_r}; " \
		"bootm ${loadaddr} ${ramdisk_addr_r} ${fdt_addr_r}\0"

#endif /* __GX6706_CONFIG_H__ */
