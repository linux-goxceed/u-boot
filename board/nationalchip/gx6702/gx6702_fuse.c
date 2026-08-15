// SPDX-License-Identifier: GPL-2.0+
/*
 * GX6702 eFuse backend for U-Boot's fuse API.
 *
 * Read: open IPL (gxipl/ipl.c efuse_read) / gemini gx_otp read @ 0x93cfe4b0.
 * Write: gemini gx_otp write @ 0x93cfe56c (CMD bit15 strobe, data in [23:16]).
 * Uncached CMD/STATUS at 0xA0F80080 / 0xA0F80088.  Address space is 11-bit
 * bytes; CMD_FUSE maps bank 0 / word == byte address, value in bits[7:0].
 *
 * fuse_override is unsupported (no shadow register).  Programming is
 * irreversible — do not exercise fuse prog on a populated device casually.
 */

#include <errno.h>
#include <fuse.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/types.h>

#include "gx6702_efuse.h"

#define GX6702_EFUSE_CMD		0xA0F80080
#define GX6702_EFUSE_STATUS		0xA0F80088
#define GX6702_EFUSE_ADDR_MASK		0x7ff
#define GX6702_EFUSE_WAIT_LIMIT		0x01000000
/* Vendor passes 64 to its delay helper around the write strobe. */
#define GX6702_EFUSE_PROG_DELAY_US	100

#define GX6702_EFUSE_DAC_ADDR0		0x126
#define GX6702_EFUSE_DAC_LANES		4

/* Vendor blank defaults at BOOT.bin 0x93176784 (lanes 0..3). */
#define GX6702_EFUSE_DAC_BLANK_0	0x1e
#define GX6702_EFUSE_DAC_BLANK_3	0x20

/* Live golden when the controller does not respond. */
#define GX6702_EFUSE_DAC_GAIN_FALLBACK	0x1b1b1b1b

static const u8 gx6702_dac_blank_defaults[GX6702_EFUSE_DAC_LANES] = {
	GX6702_EFUSE_DAC_BLANK_0,
	GX6702_EFUSE_DAC_BLANK_0,
	GX6702_EFUSE_DAC_BLANK_0,
	GX6702_EFUSE_DAC_BLANK_3,
};

static int gx6702_efuse_read_byte(u32 addr, u8 *value)
{
	u32 count;
	u32 cmd;

	if (addr > GX6702_EFUSE_ADDR_MASK)
		return -EINVAL;

	for (count = 0; count < GX6702_EFUSE_WAIT_LIMIT; count++) {
		if (readl(GX6702_EFUSE_STATUS) & BIT(10))
			break;
	}
	if (count == GX6702_EFUSE_WAIT_LIMIT)
		return -ETIMEDOUT;

	for (count = 0; count < GX6702_EFUSE_WAIT_LIMIT; count++) {
		if (!(readl(GX6702_EFUSE_STATUS) & BIT(8)))
			break;
	}
	if (count == GX6702_EFUSE_WAIT_LIMIT)
		return -ETIMEDOUT;

	cmd = ((addr & GX6702_EFUSE_ADDR_MASK) << 3) | BIT(14);
	writel(cmd, GX6702_EFUSE_CMD);
	udelay(10);
	writel(cmd & ~BIT(14), GX6702_EFUSE_CMD);

	for (count = 0; count < GX6702_EFUSE_WAIT_LIMIT; count++) {
		if (readl(GX6702_EFUSE_STATUS) & BIT(9)) {
			*value = (u8)readl(GX6702_EFUSE_STATUS);
			writel(0, GX6702_EFUSE_CMD);
			return 0;
		}
	}

	writel(0, GX6702_EFUSE_CMD);
	return -ETIMEDOUT;
}

static int gx6702_efuse_write_byte(u32 addr, u8 value)
{
	u32 count;
	u32 cmd;

	if (addr > GX6702_EFUSE_ADDR_MASK)
		return -EINVAL;

	for (count = 0; count < GX6702_EFUSE_WAIT_LIMIT; count++) {
		if (readl(GX6702_EFUSE_STATUS) & BIT(10))
			break;
	}
	if (count == GX6702_EFUSE_WAIT_LIMIT)
		return -ETIMEDOUT;

	for (count = 0; count < GX6702_EFUSE_WAIT_LIMIT; count++) {
		if (!(readl(GX6702_EFUSE_STATUS) & BIT(8)))
			break;
	}
	if (count == GX6702_EFUSE_WAIT_LIMIT)
		return -ETIMEDOUT;

	/*
	 * cmd = (addr << 3) | (byte << 16); assert BIT(15), delay, then
	 * rewrite without BIT(15).  Vendor does not poll STATUS[9] on write.
	 */
	cmd = ((addr & GX6702_EFUSE_ADDR_MASK) << 3) | ((u32)value << 16);
	writel(cmd | BIT(15), GX6702_EFUSE_CMD);
	udelay(GX6702_EFUSE_PROG_DELAY_US);
	writel(cmd, GX6702_EFUSE_CMD);
	udelay(GX6702_EFUSE_PROG_DELAY_US);
	writel(0, GX6702_EFUSE_CMD);

	return 0;
}

u32 gx6702_efuse_dac_gain(void)
{
	u8 ch[GX6702_EFUSE_DAC_LANES];
	u32 gain = 0;
	u32 i;

	for (i = 0; i < GX6702_EFUSE_DAC_LANES; i++) {
		if (gx6702_efuse_read_byte(GX6702_EFUSE_DAC_ADDR0 + i, &ch[i]))
			return GX6702_EFUSE_DAC_GAIN_FALLBACK;

		if (!ch[i])
			ch[i] = gx6702_dac_blank_defaults[i];
		else
			ch[i] &= 0x3f;

		gain |= ((u32)ch[i] & 0x3f) << (i * 8);
	}

	return gain;
}

int fuse_read(u32 bank, u32 word, u32 *val)
{
	u8 byte;
	int ret;

	if (bank != 0 || word > GX6702_EFUSE_ADDR_MASK)
		return -EINVAL;

	ret = gx6702_efuse_read_byte(word, &byte);
	if (ret)
		return ret;

	*val = byte;
	return 0;
}

int fuse_sense(u32 bank, u32 word, u32 *val)
{
	return fuse_read(bank, word, val);
}

int fuse_prog(u32 bank, u32 word, u32 val)
{
	if (bank != 0 || word > GX6702_EFUSE_ADDR_MASK)
		return -EINVAL;

	return gx6702_efuse_write_byte(word, (u8)val);
}

int fuse_override(u32 bank, u32 word, u32 val)
{
	return -EPERM;
}
