// SPDX-License-Identifier: GPL-2.0+
/*
 * NationalChip GX6702 hardware watchdog.
 *
 * The register sequence was recovered independently from the GX6702 eCos
 * watchdog driver and the GxLoader reset implementation.  The block runs from
 * a 27 MHz source; GxLoader divides it to 1 kHz and stores the one's-complement
 * millisecond timeout in the low half of the clock/load register.
 */

#include <dm.h>
#include <errno.h>
#include <wdt.h>
#include <asm/io.h>
#include <linux/bitops.h>

#define GX6702_WDT_CTRL		0x00
#define GX6702_WDT_CLOCK_LOAD	0x04
#define GX6702_WDT_COUNT		0x08
#define GX6702_WDT_RESTART	0x0c

#define GX6702_WDT_CTRL_ENABLE	BIT(0)
#define GX6702_WDT_CTRL_RESET	BIT(1)
#define GX6702_WDT_CTRL_MASK	(GX6702_WDT_CTRL_ENABLE | \
				 GX6702_WDT_CTRL_RESET)

#define GX6702_WDT_INPUT_HZ	27000000U
#define GX6702_WDT_TICK_HZ	1000U
#define GX6702_WDT_DIVIDER	(GX6702_WDT_INPUT_HZ / \
				 GX6702_WDT_TICK_HZ - 1U)
#define GX6702_WDT_MAX_MS	0xffffU

#define GX6702_WDT_RESTART_1	0x5555U
#define GX6702_WDT_RESTART_2	0xaaaaU

struct gx6702_wdt_priv {
	void __iomem *base;
};

static int gx6702_wdt_stop(struct udevice *dev)
{
	struct gx6702_wdt_priv *priv = dev_get_priv(dev);
	u32 ctrl;

	/* Match GxLoader: clear restart state before disabling both actions. */
	writel(0, priv->base + GX6702_WDT_RESTART);
	ctrl = readl(priv->base + GX6702_WDT_CTRL);
	ctrl &= ~GX6702_WDT_CTRL_MASK;
	writel(ctrl, priv->base + GX6702_WDT_CTRL);
	sync();

	return 0;
}

static int gx6702_wdt_start(struct udevice *dev, u64 timeout_ms, ulong flags)
{
	struct gx6702_wdt_priv *priv = dev_get_priv(dev);
	u32 reg;

	if (!timeout_ms || timeout_ms > GX6702_WDT_MAX_MS)
		return -EINVAL;

	gx6702_wdt_stop(dev);

	/* Program the vendor-proven 1 kHz watchdog timebase. */
	reg = readl(priv->base + GX6702_WDT_CLOCK_LOAD) & 0xffffU;
	reg |= GX6702_WDT_DIVIDER << 16;
	writel(reg, priv->base + GX6702_WDT_CLOCK_LOAD);

	/* The hardware counts from (0x10000 - milliseconds) to overflow. */
	reg = readl(priv->base + GX6702_WDT_CLOCK_LOAD) & 0xffff0000U;
	reg |= (0x10000U - (u32)timeout_ms) & 0xffffU;
	writel(reg, priv->base + GX6702_WDT_CLOCK_LOAD);

	reg = readl(priv->base + GX6702_WDT_CTRL);
	reg |= GX6702_WDT_CTRL_MASK;
	writel(reg, priv->base + GX6702_WDT_CTRL);
	sync();

	return 0;
}

static int gx6702_wdt_reset(struct udevice *dev)
{
	struct gx6702_wdt_priv *priv = dev_get_priv(dev);
	u32 reg;

	/* eCos feeds the watchdog by writing these two low-halfword keys. */
	reg = readl(priv->base + GX6702_WDT_RESTART) & 0xffff0000U;
	writel(reg | GX6702_WDT_RESTART_1,
	       priv->base + GX6702_WDT_RESTART);
	writel(reg | GX6702_WDT_RESTART_2,
	       priv->base + GX6702_WDT_RESTART);
	sync();

	return 0;
}

static int gx6702_wdt_expire_now(struct udevice *dev, ulong flags)
{
	int ret;

	ret = gx6702_wdt_start(dev, 1, flags);
	if (ret)
		return ret;

	for (;;)
		asm volatile("" : : : "memory");
}

static int gx6702_wdt_probe(struct udevice *dev)
{
	struct gx6702_wdt_priv *priv = dev_get_priv(dev);

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	/* Do not inherit a short watchdog left running by earlier firmware. */
	return gx6702_wdt_stop(dev);
}

static const struct wdt_ops gx6702_wdt_ops = {
	.start = gx6702_wdt_start,
	.stop = gx6702_wdt_stop,
	.reset = gx6702_wdt_reset,
	.expire_now = gx6702_wdt_expire_now,
};

static const struct udevice_id gx6702_wdt_ids[] = {
	{ .compatible = "nationalchip,gx6702-wdt" },
	{ }
};

U_BOOT_DRIVER(gx6702_wdt) = {
	.name = "gx6702_wdt",
	.id = UCLASS_WDT,
	.of_match = gx6702_wdt_ids,
	.probe = gx6702_wdt_probe,
	.priv_auto = sizeof(struct gx6702_wdt_priv),
	.ops = &gx6702_wdt_ops,
};
