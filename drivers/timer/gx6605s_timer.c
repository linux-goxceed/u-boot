// SPDX-License-Identifier: GPL-2.0+
/*
 * NationalChip Gemini SoC timer (GX6605s / GX6702 family).
 *
 * Ported from the Linux "csky,gx6605s-timer" clocksource driver. The block
 * contains two identical 32-bit count-up timers: the clock-event timer at
 * offset 0x00 and the clock-source (free-running) timer at offset 0x40. U-Boot
 * only needs the free-running counter, so we program the clock-source half and
 * expose its VALUE register as the DM timer count.
 *
 * NOTE: the exact register base for the GX6702 is not yet confirmed. Fill in
 * the "reg" in the device tree with the address of a free-running counter
 * (verify from the U-Boot prompt with e.g. "md.l <base+0x44> 2" showing an
 * incrementing value) before enabling CONFIG_GX6605S_TIMER.
 */

#include <dm.h>
#include <timer.h>
#include <clk.h>
#include <asm/io.h>
#include <linux/bitops.h>

/* Clock-source counter lives at +0x40 in the block. */
#define GX6605S_CLKSRC_OFFSET	0x40

#define GX6605S_TIMER_STATUS	0x00
#define GX6605S_TIMER_VALUE	0x04
#define GX6605S_TIMER_CONTRL	0x10
#define GX6605S_TIMER_CONFIG	0x20
#define GX6605S_TIMER_DIV	0x24
#define GX6605S_TIMER_INI	0x28

#define GX6605S_STATUS_CLR	BIT(0)
#define GX6605S_CONTRL_RST	BIT(0)
#define GX6605S_CONTRL_START	BIT(1)
#define GX6605S_CONFIG_EN	BIT(0)
#define GX6605S_CONFIG_IRQ_EN	BIT(1)

struct gx6605s_timer_priv {
	void __iomem *base;
};

static u64 gx6605s_timer_get_count(struct udevice *dev)
{
	struct gx6605s_timer_priv *priv = dev_get_priv(dev);

	return timer_conv_64(readl(priv->base + GX6605S_TIMER_VALUE));
}

static int gx6605s_timer_probe(struct udevice *dev)
{
	struct timer_dev_priv *uc_priv = dev_get_uclass_priv(dev);
	struct gx6605s_timer_priv *priv = dev_get_priv(dev);
	void __iomem *base;

	base = dev_read_addr_ptr(dev);
	if (!base)
		return -EINVAL;

	/* Use the free-running clock-source half of the block. */
	priv->base = base + GX6605S_CLKSRC_OFFSET;

	if (!uc_priv->clock_rate) {
#if CONFIG_IS_ENABLED(CLK)
		struct clk clk;

		if (!clk_get_by_index(dev, 0, &clk))
			uc_priv->clock_rate = clk_get_rate(&clk);
#endif
		if (!uc_priv->clock_rate)
			uc_priv->clock_rate =
				dev_read_u32_default(dev, "clock-frequency", 0);
	}
	if (!uc_priv->clock_rate)
		return -EINVAL;

	/* Program a free-running count-up source (Linux gx6605s_clksrc_init). */
	writel(0, priv->base + GX6605S_TIMER_DIV);
	writel(0, priv->base + GX6605S_TIMER_INI);
	writel(GX6605S_CONTRL_RST, priv->base + GX6605S_TIMER_CONTRL);
	writel(GX6605S_CONFIG_EN, priv->base + GX6605S_TIMER_CONFIG);
	writel(GX6605S_CONTRL_START, priv->base + GX6605S_TIMER_CONTRL);

	return 0;
}

static const struct timer_ops gx6605s_timer_ops = {
	.get_count = gx6605s_timer_get_count,
};

static const struct udevice_id gx6605s_timer_ids[] = {
	{ .compatible = "csky,gx6605s-timer" },
	{ .compatible = "nationalchip,gx6702-timer" },
	{}
};

U_BOOT_DRIVER(gx6605s_timer) = {
	.name		= "gx6605s_timer",
	.id		= UCLASS_TIMER,
	.of_match	= gx6605s_timer_ids,
	.priv_auto	= sizeof(struct gx6605s_timer_priv),
	.probe		= gx6605s_timer_probe,
	.ops		= &gx6605s_timer_ops,
};
