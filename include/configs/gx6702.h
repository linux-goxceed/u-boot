/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * NationalChip GX6702 (Gemini 6702H5) board configuration.
 *
 * Open boot path: BootROM UART handshake -> gxipl (SRAM) -> RUNGET ->
 * raw U-Boot loaded to DDR at 0x93CE8420.  The legacy vendor stage-1 uses the
 * same final machine state and remains useful as a comparison target.
 *
 * UART: ns16550a, 32-bit register access (reg-shift 2).  After the vendor
 * stage-1 enables the MMU, use the uncached alias 0xA0402000.
 */

#ifndef __GX6702_CONFIG_H__
#define __GX6702_CONFIG_H__

#define CFG_SYS_SDRAM_BASE		0x90000000
#define CFG_SYS_SDRAM_SIZE		(64 * 1024 * 1024)

/* Below U-Boot @ 0x93CE8420; stack grows down into free DRAM. */
#define CFG_SYS_INIT_SP_ADDR		0x93CE8420

#define CFG_SYS_NS16550_CLK		24000000
#define CFG_SYS_NS16550_COM1		0xA0402000

/* ns16550a: 32-bit registers (reg-shift 2), uncached after stage-1 MMU */
#define CFG_SYS_NS16550_REG_SIZE	(-4)

/* The IPL leaves the UART at 115200 8N1; do not reprogram it in U-Boot. */
#define CFG_SYS_NS16550_SKIP_INIT

#define CFG_SYS_BAUDRATE_TABLE	{ 115200, 57600, 38400, 19200, 9600 }

/*
 * SYS_CONSOLE_IS_IN_ENV means stdio_add_devices() leaves video devices alone
 * and console_init_r() only probes the ones stdout names, so the display stays
 * dark unless vidconsole is listed here.  Serial comes first so the UART keeps
 * the console if the display fails to come up.
 */
#define CFG_EXTRA_ENV_SETTINGS \
	"stdout=serial,vidconsole\0" \
	"stderr=serial,vidconsole\0" \
	"bootargs=earlycon=gxuart keep_bootcon console=ttyS0,115200n8 rdinit=/init\0" \
	"loadaddr=0x92000000\0" \
	"fdt_addr_r=0x91f00000\0" \
	"ramdisk_addr_r=0x92800000\0" \
	"boot_usb=" \
		"usb start; " \
		"fatload usb 0:1 ${loadaddr} uImage; " \
		"fatload usb 0:1 ${fdt_addr_r} gx6702-gemini.dtb; " \
		"fatload usb 0:1 ${ramdisk_addr_r} initramfs.uImage; " \
		"bootm ${loadaddr} ${ramdisk_addr_r} ${fdt_addr_r}\0" \
	"boot_uart=" \
		"echo 'host: sb uImage (ymodem) after loady'; " \
		"loady ${loadaddr}; " \
		"echo 'host: sb gx6702-gemini.dtb'; " \
		"loady ${fdt_addr_r}; " \
		"echo 'host: sb initramfs.uImage'; " \
		"loady ${ramdisk_addr_r}; " \
		"bootm ${loadaddr} ${ramdisk_addr_r} ${fdt_addr_r}\0"

#endif /* __GX6702_CONFIG_H__ */
