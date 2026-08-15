// SPDX-License-Identifier: GPL-2.0+
/*
 * GX6702 USB host pad / PHY bring-up.
 *
 * Sequence recovered from vendor GxLoader
 * (libre-gxdl/loaders/gemini-6702H5-sflash-24M.boot stage-2 @ VA 0x93CE6620):
 *
 *   0) Clock-route and gate state installed by the open IPL
 *   1) Pad config via A030Axxx (shadow/padmux window)
 *   2) Dual PHY channel programming at A0908xxx / A090Axxx
 *   3) Final VBUS/port bit on A030AA08 / A030A20C
 *
 * EHCI itself is stock generic-ehci at 0xA0904000 (vendor ehci_hcd_init).
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <asm/io.h>
#include "gx6702_usb.h"

#define GX_USB_PHY0_BASE	0xA0908000
#define GX_USB_PHY1_BASE	0xA0908400
#define GX_USB_CFG_BASE		0xA090A000

static void gx_clrset(u32 addr, u32 clear, u32 set)
{
	u32 v = readl(addr);

	v = (v & ~clear) | set;
	writel(v, addr);
}

static void gx_usb_phy_bits_clear_en(void)
{
	/* 0x93CE65B4: clear enable-ish bits */
	gx_clrset(GX_USB_CFG_BASE + 0x4, (1U << 10) | (1U << 26), 0);
	gx_clrset(GX_USB_CFG_BASE + 0x8, (1U << 26), 0);
}

static void gx_usb_phy_bits_set_mid(void)
{
	/* 0x93CE65D8 */
	gx_clrset(GX_USB_CFG_BASE + 0x4, 0, (1U << 12) | (1U << 28));
	gx_clrset(GX_USB_CFG_BASE + 0x8, 0, (1U << 28));
}

static void gx_usb_phy_bits_clear_mid(void)
{
	/* 0x93CE65FC */
	gx_clrset(GX_USB_CFG_BASE + 0x4, (1U << 12) | (1U << 28), 0);
	gx_clrset(GX_USB_CFG_BASE + 0x8, (1U << 28), 0);
}

static void gx_usb_phy_bits_set_en(void)
{
	gx_clrset(GX_USB_CFG_BASE + 0x4, 0, (1U << 10) | (1U << 26));
	gx_clrset(GX_USB_CFG_BASE + 0x8, 0, (1U << 26));
}

static void gx_usb_program_phy_channel(u32 base)
{
	writel(31, base + 0x0);
	writel(92, base + 0x8);
	writel(0xac, base + 0x14);
	writel(5, base + 0x18);
}

int gx6702_pad_init(void)
{
	u32 v;

	/* Clock routes and reset gates must already have been set by the IPL. */

	/* ---- pad / mux window (0x93CE66C6 / 6B61C @ 0x9316b71c..) ---- */
	gx_clrset(0xA030A068, (1U << 23), 0);
	gx_clrset(0xA030A00C, (1U << 8), 0);

	v = readl(0xA030A17C);
	v &= ~(1U << 23);
	writel(v, 0xA030A17C);
	v = readl(0xA030A17C);
	v |= (1U << 23);
	writel(v, 0xA030A17C);
	v = readl(0xA030A17C);
	v = (v & 0xFFC0FFFF) | 0x002B0000;
	writel(v, 0xA030A17C);
	v = readl(0xA030A17C);
	v &= ~(1U << 22);
	writel(v, 0xA030A17C);
	v = readl(0xA030A17C);
	v |= (1U << 22);
	writel(v, 0xA030A17C);

	gx_clrset(0xA030A174, (1U << 1) | (1U << 2), 0);

	v = readl(0xA030A114);
	v |= (1U << 23);
	writel(v, 0xA030A114);
	v = readl(0xA030A114);
	v &= ~((1U << 0) | (1U << 1));
	writel(v, 0xA030A114);
	v = readl(0xA030A114);
	v &= ~((1U << 9) | (1U << 10));
	writel(v, 0xA030A114);
	v = readl(0xA030A114);
	v &= ~((1U << 7) | (1U << 8));
	writel(v, 0xA030A114);
	v = readl(0xA030A114);
	v |= (1U << 8);
	writel(v, 0xA030A114);
	v = readl(0xA030A114);
	v &= ~0x70U;
	writel(v, 0xA030A114);
	v = readl(0xA030A114);
	v |= (1U << 5) | (1U << 6);
	writel(v, 0xA030A114);
	v = readl(0xA030A114);
	v &= 0xFFFF07FF;
	writel(v, 0xA030A114);
	v = readl(0xA030A114);
	v |= (1U << 11);
	writel(v, 0xA030A114);
	v = readl(0xA030A114);
	v &= ~(1U << 3);
	writel(v, 0xA030A114);
	v = readl(0xA030A114);
	v &= 0xFFF0FFFF;
	writel(v, 0xA030A114);
	v = readl(0xA030A114);
	v |= 0x00070000;
	writel(v, 0xA030A114);
	v = readl(0xA030A114);
	v &= ~(1U << 2);
	writel(v, 0xA030A114);
	v = readl(0xA030A114);
	v |= (1U << 2);
	writel(v, 0xA030A114);

	gx_clrset(0xA030A170, 0, (1U << 16) | (1U << 17) | (1U << 18));

	v = readl(0xA030A1B0);
	v |= (1U << 8);
	v &= ~((1U << 6) | (1U << 7));
	writel(v, 0xA030A1B0);

	writel(0, 0xA48040CC);
	writel(0, 0xA48040DC);
	writel(0, 0xA49040CC);
	writel(0, 0xA49040DC);

	v = readl(0xA030A200);
	v &= 0xFFFC03FF;
	writel(v, 0xA030A200);
	v = readl(0xA030A200);
	v &= ~0x7CU;
	writel(v, 0xA030A200);
	v = readl(0xA030A200);
	v |= (1U << 3) | (1U << 4);
	writel(v, 0xA030A200);
	v = readl(0xA030A200);
	v &= 0xE0FFFFFF;
	writel(v, 0xA030A200);
	v = readl(0xA030A200);
	v |= 0x1D000000;
	writel(v, 0xA030A200);
	v = readl(0xA030A200);
	v |= 0xE0000000;
	writel(v, 0xA030A200);
	v = readl(0xA030A200);
	v &= 0xFFF3FEFF;
	writel(v, 0xA030A200);
	v = readl(0xA030A200);
	v |= (1U << 8);
	writel(v, 0xA030A200);
	v = readl(0xA030A200);
	v &= ~((1U << 22) | (1U << 23));
	writel(v, 0xA030A200);
	v = readl(0xA030A200);
	v &= ~(1U << 29);
	writel(v, 0xA030A200);
	v = readl(0xA030A200);
	v |= (1U << 29);
	writel(v, 0xA030A200);
	v = readl(0xA030A200);
	v |= (1U << 18) | (1U << 19);
	writel(v, 0xA030A200);
	v = readl(0xA030A200);
	v &= ~((1U << 7) | (1U << 20));
	writel(v, 0xA030A200);

	return 0;
}

int gx6702_usb_enable(void)
{
	u32 v;

	gx6702_pad_init();

	/* ---- USB PHY (0x93CE680A..) ---- */
	gx_usb_program_phy_channel(GX_USB_PHY0_BASE);
	gx_usb_program_phy_channel(GX_USB_PHY1_BASE);

	gx_clrset(GX_USB_CFG_BASE + 0x0, 0, 0x820F2000);
	gx_clrset(GX_USB_CFG_BASE + 0xC, 0, (1U << 25));

	gx_usb_phy_bits_clear_en();
	gx_usb_phy_bits_set_mid();
	gx_usb_phy_bits_clear_mid();
	gx_usb_phy_bits_set_en();
	gx_usb_phy_bits_clear_en();
	gx_usb_phy_bits_set_mid();
	gx_usb_phy_bits_clear_mid();

	/* Port / VBUS-related pad bits */
	v = readl(0xA030AA08);
	v &= ~((1U << 12) | (1U << 13));
	writel(v, 0xA030AA08);
	v = readl(0xA030AA08);
	v |= (1U << 13);
	writel(v, 0xA030AA08);

	gx_clrset(0xA030A20C, 0, (1U << 0));

	mdelay(10);

	return 0;
}
