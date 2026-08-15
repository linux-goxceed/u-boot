// SPDX-License-Identifier: GPL-2.0+
/*
 * NationalChip Gemini GX6702 GPIO controller.
 *
 * GX6702 is similar to, but register-incompatible with, the GX6605S block
 * described as "wd,mbl-gpio" by Linux.  The layout below is recovered from
 * the GX6702 vendor BOOT.bin routines at 0x9316f3d4 (direction) and
 * 0x9316f44c (value), plus live register traces from the board:
 *
 *   direction(+0x00): current output-enable bitmap
 *   dir-set  (+0x04): write 1 to make a pin an output
 *   dir-clr  (+0x08): write 1 to make a pin an input
 *   reserved (+0x0c)
 *   data     (+0x10): current GPIO levels
 *   out-set  (+0x14): write-one high alias
 *   out-clr  (+0x18): write-one low alias
 *
 * A030A000 bit 7 is an active-high reset.  Vendor BOOT pulses it and leaves
 * it clear before accessing these GPIO command registers.
 */

#include <dm.h>
#include <asm/io.h>
#include <errno.h>
#include <asm/gpio.h>
#include <linux/bitops.h>

#define GX_GPIO_INPUT		0x10
#define GX_GPIO_DIR_SET		0x04
#define GX_GPIO_DIR_CLR		0x08
#define GX_GPIO_OUT_SET		0x14
#define GX_GPIO_OUT_CLR		0x18

#define GX_GPIO_COUNT		32

#define GX_GPIO_CTL		0xA030A000UL
struct gx6702_gpio_plat {
	void __iomem *base;
};

static void gx6702_gpio_set_latch(struct gx6702_gpio_plat *plat,
				  unsigned int offset, int value)
{
	if (value)
		writel(BIT(offset), plat->base + GX_GPIO_OUT_SET);
	else
		writel(BIT(offset), plat->base + GX_GPIO_OUT_CLR);
}

struct gx6702_gpio_priv {
	/* The direction command registers are not readable. */
	u32 dirout;
};

static void gx6702_gpio_release_reset(void)
{
	void __iomem *ctl = (void __iomem *)GX_GPIO_CTL;
	u32 v;

	v = readl(ctl);
	if (v & BIT(7))
		writel(v & ~BIT(7), ctl);
}

static int gx6702_gpio_direction_input(struct udevice *dev, u32 offset)
{
	struct gx6702_gpio_plat *plat = dev_get_plat(dev);
	struct gx6702_gpio_priv *priv = dev_get_priv(dev);

	gx6702_gpio_release_reset();
	writel(BIT(offset), plat->base + GX_GPIO_DIR_CLR);
	priv->dirout &= ~BIT(offset);

	return 0;
}

static int gx6702_gpio_direction_output(struct udevice *dev, u32 offset,
					int value)
{
	struct gx6702_gpio_plat *plat = dev_get_plat(dev);
	struct gx6702_gpio_priv *priv = dev_get_priv(dev);
	u32 mask = BIT(offset);

	gx6702_gpio_release_reset();

	/* Program the latch before enabling the output to avoid a glitch. */
	gx6702_gpio_set_latch(plat, offset, value);
	writel(mask, plat->base + GX_GPIO_DIR_SET);
	priv->dirout |= mask;

	return 0;
}

static int gx6702_gpio_get_value(struct udevice *dev, u32 offset)
{
	struct gx6702_gpio_plat *plat = dev_get_plat(dev);

	return !!(readl(plat->base + GX_GPIO_INPUT) & BIT(offset));
}

static int gx6702_gpio_set_value(struct udevice *dev, u32 offset, int value)
{
	struct gx6702_gpio_plat *plat = dev_get_plat(dev);

	gx6702_gpio_release_reset();
	gx6702_gpio_set_latch(plat, offset, value);

	return 0;
}

static int gx6702_gpio_get_function(struct udevice *dev, unsigned int offset)
{
	struct gx6702_gpio_priv *priv = dev_get_priv(dev);

	if (priv->dirout & BIT(offset))
		return GPIOF_OUTPUT;

	return GPIOF_INPUT;
}

static const struct dm_gpio_ops gx6702_gpio_ops = {
	.direction_input	= gx6702_gpio_direction_input,
	.direction_output	= gx6702_gpio_direction_output,
	.get_value		= gx6702_gpio_get_value,
	.set_value		= gx6702_gpio_set_value,
	.get_function		= gx6702_gpio_get_function,
};

static int gx6702_gpio_probe(struct udevice *dev)
{
	struct gpio_dev_priv *uc_priv = dev_get_uclass_priv(dev);

	uc_priv->bank_name = "gpio";
	uc_priv->gpio_count = dev_read_u32_default(dev, "ngpios",
						   GX_GPIO_COUNT);

	return 0;
}

static int gx6702_gpio_of_to_plat(struct udevice *dev)
{
	struct gx6702_gpio_plat *plat = dev_get_plat(dev);

	plat->base = dev_read_addr_ptr(dev);
	if (!plat->base)
		return -EINVAL;

	return 0;
}

static const struct udevice_id gx6702_gpio_ids[] = {
	{ .compatible = "nationalchip,gx6702-gpio" },
	{}
};

U_BOOT_DRIVER(gx6702_gpio) = {
	.name		= "gx6702_gpio",
	.id		= UCLASS_GPIO,
	.of_match	= gx6702_gpio_ids,
	.of_to_plat	= gx6702_gpio_of_to_plat,
	.plat_auto	= sizeof(struct gx6702_gpio_plat),
	.priv_auto	= sizeof(struct gx6702_gpio_priv),
	.ops		= &gx6702_gpio_ops,
	.probe		= gx6702_gpio_probe,
};
