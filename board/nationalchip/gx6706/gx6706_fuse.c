// SPDX-License-Identifier: GPL-2.0+
/*
 * GX6706 eFuse backend for U-Boot's fuse API. Read only.
 *
 * Same CMD/STATUS window as GX6702: uncached 0xA0F80080 / 0xA0F80088.
 * Bank 0 word is an 11-bit byte address. fuse prog and fuse override
 * are refused.
 */

#include <errno.h>
#include <fuse.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>

#define GX6706_EFUSE_CMD		0xA0F80080
#define GX6706_EFUSE_STATUS		0xA0F80088
#define GX6706_EFUSE_ADDR_MASK		0x7ff
#define GX6706_EFUSE_WAIT_LIMIT		0x01000000

static int gx6706_efuse_read_byte(u32 addr, u8 *value)
{
	u32 count;
	u32 cmd;

	if (addr > GX6706_EFUSE_ADDR_MASK)
		return -EINVAL;

	for (count = 0; count < GX6706_EFUSE_WAIT_LIMIT; count++) {
		if (readl(GX6706_EFUSE_STATUS) & BIT(10))
			break;
	}
	if (count == GX6706_EFUSE_WAIT_LIMIT)
		return -ETIMEDOUT;

	for (count = 0; count < GX6706_EFUSE_WAIT_LIMIT; count++) {
		if (!(readl(GX6706_EFUSE_STATUS) & BIT(8)))
			break;
	}
	if (count == GX6706_EFUSE_WAIT_LIMIT)
		return -ETIMEDOUT;

	cmd = ((addr & GX6706_EFUSE_ADDR_MASK) << 3) | BIT(14);
	writel(cmd, GX6706_EFUSE_CMD);
	udelay(10);
	writel(cmd & ~BIT(14), GX6706_EFUSE_CMD);

	for (count = 0; count < GX6706_EFUSE_WAIT_LIMIT; count++) {
		if (readl(GX6706_EFUSE_STATUS) & BIT(9)) {
			*value = (u8)readl(GX6706_EFUSE_STATUS);
			writel(0, GX6706_EFUSE_CMD);
			return 0;
		}
	}

	writel(0, GX6706_EFUSE_CMD);
	return -ETIMEDOUT;
}

int fuse_read(u32 bank, u32 word, u32 *val)
{
	u8 byte;
	int ret;

	if (bank != 0 || word > GX6706_EFUSE_ADDR_MASK)
		return -EINVAL;

	ret = gx6706_efuse_read_byte(word, &byte);
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
	return -EPERM;
}

int fuse_override(u32 bank, u32 word, u32 val)
{
	return -EPERM;
}

/*
 * Same four DAC trim bytes the GX6702 display path reads.  A failed read
 * keeps the live-golden gain instead of blocking HDMI bring-up.
 */
u32 gx6702_efuse_dac_gain(void)
{
	static const u8 blank[4] = { 0x1e, 0x1e, 0x1e, 0x20 };
	u8 ch;
	u32 gain = 0;
	u32 i;

	for (i = 0; i < 4; i++) {
		if (gx6706_efuse_read_byte(0x126 + i, &ch))
			return 0x1b1b1b1b;
		if (!ch)
			ch = blank[i];
		else
			ch &= 0x3f;
		gain |= (u32)ch << (i * 8);
	}

	return gain;
}
