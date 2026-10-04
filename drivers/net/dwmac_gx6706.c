// SPDX-License-Identifier: GPL-2.0+
/* NationalChip GX6706 glue for the Synopsys DesignWare GMAC. */

#include <dm.h>
#include <eth_phy.h>
#include <net.h>
#include <phy.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include "designware.h"

#define GX6706_RTL8201F_ID	0x001cc816
#define GX6706_PHY_PAGE		0x1f
#define GX6706_PHY_PAGE_MASK	0xff
#define GX6706_PHY_RMSR		0x10
#define GX6706_PHY_RMII		BIT(3)
#define GX6706_PHY_REFCLK_IN	BIT(12)

static int gx6706_h5_phy_clock(struct phy_device *phydev)
{
	int page, mode, ret, restore;

	/* Standard PHY discovery selects the driver; this is H5 board wiring. */
	if ((phydev->phy_id & 0xffffff) != GX6706_RTL8201F_ID)
		return 0;

	page = phy_read(phydev, MDIO_DEVAD_NONE, GX6706_PHY_PAGE);
	if (page < 0)
		return page;
	ret = phy_write(phydev, MDIO_DEVAD_NONE, GX6706_PHY_PAGE,
			(page & ~GX6706_PHY_PAGE_MASK) | 7);
	if (ret)
		goto restore_page;
	mode = phy_read(phydev, MDIO_DEVAD_NONE, GX6706_PHY_RMSR);
	if (mode < 0) {
		ret = mode;
		goto restore_page;
	}
	if (mode == 0xffff || !(mode & GX6706_PHY_RMII)) {
		ret = -ENODEV;
		goto restore_page;
	}
	/*
	 * H5 takes its RMII reference clock from the RTL8201F. Select
	 * PHY clock output before DMA reset, preserving factory timings
	 * and the other RMII settings.
	 */
	if (mode & GX6706_PHY_REFCLK_IN) {
		ret = phy_write(phydev, MDIO_DEVAD_NONE, GX6706_PHY_RMSR,
				mode & ~GX6706_PHY_REFCLK_IN);
		if (ret)
			goto restore_page;
	}
	mode = phy_read(phydev, MDIO_DEVAD_NONE, GX6706_PHY_RMSR);
	if (mode < 0)
		ret = mode;
	else if ((mode & GX6706_PHY_REFCLK_IN) || !(mode & GX6706_PHY_RMII))
		ret = -EIO;
restore_page:
	/* A timed-out transaction may still be busy: stop issuing commands. */
	if (ret == -ETIMEDOUT)
		return ret;
	restore = phy_write(phydev, MDIO_DEVAD_NONE, GX6706_PHY_PAGE, page);
	if (!ret)
		ret = restore;
	return ret;
}

static void gx6706_h5_early_setup(void)
{
	/*
	 * H5 loader 0x93ce679c applies this gate/reset sequence before
	 * Ethernet discovery. Keep its write order and disabled gates,
	 * preserving the other domains.
	 */
	writel(readl(0xa030a170) & ~BIT(9), 0xa030a170);
	writel(readl(0xa030a018) | BIT(8), 0xa030a018);
	writel(readl(0xa030a170) & ~BIT(13), 0xa030a170);
	writel(readl(0xa030a018) | BIT(12), 0xa030a018);
	writel(readl(0xa030a068) | BIT(1), 0xa030a068);
	writel(readl(0xa030a068) | BIT(31), 0xa030a068);
}

static int gx6706_dma_reset(struct dw_eth_dev *priv)
{
	struct eth_dma_regs *dma = priv->dma_regs_p;
	u32 busmode;
	unsigned int polls = 0;
	ulong start;
	int ret = -ETIMEDOUT;

	/* H5 0x93cfbb9c writes only SWR, not the old burst/bus fields. */
	writel(DMAMAC_SRST, &dma->busmode);
	start = get_timer(0);
	for (;;) {
		/*
		 * PHY startup applies negotiated speed/duplex after reset.
		 * Leave MAC configuration alone while SWR is asserted.
		 */
		busmode = readl(&dma->busmode);
		if (!(busmode & DMAMAC_SRST)) {
			ret = 0;
			break;
		}
		if (polls >= CFG_MACRESET_TIMEOUT ||
		    get_timer(start) >= CFG_MACRESET_TIMEOUT)
			break;
		udelay(1000);
		polls++;
	}
	if (ret)
		printf("DMA reset timeout (busmode=%08x)\n", busmode);
	return ret;
}

static int gx6706_eth_probe(struct udevice *dev)
{
	struct eth_pdata *pdata = dev_get_plat(dev);
	struct dw_eth_dev *priv = dev_get_priv(dev);
	u32 value;
	int ret;

	if (pdata->iobase != 0xa0a00000 ||
	    pdata->phy_interface != PHY_INTERFACE_MODE_RMII)
		return -EINVAL;

	/*
	 * H5 stage-2 0x93ce66c8 enables this gate mask; 0x93cfb7b8
	 * applies this pad field. Preserve the other peripheral settings.
	 */
	writel(readl(0xa030a170) | GENMASK(18, 16), 0xa030a170);
	value = readl(0xa030a1b0);
	value = (value & ~(BIT(6) | BIT(7))) | BIT(8);
	writel(value, 0xa030a1b0);

	/*
	 * H5 pinmux table 0x93d00e70 selects function 1 for pins 60..68.
	 * Change only the low three function bits per byte; retain the
	 * pad control bits and the other pins in the last word.
	 */
	value = readl(0xa030a43c);
	writel((value & ~0x07070707u) | 0x01010101u, 0xa030a43c);
	value = readl(0xa030a440);
	writel((value & ~0x07070707u) | 0x01010101u, 0xa030a440);
	value = readl(0xa030a444);
	writel((value & ~0x00000007u) | 0x00000001u, 0xa030a444);
	gx6706_h5_early_setup();

	/* H5 0x93cfb870 constructs PHY reads with the CR field clear. */
	priv->mii_clk = MII_CLKRANGE_60_100M;
	priv->mii_clk_set = true;
	priv->mdio_timeout_ms = 100;
	priv->dma_reset = gx6706_dma_reset;

	value = readl(pdata->iobase + 0x20);
	if (!value || value == 0xffffffff) {
		printf("gx6706-eth: MAC is not responding (version=%08x)\n", value);
		return -ENODEV;
	}
	/* Generic probe discovers/configures the PHY without resetting DMA. */
	ret = designware_eth_probe(dev);
	if (!ret) {
		ret = gx6706_h5_phy_clock(priv->phydev);
		if (ret) {
			designware_eth_remove(dev);
			priv->phydev = NULL;
			priv->bus = NULL;
		}
	}
	if (ret)
		printf("gx6706-eth: PHY setup failed (%d)\n", ret);
	return ret;
}

static int gx6706_eth_start(struct udevice *dev)
{
	struct dw_eth_dev *priv = dev_get_priv(dev);
	int ret;

	/*
	 * usb start can enable route gates 9/13 again. Reapply the H5
	 * early sequence before each checked start, independent of USB.
	 */
	gx6706_h5_early_setup();
	ret = gx6706_h5_phy_clock(priv->phydev);
	if (!ret)
		ret = designware_eth_ops.start(dev);
	if (ret)
		printf("gx6706-eth: start failed (%d)\n", ret);
	return ret;
}

static const struct eth_ops gx6706_eth_ops = {
	.start = gx6706_eth_start,
	.send = designware_eth_send,
	.recv = designware_eth_recv,
	.free_pkt = designware_eth_free_pkt,
	.stop = designware_eth_stop,
	.write_hwaddr = designware_eth_write_hwaddr,
};

static const struct udevice_id gx6706_eth_ids[] = {
	{ .compatible = "nationalchip,gx6706-dwmac" },
	{ }
};

U_BOOT_DRIVER(gx6706_dwmac) = {
	.name = "gx6706_dwmac",
	.id = UCLASS_ETH,
	.of_match = gx6706_eth_ids,
	.bind = eth_phy_binds_nodes,
	.of_to_plat = designware_eth_of_to_plat,
	.probe = gx6706_eth_probe,
	.remove = designware_eth_remove,
	.ops = &gx6706_eth_ops,
	.priv_auto = sizeof(struct dw_eth_dev),
	.plat_auto = sizeof(struct dw_eth_pdata),
	.flags = DM_FLAG_ALLOC_PRIV_DMA,
};
