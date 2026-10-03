// SPDX-License-Identifier: GPL-2.0+
/*
 * GX6706 USB host clock, pad, and PHY bring-up.
 *
 * Clean-room sequence from the Cygnus H5/S5 loader, as transcribed in
 * gxipl. EHCI is stock generic-ehci at 0xA0904000. PHY trim lives in the
 * 0xA0702xxx pad block. Do not run the GX6702 pad or 0xA0908xxx PHY path.
 */

#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <asm/io.h>
#include "gx6706_usb.h"

#define GX6706_USB_CFG_BASE	0xA090A000

#define CYGNUS_NO_GATE		0xffu
#define CYGNUS_F_OVR_FIRST	BIT(0)
#define CYGNUS_F_OVR_SECOND	BIT(1)

struct cygnus_row {
	u8 tg;
	u8 gshift;
	u8 flags;
	u8 fshift;
	u32 clear;
	u32 value;
};

static const struct cygnus_route {
	u8 index;
	u8 gate;
	u32 value;
	u32 gate_mask;
} cygnus_usb_routes[] = {
	{  1, 1, 0x05555555, 0x00000001 },
	{  2, 1, 0x05555555, 0x00000002 },
	{  3, 1, 0x15555555, 0x00000200 },
	{  4, 1, 0x0ccccccc, 0x00000400 },
	{  5, 1, 0x0ccccccc, 0x00000800 },
	{  6, 1, 0x10000000, 0x00001000 },
	{  7, 1, 0x0ccccccc, 0x00002000 },
	{  8, 1, 0x09249249, 0x00004000 },
	{  9, 1, 0x05d1745d, 0x00008000 },
	{ 12, 2, 0x0ccccccc, 0x00000200 },
	{ 13, 2, 0x0aaaaaaa, 0x00080000 },
	{ 14, 2, 0x08000000, 0x00000008 },
	{ 16, 2, 0x10000000, 0x00000001 },
	{ 17, 2, 0x0ccccccc, 0x00000800 },
	{ 18, 2, 0x10000000, 0x00000400 },
};

static const u32 cygnus_first_ovr[] = { 0x00000000 };
static const u32 cygnus_second_ovr[] = { 0x00000040, 0x00000000 };

static const struct cygnus_row cygnus_usb_fields[] = {
	{ (1 << 4) | 2, CYGNUS_NO_GATE, 0, 31, 0xff800000, 0x0a800000 },
	{ (1 << 4) | 1,  4, 0, 19, 0x000ff000, 0x0002b000 },
	{ (1 << 4) | 1,  3, 0,  7, 0x000000ff, 0x00000007 },
	{ (2 << 4) | 1, 19, 0, 31, 0xff000000, 0x0e000000 },
	{ (2 << 4) | 1, 21, 0, 23, 0x00ff0000, 0x00050000 },
	{ (2 << 4) | 1, 22, 0, 15, 0x0000ff00, 0x00000900 },
	{ (3 << 4) | 1, 27, 0, 31, 0xff000000, 0x03000000 },
	{ (3 << 4) | 2, 29, 0, 23, 0x00ff0000, 0x002b0000 },
	{ (3 << 4) | 1, CYGNUS_NO_GATE, CYGNUS_F_OVR_SECOND, 6,
	  0x00000078, 0x00000038 },
	{ (3 << 4) | 1, CYGNUS_NO_GATE,
	  CYGNUS_F_OVR_FIRST | CYGNUS_F_OVR_SECOND, CYGNUS_NO_GATE,
	  0x00000007, 0x00000007 },
};

/*
 * The route/field writes below can stop the free-running timer that udelay()
 * uses.  A timed delay here would never return, so spin on the CPU instead.
 */
static void gx6706_delay(u32 spins)
{
	volatile u32 i;

	for (i = 0; i < spins; i++)
		;
}

/* Match drivers/timer/gx6605s_timer.c clock-source half at +0x40. */
static void gx6706_timer_rearm(void)
{
	void __iomem *base = (void __iomem *)0xa020a040;

	writel(0, base + 0x24);
	writel(0, base + 0x28);
	writel(BIT(0), base + 0x10);
	writel(BIT(0), base + 0x20);
	writel(BIT(1), base + 0x10);
}

static void gx_clrset(u32 addr, u32 clear, u32 set)
{
	u32 v = readl(addr);

	v = (v & ~clear) | set;
	writel(v, addr);
}

static u32 cygnus_target(u8 target)
{
	if (target == 1)
		return 0xa030a024;
	if (target == 2)
		return 0xa030a178;
	return 0xa030a17c;
}

static u32 cygnus_gate(u8 gate)
{
	return gate == 2 ? 0xa030a174 : 0xa030a170;
}

static u32 cygnus_bit(u8 shift)
{
	return shift == CYGNUS_NO_GATE ? 0 : (1U << shift);
}

static void gx6706_timer_stop(void)
{
	writel(0, 0xa020a020);
	writel(BIT(0), 0xa020a010);
	writel(0, 0xa020a060);
	writel(BIT(0), 0xa020a050);
}

static u32 gx6706_wdt_pause(void)
{
	u32 ctrl = readl(0xa020b000);

	writel(ctrl & ~3u, 0xa020b000);
	return ctrl;
}

static void cygnus_usb_clocks(void)
{
	u32 i;
	u32 wdt = 0;
	int paused = 0;

	/* USB PLL: packed 0x405f7e00 maps to 0x0011102d. */
	writel(0x7811102d, 0xa030a0cc);
	gx6706_delay(1000);
	writel(0x0011102d, 0xa030a0cc);

	for (i = 0; i < ARRAY_SIZE(cygnus_usb_routes); i++) {
		const struct cygnus_route *r = &cygnus_usb_routes[i];
		u32 addr = 0xa0600ffc + (u32)r->index * 4;
		u32 load = r->value | BIT(30);
		u32 apply = load | BIT(31);

		if (r->index >= 13 && !paused) {
			wdt = gx6706_wdt_pause();
			gx6706_timer_stop();
			paused = 1;
		}
		if (r->index >= 13) {
			u32 cur = readl(addr);

			/*
			 * The two-step update writes BIT(30) with BIT(31) clear.
			 * On route 13 that decommits a live divider and the store
			 * never completes.  Enable the gate first, then commit
			 * with BIT(31) left set.
			 */
			gx_clrset(cygnus_gate(r->gate), 0, r->gate_mask);
			if ((cur & 0xbfffffff) != (apply & 0xbfffffff))
				writel(apply, addr);
		} else {
			writel(load, addr);
			writel(apply, addr);
			gx_clrset(cygnus_gate(r->gate), 0, r->gate_mask);
		}
	}
	if (paused)
		writel(wdt, 0xa020b000);
	for (i = 0; i < ARRAY_SIZE(cygnus_usb_fields); i++) {
		const struct cygnus_row *f = &cygnus_usb_fields[i];
		u32 addr = cygnus_target(f->tg >> 4);
		u32 first = cygnus_bit(f->fshift);
		u32 second = first >> 1;
		u32 value = readl(addr);

		if (f->flags & CYGNUS_F_OVR_FIRST)
			first = cygnus_first_ovr[0];
		if (f->flags & CYGNUS_F_OVR_SECOND)
			second = cygnus_second_ovr[i -
				(ARRAY_SIZE(cygnus_usb_fields) - 2)];
		value = (value & ~f->clear) | f->value | second;
		writel(value, addr);
		writel(value | first, addr);
		gx_clrset(cygnus_gate(f->tg & 0xf), 0, cygnus_bit(f->gshift));
	}
	gx_clrset(0xa030a170, 0,
		  0x07000000 | BIT(30) | BIT(23) | 0x300001e0 | 0x00070000);
	gx_clrset(0xa030a174, 0,
		  BIT(1) | BIT(2) | BIT(14) | 0x1f800000 | BIT(4));
}

static void usb_phy_bits_clear_en(void)
{
	gx_clrset(GX6706_USB_CFG_BASE + 0x4, BIT(10) | BIT(26), 0);
	gx_clrset(GX6706_USB_CFG_BASE + 0x8, BIT(26), 0);
}

static void usb_phy_bits_set_mid(void)
{
	gx_clrset(GX6706_USB_CFG_BASE + 0x4, 0, BIT(12) | BIT(28));
	gx_clrset(GX6706_USB_CFG_BASE + 0x8, 0, BIT(28));
}

static void usb_phy_bits_clear_mid(void)
{
	gx_clrset(GX6706_USB_CFG_BASE + 0x4, BIT(12) | BIT(28), 0);
	gx_clrset(GX6706_USB_CFG_BASE + 0x8, BIT(28), 0);
}

static void usb_phy_bits_set_en(void)
{
	gx_clrset(GX6706_USB_CFG_BASE + 0x4, 0, BIT(10) | BIT(26));
	gx_clrset(GX6706_USB_CFG_BASE + 0x8, 0, BIT(26));
}

int gx6706_usb_enable(void)
{
	static int done;

	/* EHCI and both OHCI companions share one PHY.  A second pass wedges. */
	if (done)
		return 0;

	cygnus_usb_clocks();
	gx_clrset(0xa030a1b0, BIT(6) | BIT(7), BIT(8));
	gx_clrset(0xa030a200, 0, BIT(0) | BIT(1));
	writel(5, 0xa0702018);
	writel(5, 0xa0702418);
	gx_clrset(GX6706_USB_CFG_BASE + 0x0, 0, 0x820f2000);
	gx_clrset(GX6706_USB_CFG_BASE + 0xc, 0, BIT(25));
	usb_phy_bits_clear_en();
	usb_phy_bits_set_mid();
	usb_phy_bits_clear_mid();
	usb_phy_bits_set_en();
	usb_phy_bits_clear_en();
	usb_phy_bits_set_mid();
	usb_phy_bits_clear_mid();
	gx6706_delay(200000);
	gx6706_timer_rearm();
	gx6706_timer_check();
	done = 1;

	return 0;
}
