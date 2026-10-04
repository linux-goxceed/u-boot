// SPDX-License-Identifier: GPL-2.0+
/*
 * GX6702 Ethernet pad.
 *
 * The gemini-6702H5 UART loader installs one
 * Synopsys DesignWare GMAC at 0xA0A00000, with the DMA block at +0x1000.
 * The pad field programmed by the vendor MAC initialization is
 * the RMII pad at 0xA030A1B0: set bit 8, clear bits 6 and 7. This is the
 * same bit field the USB bring-up already applies, repeated here so
 * Ethernet does not depend on "usb start".
 */

#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <asm/io.h>
#include "gx_eth.h"

#define GX_ETH_PAD	0xa030a1b0
#define GX_GATE0	0xa030a170
#define GX_GMAC_DMA	0xa0a01000

static void gx_setbits(u32 addr, u32 set)
{
	writel(readl(addr) | set, addr);
}

void gx_eth_enable(void)
{
	u32 v;
	int i;

	/*
	 * The IPL leaves the peripheral gates clear. The vendor loader
	 * ORs these masks immediately before the RMII pad, in the same
	 * block as the USB clock enable. Without them the GMAC register
	 * file stays at zero and MDIO never sees the PHY.
	 */
	gx_setbits(GX_GATE0, BIT(16) | BIT(17) | BIT(18));

	v = readl(GX_ETH_PAD);
	v &= ~(BIT(6) | BIT(7));
	v |= BIT(8);
	writel(v, GX_ETH_PAD);

	/* Synopsys DMA software reset, then let MDIO run. */
	writel(1, GX_GMAC_DMA);
	for (i = 0; i < 100000; i++) {
		if (!(readl(GX_GMAC_DMA) & 1))
			break;
	}
}
