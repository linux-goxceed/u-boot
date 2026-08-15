// SPDX-License-Identifier: GPL-2.0+
/*
 * NationalChip GX6702 display output: HDMI plus CVBS/YPbPr.
 *
 * HDMI and YPbPr use the primary DVE at 0xA4804000.  CVBS is independent:
 * the SVPU at 0xA4900000 clones/downscales the primary display and its DVE at
 * 0xA4904000 generates PAL 720x576i50 or NTSC 720x480i60.  This is the
 * topology used by GxLoader/eCos; merely opening the analogue DACs does not
 * produce composite sync.
 *
 * U-Boot's console wants a linear RGB framebuffer but the graphics layer only
 * fetches UYVY 4:2:2, so the uclass framebuffer is a shadow that video_sync()
 * converts into the buffer the display DMA actually reads.
 *
 * The register sequences come from gx3201_videoout_init() at 0x931713C0 in the
 * 64 KiB vendor BOOT.bin; see gx6702_video.h for the map.
 */

#include <cpu_func.h>
#include <cyclic.h>
#include <dm.h>
#include <errno.h>
#include <fdtdec.h>
#include <log.h>
#include <mapmem.h>
#include <splash.h>
#include <time.h>
#include <video.h>
#include <video_console.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/sizes.h>

#include "gx6702_video.h"
#include "gx6702_ecos_goldens.h"
#include "gx6702_efuse.h"

/* HPD poll period; two matching samples debounce cable bounce. */
#define GX6702_HPD_POLL_US		500000
#define GX6702_HPD_DEBOUNCE		2

#define GX6702_HW_BYTES_PER_PIXEL	2	/* UYVY 4:2:2 */
#define GX6702_FB_MAX_W			1920
#define GX6702_FB_MAX_H			1080
#define GX6702_SVPU_MEM_SIZE		(812 * SZ_1K)

/* Match GxLoader's fixed input window and per-plane allocation geometry. */
#define GX6702_JPEG_BS_SIZE		(2 * SZ_1M)
#define GX6702_JPEG_STRIDE		GX6702_FB_MAX_W
#define GX6702_JPEG_LINES		ALIGN(GX6702_FB_MAX_H, 16)
#define GX6702_JPEG_PLANE_SIZE		(ALIGN(GX6702_JPEG_STRIDE * \
					       GX6702_JPEG_LINES, SZ_4K) + SZ_4K)
#define GX6702_JPEG_MEM_SIZE		GX6702_JPEG_BS_SIZE
#define GX6702_JPEG_TIMEOUT_MS		1000

/* gx3201_videoout_init(), 0x931717be: six pointers repeated three times. */
static const u32 gx6702_svpu_ptr_offsets[] = {
	0x00000, 0x002d0, 0x65400, 0x656d0, 0xca800, 0xcaad0,
};

/*
 * Indexed by enum gx6702_video_mode minus one (eCos video>mode 1..10).
 * vic / dve_code / sync flags match gxtest/goldens/dumps/; porches are
 * CEA-861.  Interlaced vactive is field height (DVE + dw_hdmi).
 */
static const struct gx6702_video_mode_info gx6702_modes[] = {
	[GX6702_MODE_480I60 - 1] = {
		.name = "720x480i@60", .vic = 6, .dve_code = 0x04,
		.dve_line = 0x8901, .cvbs_standard = GX6702_CVBS_NTSC,
		.svpu_source = 0x08,
		.timing = {
			.pixelclock = { .typ = 27000000 },
			.hactive = { .typ = 720 }, .hfront_porch = { .typ = 16 },
			.hsync_len = { .typ = 62 }, .hback_porch = { .typ = 60 },
			.vactive = { .typ = 240 }, .vfront_porch = { .typ = 4 },
			.vsync_len = { .typ = 3 }, .vback_porch = { .typ = 15 },
			.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW |
				 DISPLAY_FLAGS_INTERLACED,
		},
	},
	[GX6702_MODE_480P60 - 1] = {
		.name = "720x480p@60", .vic = 2, .dve_code = 0x08,
		.dve_line = 0x11A03, .cvbs_standard = GX6702_CVBS_NTSC,
		.svpu_source = 0x08,
		.timing = {
			.pixelclock = { .typ = 27000000 },
			.hactive = { .typ = 720 }, .hfront_porch = { .typ = 16 },
			.hsync_len = { .typ = 62 }, .hback_porch = { .typ = 60 },
			.vactive = { .typ = 480 }, .vfront_porch = { .typ = 9 },
			.vsync_len = { .typ = 6 }, .vback_porch = { .typ = 30 },
			.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW,
		},
	},
	[GX6702_MODE_576I50 - 1] = {
		.name = "720x576i@50", .vic = 21, .dve_code = 0x00,
		.dve_line = 0xA935, .cvbs_standard = GX6702_CVBS_PAL,
		.svpu_source = 0x0B,
		.timing = {
			.pixelclock = { .typ = 27000000 },
			.hactive = { .typ = 720 }, .hfront_porch = { .typ = 12 },
			.hsync_len = { .typ = 63 }, .hback_porch = { .typ = 69 },
			.vactive = { .typ = 288 }, .vfront_porch = { .typ = 2 },
			.vsync_len = { .typ = 3 }, .vback_porch = { .typ = 19 },
			.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW |
				 DISPLAY_FLAGS_INTERLACED,
		},
	},
	[GX6702_MODE_576P50 - 1] = {
		/* True progressive 576p from eCos (VIC 17, INVIDCONF 0x18). */
		.name = "720x576p@50", .vic = 17, .dve_code = 0x09,
		.dve_line = 0x15A6B, .cvbs_standard = GX6702_CVBS_PAL,
		.svpu_source = 0x05,
		.timing = {
			.pixelclock = { .typ = 27000000 },
			.hactive = { .typ = 720 }, .hfront_porch = { .typ = 12 },
			.hsync_len = { .typ = 64 }, .hback_porch = { .typ = 68 },
			.vactive = { .typ = 576 }, .vfront_porch = { .typ = 5 },
			.vsync_len = { .typ = 5 }, .vback_porch = { .typ = 39 },
			.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW,
		},
	},
	[GX6702_MODE_720P50 - 1] = {
		.name = "1280x720p@50", .vic = 19, .dve_code = 0x2D,
		.dve_hd = true, .dve_line = 0xC2E8,
		.cvbs_standard = GX6702_CVBS_PAL, .svpu_source = 0x05,
		.timing = {
			.pixelclock = { .typ = 74250000 },
			.hactive = { .typ = 1280 }, .hfront_porch = { .typ = 440 },
			.hsync_len = { .typ = 40 }, .hback_porch = { .typ = 220 },
			.vactive = { .typ = 720 }, .vfront_porch = { .typ = 5 },
			.vsync_len = { .typ = 5 }, .vback_porch = { .typ = 20 },
			.flags = DISPLAY_FLAGS_HSYNC_HIGH | DISPLAY_FLAGS_VSYNC_HIGH,
		},
	},
	[GX6702_MODE_720P60 - 1] = {
		.name = "1280x720p@60", .vic = 4, .dve_code = 0x0D,
		.dve_hd = true, .dve_line = 0xC2E8,
		.cvbs_standard = GX6702_CVBS_NTSC, .svpu_source = 0x05,
		.timing = {
			.pixelclock = { .typ = 74250000 },
			.hactive = { .typ = 1280 }, .hfront_porch = { .typ = 110 },
			.hsync_len = { .typ = 40 }, .hback_porch = { .typ = 220 },
			.vactive = { .typ = 720 }, .vfront_porch = { .typ = 5 },
			.vsync_len = { .typ = 5 }, .vback_porch = { .typ = 20 },
			.flags = DISPLAY_FLAGS_HSYNC_HIGH | DISPLAY_FLAGS_VSYNC_HIGH,
		},
	},
	[GX6702_MODE_1080I50 - 1] = {
		.name = "1920x1080i@50", .vic = 20, .dve_code = 0x26,
		.dve_hd = true, .dve_line = 0x9A2F,
		.cvbs_standard = GX6702_CVBS_PAL, .svpu_source = 0x03,
		.timing = {
			.pixelclock = { .typ = 74250000 },
			.hactive = { .typ = 1920 }, .hfront_porch = { .typ = 528 },
			.hsync_len = { .typ = 44 }, .hback_porch = { .typ = 148 },
			.vactive = { .typ = 540 }, .vfront_porch = { .typ = 2 },
			.vsync_len = { .typ = 5 }, .vback_porch = { .typ = 15 },
			/* eCos INVIDCONF 0x7b: H+/V+ interlaced HDMI. */
			.flags = DISPLAY_FLAGS_HSYNC_HIGH | DISPLAY_FLAGS_VSYNC_HIGH |
				 DISPLAY_FLAGS_INTERLACED,
		},
	},
	[GX6702_MODE_1080I60 - 1] = {
		.name = "1920x1080i@60", .vic = 5, .dve_code = 0x06,
		.dve_hd = true, .dve_line = 0x9A2F,
		.cvbs_standard = GX6702_CVBS_NTSC, .svpu_source = 0x03,
		.timing = {
			.pixelclock = { .typ = 74250000 },
			.hactive = { .typ = 1920 }, .hfront_porch = { .typ = 88 },
			.hsync_len = { .typ = 44 }, .hback_porch = { .typ = 148 },
			.vactive = { .typ = 540 }, .vfront_porch = { .typ = 2 },
			.vsync_len = { .typ = 5 }, .vback_porch = { .typ = 15 },
			.flags = DISPLAY_FLAGS_HSYNC_HIGH | DISPLAY_FLAGS_VSYNC_HIGH |
				 DISPLAY_FLAGS_INTERLACED,
		},
	},
	[GX6702_MODE_1080P50 - 1] = {
		.name = "1920x1080p@50", .vic = 31, .dve_code = 0x2E,
		.dve_hd = true, .dve_line = 0x14460,
		.cvbs_standard = GX6702_CVBS_PAL, .svpu_source = 0x01,
		.timing = {
			.pixelclock = { .typ = 148500000 },
			.hactive = { .typ = 1920 }, .hfront_porch = { .typ = 528 },
			.hsync_len = { .typ = 44 }, .hback_porch = { .typ = 148 },
			.vactive = { .typ = 1080 }, .vfront_porch = { .typ = 4 },
			.vsync_len = { .typ = 5 }, .vback_porch = { .typ = 36 },
			.flags = DISPLAY_FLAGS_HSYNC_HIGH | DISPLAY_FLAGS_VSYNC_HIGH,
		},
	},
	[GX6702_MODE_1080P60 - 1] = {
		.name = "1920x1080p@60", .vic = 16, .dve_code = 0x0E,
		.dve_hd = true, .dve_line = 0x14460,
		.cvbs_standard = GX6702_CVBS_NTSC, .svpu_source = 0x01,
		.timing = {
			.pixelclock = { .typ = 148500000 },
			.hactive = { .typ = 1920 }, .hfront_porch = { .typ = 88 },
			.hsync_len = { .typ = 44 }, .hback_porch = { .typ = 148 },
			.vactive = { .typ = 1080 }, .vfront_porch = { .typ = 4 },
			.vsync_len = { .typ = 5 }, .vback_porch = { .typ = 36 },
			.flags = DISPLAY_FLAGS_HSYNC_HIGH | DISPLAY_FLAGS_VSYNC_HIGH,
		},
	},
};

const struct gx6702_video_mode_info *gx6702_video_mode_info(unsigned int mode)
{
	if (mode < GX6702_MODE_480I60 || mode >= GX6702_MODE_COUNT)
		return NULL;

	return &gx6702_modes[mode - 1];
}

/*
 * BT.601 limited range, scaled by 256.  The order is Y, Cb, Cr and within each
 * row R, G, B followed by the offset.
 *
 * BT.601 is used for the HD modes too, because the only inverse matrix
 * dw_hdmi.c carries is csc_coeff_rgb_out_eitu601: pairing anything else with
 * it would tint the picture rather than round-trip.
 */
static const s16 gx6702_csc_bt601[3][4] = {
	{   66,  129,   25,  16 },
	{  -38,  -74,  112, 128 },
	{  112,  -94,  -18, 128 },
};

struct gx6702_video_priv {
	const struct gx6702_video_mode_info *mode;
	void *hw_fb;
	void *svpu_fb;
	void *jpeg_bs;
	void *jpeg_plane[3];
	u32 hw_line_length;
	unsigned int mode_index;	/* last concrete eCos index 1..10 */
	bool cvbs_requested;	/* false for nationalchip,ypbpr-only */
	bool cvbs_active;
	u8 cvbs_standard;
	bool auto_mode;
	bool hpd_connected;
	u8 hpd_debounce;
	bool fb_busy;		/* suppress RGB→UYVY while mode/logo updates */
	struct cyclic_info cyclic;
	struct udevice *dev;
};

/* PAL-capable preference used before the normal/native order when requested. */
static const u8 gx6702_auto_pal_pref[] = {
	GX6702_MODE_1080P50,
	GX6702_MODE_1080I50,
	GX6702_MODE_720P50,
	GX6702_MODE_576P50,
	GX6702_MODE_576I50,
};

/* Normal HDMI preference when EDID has no native SVD we understand. */
static const u8 gx6702_auto_pref[] = {
	GX6702_MODE_1080P60,
	GX6702_MODE_1080P50,
	GX6702_MODE_1080I60,
	GX6702_MODE_1080I50,
	GX6702_MODE_720P60,
	GX6702_MODE_720P50,
	GX6702_MODE_576P50,
	GX6702_MODE_480P60,
	GX6702_MODE_576I50,
	GX6702_MODE_480I60,
};

static unsigned int gx6702_mode_for_vic(u8 vic)
{
	unsigned int i;

	if (!vic)
		return 0;

	for (i = GX6702_MODE_480I60; i < GX6702_MODE_COUNT; i++) {
		if (gx6702_modes[i - 1].vic == vic)
			return i;
	}

	return 0;
}

static unsigned int gx6702_video_pick_auto_mode_policy(bool prefer_pal)
{
	bool edid_ok = false;
	u8 native, vic;
	unsigned int mode, native_mode, i;

	native = gx6702_hdmi_cea_native_vic(&edid_ok);
	native_mode = gx6702_mode_for_vic(native);

	if (edid_ok && prefer_pal) {
		for (i = 0; i < ARRAY_SIZE(gx6702_auto_pal_pref); i++) {
			mode = gx6702_auto_pal_pref[i];
			vic = gx6702_modes[mode - 1].vic;
			if (gx6702_hdmi_sink_supports_vic(vic, NULL))
				return mode;
		}
	}

	if (native_mode && edid_ok &&
	    gx6702_hdmi_sink_supports_vic(native, NULL))
		return native_mode;

	if (edid_ok) {
		for (i = 0; i < ARRAY_SIZE(gx6702_auto_pref); i++) {
			mode = gx6702_auto_pref[i];
			vic = gx6702_modes[mode - 1].vic;
			if (gx6702_hdmi_sink_supports_vic(vic, NULL))
				return mode;
		}
	}

	return GX6702_MODE_1080I50;
}

unsigned int gx6702_video_pick_auto_mode(void)
{
	struct udevice *dev;
	struct gx6702_video_priv *priv;
	bool prefer_pal = true;

	if (!uclass_first_device_err(UCLASS_VIDEO, &dev)) {
		priv = dev_get_priv(dev);
		prefer_pal = priv->cvbs_requested;
	}

	return gx6702_video_pick_auto_mode_policy(prefer_pal);
}

unsigned int gx6702_video_current_mode(void)
{
	struct udevice *dev;
	struct gx6702_video_priv *priv;

	if (uclass_first_device_err(UCLASS_VIDEO, &dev))
		return 0;
	priv = dev_get_priv(dev);
	return priv->mode_index;
}

bool gx6702_video_auto_enabled(void)
{
	struct udevice *dev;
	struct gx6702_video_priv *priv;

	if (uclass_first_device_err(UCLASS_VIDEO, &dev))
		return false;
	priv = dev_get_priv(dev);
	return priv->auto_mode;
}

enum gx6702_cvbs_state gx6702_video_cvbs_state(void)
{
	struct udevice *dev;
	struct gx6702_video_priv *priv;

	if (uclass_first_device_err(UCLASS_VIDEO, &dev))
		return GX6702_CVBS_DISABLED;

	priv = dev_get_priv(dev);
	if (priv->cvbs_active)
		return priv->cvbs_standard == GX6702_CVBS_NTSC ?
			GX6702_CVBS_ACTIVE_NTSC : GX6702_CVBS_ACTIVE_PAL;

	return GX6702_CVBS_DISABLED;
}

static u32 gx6702_mode_frame_h(const struct gx6702_video_mode_info *mode)
{
	u32 ysize = mode->timing.vactive.typ;

	if (mode->timing.flags & DISPLAY_FLAGS_INTERLACED)
		ysize *= 2;

	return ysize;
}

static u32 gx6702_max_shadow_size(void)
{
	return ALIGN(GX6702_FB_MAX_W * GX6702_FB_MAX_H * sizeof(u32), SZ_4K);
}

static u32 gx6702_max_hw_size(void)
{
	return ALIGN(GX6702_FB_MAX_W * GX6702_FB_MAX_H *
		     GX6702_HW_BYTES_PER_PIXEL, SZ_4K);
}

static u32 gx6702_jpeg_mem_offset(void)
{
	return gx6702_max_shadow_size() + gx6702_max_hw_size() +
		GX6702_SVPU_MEM_SIZE;
}

static inline ulong gx6702_bus_addr(void *cpu_addr)
{
	return (ulong)cpu_addr - GX6702_DDR_CPU_TO_BUS;
}

/* asm/io.h on C-SKY does not carry the clrsetbits_le32() family. */
static void gx6702_rmw(ulong reg, u32 clear, u32 set)
{
	writel((readl(reg) & ~clear) | set, reg);
}

/*
 * Match gx3201_dve_reset() at 0x93171318: after the display CRG pulse,
 * assert/deassert CRG_RESET0_DVE (FUN_931712f0 / 71304) with 2 ms gaps,
 * then soft-reset 0x5a.  Mode is programmed before this and survives.
 */
static void gx6702_display_reset(void)
{
	gx6702_rmw(GX6702_CRG_BASE + CRG_RESET3, 0, CRG_RESET3_DISPLAY);
	gx6702_rmw(GX6702_CRG_BASE + CRG_RESET3, CRG_RESET3_DISPLAY, 0);
	mdelay(20);

	gx6702_rmw(GX6702_CRG_BASE + CRG_RESET0, 0, CRG_RESET0_DVE);
	mdelay(2);
	gx6702_rmw(GX6702_CRG_BASE + CRG_RESET0, CRG_RESET0_DVE, 0);
	mdelay(2);

	writel(DVE_SOFT_RESET_MAGIC, GX6702_DVE_BASE + DVE_SOFT_RESET);
	mdelay(2);
}

/*
 * Match the per-mode divider + CRG select from gx3201_videoout_init():
 *   FUN_931715bc: div 0xC0, sel 0x2B (27 MHz SD interlaced)
 *   FUN_93171602: div 0x00, sel 0x15 (27 MHz SD progressive)
 *   FUN_9317165c: div 0x30, sel 0x07 (74.25 MHz HD)
 *   FUN_931716ba: div 0x18, sel 0x07 (148.5 MHz FHD)
 * The live eCos dumps are authoritative for its final progressive-SD
 * state: both 480p and 576p have GFX+0xf4 bits [23:16] clear, CRG+0x24
 * selector 0x15, and CRG+0x17c display gates 3/4/5 enabled.  Preserve the
 * distinct 0xc0/0x2b route for the already-working interlaced modes.
 */
static void gx6702_pixclk_set(const struct gx6702_video_mode_info *mode)
{
	bool interlaced = mode->timing.flags & DISPLAY_FLAGS_INTERLACED;
	u32 div, sel, gate0, gate1, clk;
	u32 rate = mode->timing.pixelclock.typ;

	if (rate <= 27000000) {
		div = interlaced ? GFX_PIXCLK_DIV_27M :
			GFX_PIXCLK_DIV_27M_PROGRESSIVE;
		sel = interlaced ? CRG_PIXCLK_SEL_SD_INTERLACED :
			CRG_PIXCLK_SEL_SD_PROGRESSIVE;
	} else if (rate <= 74250000) {
		div = GFX_PIXCLK_DIV_74M;
		sel = CRG_PIXCLK_SEL_HD;
	} else {
		div = GFX_PIXCLK_DIV_148M;
		sel = CRG_PIXCLK_SEL_HD;
	}

	gx6702_rmw(GX6702_GFX_BASE + GFX_PIXCLK, GFX_PIXCLK_DIV_MASK,
		   div << GFX_PIXCLK_DIV_SHIFT);

	/* Gate the display pixel path, switch source, pulse, then restore. */
	gate0 = readl(GX6702_CRG_BASE + CRG_CLK_GATE0);
	writel(gate0 & ~CRG_CLK_GATE0_BIT3, GX6702_CRG_BASE + CRG_CLK_GATE0);

	clk = readl(GX6702_CRG_BASE + CRG_PIXCLK_SEL);
	clk &= ~CRG_PIXCLK_SD_PROG_EXTRA_MASK;
	if (rate <= 27000000)
		clk |= CRG_PIXCLK_SD_EXTRA_VAL;
	else
		clk |= CRG_PIXCLK_NONPROG_EXTRA_VAL;
	clk &= ~CRG_PIXCLK_SEL_BIT7;
	writel(clk, GX6702_CRG_BASE + CRG_PIXCLK_SEL);
	clk |= CRG_PIXCLK_SEL_BIT6;
	writel(clk, GX6702_CRG_BASE + CRG_PIXCLK_SEL);
	clk = (clk & ~CRG_PIXCLK_SEL_DIV_MASK) | sel;
	writel(clk, GX6702_CRG_BASE + CRG_PIXCLK_SEL);
	writel(clk | CRG_PIXCLK_SEL_BIT7, GX6702_CRG_BASE + CRG_PIXCLK_SEL);
	writel(clk & ~CRG_PIXCLK_SEL_BIT7, GX6702_CRG_BASE + CRG_PIXCLK_SEL);

	writel(gate0 | CRG_CLK_GATE0_BIT3, GX6702_CRG_BASE + CRG_CLK_GATE0);

	/*
	 * Vendor GATE1 epilogue (FUN_931711cc) is rate-specific:
	 *   SD interlaced:  clear 3/4, set 5, pulse 6
	 *   SD progressive: set 3/4/5, pulse 6
	 *   HD  74M:  set 3/4/5, pulse 6
	 *   FHD 148M: clear 3/4/5, pulse 6   (FUN_931716ba)
	 */
	gate1 = readl(GX6702_CRG_BASE + CRG_CLK_GATE1);
	gate1 &= ~(CRG_CLK_GATE1_BIT3 | CRG_CLK_GATE1_BIT4 |
		   CRG_CLK_GATE1_BIT5 | CRG_CLK_GATE1_BIT6);
	if (rate <= 27000000 && interlaced)
		gate1 |= CRG_CLK_GATE1_BIT5;
	else if (rate <= 74250000)
		gate1 |= CRG_CLK_GATE1_BIT3 | CRG_CLK_GATE1_BIT4 |
			 CRG_CLK_GATE1_BIT5;
	/* else FHD: leave 3/4/5 clear */
	writel(gate1, GX6702_CRG_BASE + CRG_CLK_GATE1);
	writel(gate1 | CRG_CLK_GATE1_BIT6, GX6702_CRG_BASE + CRG_CLK_GATE1);

	log_info("gx6702-video: pixclk div=0x%02x sel=0x%02x reg=%08x\n",
		 div, sel, readl(GX6702_GFX_BASE + GFX_PIXCLK));
}

/*
 * Stock GxLoader leaves clock route 1 at 0x05555555 (a0601000=c5555555).
 * Do not touch CRG_PIXCLK_SEL / GATE1 here — those are mode-dependent and
 * must stay under gx6702_pixclk_set().
 */
static void gx6702_clock_route1_stock(void)
{
	u32 addr = 0xA0601000;
	u32 value = 0x05555555;

	writel(value | BIT(30), addr);
	writel(value | BIT(30) | BIT(31), addr);
}

static unsigned int gx6702_mode_index(const struct gx6702_video_mode_info *mode)
{
	if (mode < gx6702_modes ||
	    mode >= gx6702_modes + ARRAY_SIZE(gx6702_modes))
		return 0;

	return (unsigned int)(mode - gx6702_modes) + 1;
}

/*
 * Program DVE from the live eCos MMIO golden for this mode.  eCos uses
 * DVE_MODE 0xc3f200XX (not the GxLoader cc06/c406 assembly); LINE_LEN,
 * TIMING_SEL and the SD/HD trim words all come from the dump.
 */
static void gx6702_dve_set_mode(const struct gx6702_video_mode_info *mode)
{
	const struct gx6702_ecos_golden *g;
	unsigned int i, index = gx6702_mode_index(mode);

	g = gx6702_ecos_golden(index);
	if (!g) {
		log_warning("gx6702-video: no eCos golden for mode %u\n", index);
		writel(mode->dve_code, GX6702_DVE_BASE + DVE_MODE);
		return;
	}

	for (i = 0; i < GX6702_ECOS_DVE_WORDS; i++)
		writel(g->dve[i], GX6702_DVE_BASE + gx6702_ecos_dve_offs[i]);

	/* Stable post-mode words shared across the eCos dumps. */
	writel(DVE_REG_1B0_STOCK, GX6702_DVE_BASE + DVE_REG_1B0);
	writel(DVE_REG_1B0_STOCK, GX6702_DVE_BASE + DVE_REG_1B8);
}

/* Early analogue/DVE clock and DAC prime from GxLoader FUN_93170350. */
static void gx6702_analogue_init(void)
{
	/*
	 * The later videoout path changes DAC_CFG to 8, after DVE reset and mode
	 * programming.  Omitting this early phase leaves PAL sync but no burst.
	 */
	gx6702_rmw(GX6702_CRG_BASE + CRG_DVE_CLK_CFG,
		   CRG_DVE_CLK_CFG_CLEAR, CRG_DVE_CLK_CFG_ENABLE);
	writel(1, GX6702_GFX_BASE + GFX_DAC_CFG);
}

/*
 * Four 6-bit DAC trims come from eFuse words 0x126..0x129 (see
 * gx6702_efuse_dac_gain).  Blank bytes use vendor defaults 0x1e/0x20; a
 * controller timeout keeps the live-golden 0x1b1b1b1b fallback.
 *
 * @dac_enable picks which DACs are driven, matching the vendor's
 * video_dac_mode: GFX_DAC_ENABLE_YPBPR for a component-only board, or
 * GFX_DAC_ENABLE_ALL for the CVBS and YPbPr connectors together, which is what
 * gx3201_videoout_init() falls back to when the board never made a choice.
 */
static void gx6702_dac_enable(u32 dac_enable, u8 cvbs_standard,
			      bool pal_576i)
{
	u32 gain = gx6702_efuse_dac_gain();

	writel(dac_enable, GX6702_GFX_BASE + GFX_DAC_ENABLE);
	writel(gain, GX6702_GFX_BASE + GFX_DAC_GAIN);
	/* GxLoader seeds this before gx3211_svpu_config applies CVBS routing. */
	writel(0xFFFF0000, GX6702_SVPU_BASE + SVPU_ROUTE1);
	/* eCos field modes select low nibble 0xf; scaled PAL uses 0x8. */
	writel(cvbs_standard == GX6702_CVBS_NTSC || pal_576i ? 0x0F : 0x08,
	       GX6702_GFX_BASE + GFX_DAC_CFG);
	/* hd2015 clears this; stock golden is 0, U-Boot left 0x80000000. */
	writel(0, GX6702_GFX_BASE + 0x14c);
}

/*
 * Program graphics layer 0 for an unscaled full-screen surface.  Source and
 * destination heights stay equal so FUN_93171c80's vscale =
 * (src_h << 12) / dst_h stays at unity (SCALE 0x1000).
 *
 * Keep full-frame SRC/DST with a one-line TOP/BOTTOM split and unit stride —
 * that matched stock's field pointers and gave an unsquished logo.  Field-half
 * SRC/DST (ysize/2, stride 2*line) re-squished it on 1080i.
 */
static void gx6702_layer_setup(struct gx6702_video_priv *priv, u32 xsize,
			       u32 ysize, bool interlaced)
{
	u32 line = priv->hw_line_length;
	ulong bus = gx6702_bus_addr(priv->hw_fb);

	gx6702_rmw(GX6702_GFX_BASE + GFX_L0_CTRL, GFX_L0_CTRL_EN, 0);
	gx6702_rmw(GX6702_GFX_BASE + GFX_L0_STRIDE, GFX_L0_STRIDE_EN, 0);

	writel(0, GX6702_GFX_BASE + GFX_L0_VPHASE);
	writel(0, GX6702_GFX_BASE + GFX_L0_DST_POS);
	writel((ysize << GFX_L0_SRC_SIZE_H_SHIFT) | xsize,
	       GX6702_GFX_BASE + GFX_L0_SRC_SIZE);
	writel((ysize << GFX_L0_DST_SIZE_H_SHIFT) | xsize,
	       GX6702_GFX_BASE + GFX_L0_DST_SIZE);
	writel((GFX_SCALE_UNITY << 16) | GFX_SCALE_UNITY,
	       GX6702_GFX_BASE + GFX_L0_SCALE);
	gx6702_rmw(GX6702_GFX_BASE + GFX_L0_CTRL,
		   GFX_L0_CTRL_PIX32 | GFX_L0_CTRL_VSCALE_EN, 0);

	writel(GFX_L0_FILTER_BYPASS, GX6702_GFX_BASE + GFX_L0_HFILTER);
	writel(GFX_L0_FILTER_BYPASS, GX6702_GFX_BASE + GFX_L0_VFILTER);

	writel(bus, GX6702_GFX_BASE + GFX_L0_FB_TOP);
	writel(interlaced ? bus + line : bus, GX6702_GFX_BASE + GFX_L0_FB_BOTTOM);

	gx6702_rmw(GX6702_GFX_BASE + GFX_L0_STRIDE,
		   GFX_L0_STRIDE_MASK | GFX_L0_STRIDE_PITCH_CLEAR,
		   (line & GFX_L0_STRIDE_MASK) |
		   ((xsize >> 5) << GFX_L0_STRIDE_PITCH_SHIFT));
	gx6702_rmw(GX6702_GFX_BASE + GFX_L0_STRIDE, 0,
		   GFX_L0_STRIDE_UNSCALED | GFX_L0_STRIDE_EN);
	gx6702_rmw(GX6702_GFX_BASE + GFX_L0_CTRL, 0, GFX_L0_CTRL_EN);
}

static void gx6702_svpu_commit(void)
{
	writel(1, GX6702_SVPU_BASE + SVPU_COMMIT);
	writel(0, GX6702_SVPU_BASE + SVPU_COMMIT);
}

/*
 * Secondary-DVE NTSC-M golden captured from eCos UI mode 2.  Keep this in the
 * same sparse register order as the primary eCos goldens: these are the
 * registers the vendor mode routines actively program, rather than status or
 * reserved words from the surrounding 0x200-byte dump.
 */
static const u32 gx6702_cvbs_ntsc_dve[GX6702_ECOS_DVE_WORDS] = {
	0xCDF00004, 0x004C6D3E, 0x0008A318, 0x097218BE,
	0xE0100359, 0x63803C47, 0x00004761, 0x00000331,
	0x0101F16C, 0x04270000, 0x00001428, 0x00000003,
	0x00000926, 0x001F0090, 0x00008901, 0x0008C208,
	0x0008220B, 0x2554A500, 0x00000557, 0x10000001,
	0x00F13C00, 0x00000000, 0x00000000,
};

/* Secondary-DVE golden from the working eCos 720x576i50 analogue capture. */
static const u32 gx6702_cvbs_pal_576i_dve[GX6702_ECOS_DVE_WORDS] = {
	0xCDF00000, 0x004C6A3E, 0x000AA328, 0x0A023CCB,
	0xE010035F, 0x6A963F3F, 0x00003F61, 0x00000331,
	0x0101F170, 0x02270000, 0x00001428, 0x00000003,
	0x00000926, 0x001F8058, 0x0000A935, 0x000A726E,
	0x0009BA70, 0x00000000, 0x00000000, 0x10000001,
	0x00F13C00, 0x00000000, 0x00000000,
};

struct gx6702_cvbs_reg {
	u16 offset;
	u32 value;
};

/*
 * Field-mode words shared by the working eCos 480p/NTSC and 576i/PAL paths.
 */
static const struct gx6702_cvbs_reg gx6702_cvbs_field_extra[] = {
	{ 0x010, 0x20402E01 }, { 0x014, 0x21B05830 },
	{ 0x018, 0x25392542 }, { 0x01C, 0x2524897A },
	{ 0x0A4, 0x00000001 }, { 0x0A8, 0x20004000 },
	{ 0x0AC, 0x60828000 }, { 0x0B0, 0xA000B000 },
	{ 0x0B4, 0xC000D000 }, { 0x0C4, 0x00008A00 },
};

/* PAL counterparts needed when returning from NTSC without a cold reset. */
static const struct gx6702_cvbs_reg gx6702_cvbs_pal_extra[] = {
	{ 0x010, 0x00681A11 }, { 0x014, 0x00F0A215 },
	{ 0x018, 0x26404547 }, { 0x01C, 0x1F64897A },
	{ 0x0A4, 0x00000000 }, { 0x0A8, 0x10002083 },
	{ 0x0AC, 0x40866082 }, { 0x0B0, 0xA000B083 },
	{ 0x0B4, 0xD081F086 }, { 0x0C4, 0x00008A0C },
};

/*
 * Initialize the encoder behind the SVPU. NTSC and PAL 576i use their captured
 * eCos secondary-DVE goldens.  Scaled PAL modes mostly match the primary 576i
 * DVE, plus the secondary-DVE values present in both GxLoader PAL dumps.
 */
static void gx6702_cvbs_dve_setup(const struct gx6702_video_mode_info *mode)
{
	const struct gx6702_ecos_golden *pal =
		gx6702_ecos_golden(GX6702_MODE_576I50);
	const u32 *field_dve = NULL;
	bool pal_576i = gx6702_mode_index(mode) == GX6702_MODE_576I50;
	bool interlaced = mode->timing.flags & DISPLAY_FLAGS_INTERLACED;
	u32 trim;
	unsigned int i;

	if (pal_576i)
		field_dve = gx6702_cvbs_pal_576i_dve;
	else if (mode->cvbs_standard == GX6702_CVBS_NTSC)
		field_dve = gx6702_cvbs_ntsc_dve;

	if (field_dve) {
		for (i = 0; i < GX6702_ECOS_DVE_WORDS; i++)
			writel(field_dve[i], GX6702_CVBS_DVE_BASE +
			       gx6702_ecos_dve_offs[i]);
		for (i = 0; i < ARRAY_SIZE(gx6702_cvbs_field_extra); i++)
			writel(gx6702_cvbs_field_extra[i].value,
			       GX6702_CVBS_DVE_BASE +
			       gx6702_cvbs_field_extra[i].offset);

		writel(CVBS_DVE_SHADOW_EN,
		       GX6702_CVBS_DVE_BASE + CVBS_DVE_SHADOW0);
		writel(CVBS_DVE_SHADOW_EN,
		       GX6702_CVBS_DVE_BASE + CVBS_DVE_SHADOW1);
		writel(CVBS_DVE_SHADOW_EN,
		       GX6702_CVBS_DVE_BASE + CVBS_DVE_SHADOW2);
		return;
	}

	if (mode->timing.pixelclock.typ > 27000000)
		trim = DVE_MODE_TRIM_HD;
	else if (interlaced)
		trim = DVE_MODE_TRIM_SD_INTERLACED;
	else
		trim = DVE_MODE_TRIM_SD;

	for (i = 0; i < GX6702_ECOS_DVE_WORDS; i++)
		writel(pal->dve[i], GX6702_CVBS_DVE_BASE +
		       gx6702_ecos_dve_offs[i]);

	writel(0x000b0328, GX6702_CVBS_DVE_BASE + CVBS_DVE_STD_CTRL);
	writel(0x02270000, GX6702_CVBS_DVE_BASE + CVBS_DVE_TIMING);
	writel(0x00000926, GX6702_CVBS_DVE_BASE + CVBS_DVE_OUTPUT);
	writel(0, GX6702_CVBS_DVE_BASE + CVBS_DVE_REG_16C);
	writel(0, GX6702_CVBS_DVE_BASE + CVBS_DVE_REG_1A8);
	for (i = 0; i < ARRAY_SIZE(gx6702_cvbs_pal_extra); i++)
		writel(gx6702_cvbs_pal_extra[i].value,
		       GX6702_CVBS_DVE_BASE + gx6702_cvbs_pal_extra[i].offset);

	gx6702_rmw(GX6702_CVBS_DVE_BASE + DVE_MODE,
		   DVE_MODE_CODE_MASK | DVE_MODE_TRIM_MASK,
		   DVE_MODE_EN | trim);
	gx6702_rmw(GX6702_CVBS_DVE_BASE + DVE_MODE, BIT(30), 0);
	gx6702_rmw(GX6702_CVBS_DVE_BASE + CVBS_DVE_ROUTE,
		   CVBS_DVE_ROUTE_MASK, CVBS_DVE_ROUTE_CVBS);

	writel(CVBS_DVE_SHADOW_EN,
	       GX6702_CVBS_DVE_BASE + CVBS_DVE_SHADOW0);
	writel(CVBS_DVE_SHADOW_EN,
	       GX6702_CVBS_DVE_BASE + CVBS_DVE_SHADOW1);
	writel(CVBS_DVE_SHADOW_EN,
	       GX6702_CVBS_DVE_BASE + CVBS_DVE_SHADOW2);
}

static void gx6702_svpu_disable(void)
{
	gx6702_rmw(GX6702_SVPU_BASE + SVPU_CTRL, SVPU_CTRL_EN, 0);
	gx6702_rmw(GX6702_SVPU_BASE + SVPU_ENABLE, SVPU_ENABLE_EN, 0);
	gx6702_svpu_commit();
}

/*
 * Port of gx3211_svpu_config() in the 64 KiB GxLoader.  Its 812 KiB
 * svpumem region is addressed through six field/working pointers, repeated
 * in three hardware banks.  The SVPU then clones the primary display into a
 * fixed PAL/NTSC SD stream; the CPU never renders into this memory.
 */
static void gx6702_svpu_setup(struct gx6702_video_priv *priv,
			      const struct gx6702_video_mode_info *mode)
{
	ulong bus = gx6702_bus_addr(priv->svpu_fb);
	bool ntsc = mode->cvbs_standard == GX6702_CVBS_NTSC;
	bool pal_576i = gx6702_mode_index(mode) == GX6702_MODE_576I50;
	bool field_mode = ntsc || pal_576i;
	u32 source_w = mode->timing.hactive.typ;
	u32 source_h = gx6702_mode_frame_h(mode);
	unsigned int bank, i;

	gx6702_svpu_disable();

	for (bank = 0; bank < SVPU_PTR_BANKS; bank++) {
		for (i = 0; i < ARRAY_SIZE(gx6702_svpu_ptr_offsets); i++) {
			ulong ptr = field_mode && i >= 4 ? 0 :
				bus + gx6702_svpu_ptr_offsets[i];

			writel(ptr,
			       GX6702_SVPU_BASE + bank * SVPU_PTR_BANK_STRIDE +
			       i * sizeof(u32));
		}
	}

	gx6702_cvbs_dve_setup(mode);

	/* Source-raster selector and PAL/NTSC geometry control. */
	gx6702_rmw(GX6702_SVPU_BASE + SVPU_CTRL,
		   SVPU_CTRL_SOURCE_MASK | SVPU_CTRL_STD_MASK |
		   SVPU_CTRL_SHADOW,
		   (mode->svpu_source << SVPU_CTRL_SOURCE_SHIFT) |
		   (field_mode ? SVPU_CTRL_STD_NTSC : SVPU_CTRL_STD_PAL));
	gx6702_svpu_commit();
	/* Preserve GxLoader's separate PAL shadow-enable latch. */
	if (!field_mode) {
		gx6702_rmw(GX6702_SVPU_BASE + SVPU_CTRL, 0,
			   SVPU_CTRL_SHADOW);
		gx6702_svpu_commit();
	}

	/* Fixed target geometry: 720x480i NTSC or 720x576i PAL. */
	writel(0x000002d0, GX6702_SVPU_BASE + SVPU_REG_054);
	writel((source_w - 1) << 16, GX6702_SVPU_BASE + SVPU_REG_058);
	writel((source_h - 1) << 16,
	       GX6702_SVPU_BASE + SVPU_REG_05C);
	writel(ntsc ? 0x10002000 : 0x10001000,
	       GX6702_SVPU_BASE + SVPU_REG_060);
	writel(ntsc ? 0x01e002d0 : 0x024002d0,
	       GX6702_SVPU_BASE + SVPU_REG_064);
	writel(ntsc ? 0x00000800 : 0,
	       GX6702_SVPU_BASE + SVPU_REG_070);
	writel(ntsc ? 0x00f0001a : 0x00f00019,
	       GX6702_SVPU_BASE + SVPU_REG_074);
	writel(ntsc ? 0x000001df : pal_576i ? 0x0000023f : 0x0000023d,
	       GX6702_SVPU_BASE + SVPU_REG_088);
	writel(0x000002cf, GX6702_SVPU_BASE + SVPU_REG_08C);

	gx6702_rmw(GX6702_SVPU_BASE + SVPU_CTRL, SVPU_CTRL_ROUTE_DIS,
		   SVPU_CTRL_EN);
	if (ntsc) {
		writel(0x02000080, GX6702_SVPU_BASE + SVPU_ROUTE0);
		writel(0x02000000, GX6702_SVPU_BASE + SVPU_ROUTE1);
		writel(0x80108080, GX6702_SVPU_BASE + SVPU_ENABLE);
	} else {
		gx6702_rmw(GX6702_SVPU_BASE + SVPU_ROUTE0,
			   SVPU_ROUTE_FIELD_MASK, SVPU_ROUTE_CVBS);
		gx6702_rmw(GX6702_SVPU_BASE + SVPU_ROUTE1,
			   SVPU_ROUTE_FIELD_MASK, SVPU_ROUTE_CVBS);
		gx6702_rmw(GX6702_SVPU_BASE + SVPU_ENABLE, 0,
			   SVPU_ENABLE_EN);
	}
	writel(SVPU_OUTPUT_ACTIVE, GX6702_SVPU_BASE + SVPU_OUTPUT);
	gx6702_svpu_commit();
}

static void gx6702_fill_black(struct gx6702_video_priv *priv, u32 ysize)
{
	u32 *dst = priv->hw_fb;
	u32 words = priv->hw_line_length * ysize / sizeof(*dst);

	/* Two UYVY pixels of limited-range black. */
	while (words--)
		*dst++ = 0x10801080;
}

static int gx6702_video_sync(struct udevice *dev)
{
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	struct gx6702_video_priv *priv = dev_get_priv(dev);
	const s16 (*csc)[4] = gx6702_csc_bt601;

	int ystart = 0, yend = uc_priv->ysize;
	int y;

	/*
	 * video_idle()'s cyclic can run mid mode-switch / mid glyph while
	 * hotplug logs.  Converting a half-updated damage box leaves
	 * slightly corrupted characters on the UYVY surface.
	 */
	if (priv->fb_busy)
		return 0;

	if (IS_ENABLED(CONFIG_VIDEO_DAMAGE)) {
		ystart = uc_priv->damage.ystart;
		yend = min_t(int, uc_priv->damage.yend, uc_priv->ysize);
		if (ystart >= yend)
			return 0;
	}

	for (y = ystart; y < yend; y++) {
		const u32 *src = uc_priv->fb + y * uc_priv->line_length;
		u8 *dst = priv->hw_fb + y * priv->hw_line_length;
		int x;

		/*
		 * Layer fetches UYVY 4:2:2.  gx3201's planar→layer pack at
		 * 0x93170b98 writes Cb, Y0, Cr, Y1 (Cr in byte 2).  An earlier
		 * sweep that saw byte 2 ignored was under a bad config; matching
		 * the vendor layout restores distinct Cr/Cb.
		 */
		for (x = 0; x < uc_priv->xsize; x += 2) {
			u32 p0 = src[x];
			u32 p1 = src[x + 1];
			int r0 = (p0 >> 16) & 0xff, g0 = (p0 >> 8) & 0xff;
			int b0 = p0 & 0xff;
			int r1 = (p1 >> 16) & 0xff, g1 = (p1 >> 8) & 0xff;
			int b1 = p1 & 0xff;
			int r = (r0 + r1) / 2, g = (g0 + g1) / 2;
			int b = (b0 + b1) / 2;
			int cb = clamp((csc[1][0] * r + csc[1][1] * g +
					csc[1][2] * b) / 256 + csc[1][3], 0, 255);
			int cr = clamp((csc[2][0] * r + csc[2][1] * g +
					csc[2][2] * b) / 256 + csc[2][3], 0, 255);

			*dst++ = cb;
			*dst++ = clamp((csc[0][0] * r0 + csc[0][1] * g0 +
					csc[0][2] * b0) / 256 + csc[0][3], 0, 255);
			*dst++ = cr;
			*dst++ = clamp((csc[0][0] * r1 + csc[0][1] * g1 +
					csc[0][2] * b1) / 256 + csc[0][3], 0, 255);
		}
	}

	/*
	 * The display fetches from DRAM behind the CK610 D-cache, and the
	 * uclass only flushes its own framebuffer, not this one.
	 */
	flush_dcache_range((ulong)priv->hw_fb,
			   (ulong)priv->hw_fb +
			   priv->hw_line_length * uc_priv->ysize);

	return 0;
}

static const struct video_ops gx6702_video_ops = {
	.video_sync = gx6702_video_sync,
};

static unsigned int gx6702_video_dt_mode_index(struct udevice *dev)
{
	u32 index;

	index = dev_read_u32_default(dev, "nationalchip,video-mode",
				     GX6702_MODE_AUTO);
	if (index != GX6702_MODE_AUTO && !gx6702_video_mode_info(index)) {
		log_warning("gx6702-video: mode %u out of range, using auto\n",
			    index);
		return GX6702_MODE_AUTO;
	}

	return index;
}

static bool gx6702_video_dt_cvbs_requested(struct udevice *dev)
{
	return !dev_read_bool(dev, "nationalchip,ypbpr-only");
}

static void gx6702_video_program_analogue(struct gx6702_video_priv *priv,
					  const struct gx6702_video_mode_info *mode)
{
	bool interlaced = mode->timing.flags & DISPLAY_FLAGS_INTERLACED;
	u32 xsize = mode->timing.hactive.typ;
	u32 ysize = gx6702_mode_frame_h(mode);
	u32 dac_enable = priv->cvbs_active ? GFX_DAC_ENABLE_ALL :
		GFX_DAC_ENABLE_YPBPR;

	/*
	 * Stock order in FUN_931715bc / 7165c: program DVE mode, then CRG
	 * display pulse + DVE soft-reset 0x5a, then HDMI.  Re-applying DVE
	 * after the soft-reset left progressive SD (mode 9) rejected while
	 * interlaced still worked.
	 */
	gx6702_analogue_init();
	gx6702_dve_set_mode(mode);
	gx6702_pixclk_set(mode);
	gx6702_display_reset();
	gx6702_pixclk_set(mode);
	gx6702_clock_route1_stock();
	gx6702_pixclk_set(mode);
	gx6702_dac_enable(dac_enable, priv->cvbs_standard,
			  gx6702_mode_index(mode) == GX6702_MODE_576I50);
	gx6702_layer_setup(priv, xsize, ysize, interlaced);
	if (priv->cvbs_active)
		gx6702_svpu_setup(priv, mode);
	else
		gx6702_svpu_disable();
}

static void gx6702_video_program(struct udevice *dev,
				 const struct gx6702_video_mode_info *mode)
{
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	struct gx6702_video_priv *priv = dev_get_priv(dev);
	u32 xsize = mode->timing.hactive.typ;
	u32 ysize = gx6702_mode_frame_h(mode);
	int ret;

	priv->mode = mode;
	priv->mode_index = gx6702_mode_index(mode);
	priv->cvbs_standard = priv->cvbs_requested ? mode->cvbs_standard :
		GX6702_CVBS_NONE;
	priv->cvbs_active = priv->cvbs_standard != GX6702_CVBS_NONE;
	priv->hw_line_length = xsize * GX6702_HW_BYTES_PER_PIXEL;

	uc_priv->xsize = xsize;
	uc_priv->ysize = ysize;
	uc_priv->line_length = xsize * VNBYTES(uc_priv->bpix);
	uc_priv->fb_size = uc_priv->line_length * ysize;

	if (IS_ENABLED(CONFIG_VIDEO_DAMAGE)) {
		uc_priv->damage.xstart = 0;
		uc_priv->damage.ystart = 0;
		uc_priv->damage.xend = xsize;
		uc_priv->damage.yend = ysize;
	}

	gx6702_fill_black(priv, ysize);
	flush_dcache_range((ulong)priv->hw_fb,
			   (ulong)priv->hw_fb + priv->hw_line_length * ysize);

	gx6702_video_program_analogue(priv, mode);

	ret = gx6702_hdmi_enable(mode);
	if (ret) {
		if (priv->cvbs_active)
			log_warning("gx6702-video: %s on YPbPr + %s CVBS; HDMI unavailable\n",
				    mode->name,
				    priv->cvbs_standard == GX6702_CVBS_NTSC ?
				    "NTSC" : "PAL");
		else
			log_warning("gx6702-video: %s on YPbPr; HDMI unavailable\n",
				    mode->name);
	} else if (priv->cvbs_active) {
		log_info("gx6702-video: %s on HDMI/YPbPr + %s CVBS\n",
			 mode->name,
			 priv->cvbs_standard == GX6702_CVBS_NTSC ? "NTSC" : "PAL");
	} else {
		log_info("gx6702-video: %s on HDMI/YPbPr\n", mode->name);
	}
}

static void gx6702_video_console_home(struct udevice *dev)
{
	struct udevice *cons;

	if (uclass_first_device_err(UCLASS_VIDEO_CONSOLE, &cons))
		return;

	/* Keep the splash; just park the cursor so later puts don't overlap. */
	vidconsole_position_cursor(cons, 0, 0);
}

static void gx6702_video_show_logo(struct udevice *dev);

struct gx6702_jpeg_header {
	u16 width;
	u16 height;
};

static u16 gx6702_jpeg_be16(const u8 *p)
{
	return ((u16)p[0] << 8) | p[1];
}

/*
 * Read only enough of the JPEG header to put a hard bound around the DMA
 * output. The engine parses Huffman and quantisation tables itself.
 */
static int gx6702_jpeg_parse_header(const u8 *data, u32 size,
				    struct gx6702_jpeg_header *header)
{
	u32 pos = 2;
	bool have_sof = false;

	if (size < 4 || data[0] != 0xff || data[1] != 0xd8)
		return -EINVAL;

	while (pos + 1 < size) {
		u8 marker;
		u16 length;

		if (data[pos++] != 0xff)
			return -EINVAL;
		while (pos < size && data[pos] == 0xff)
			pos++;
		if (pos >= size)
			return -EINVAL;
		marker = data[pos++];

		if (marker == 0xd9 || marker == 0xda)
			return have_sof ? 0 : -EINVAL;
		if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
			continue;
		if (pos + 2 > size)
			return -EINVAL;

		length = gx6702_jpeg_be16(data + pos);
		if (length < 2 || length > size - pos)
			return -EINVAL;

		if (marker == 0xc0) {
			if (length < 8 || data[pos + 2] != 8)
				return -ENOTSUPP;
			header->height = gx6702_jpeg_be16(data + pos + 3);
			header->width = gx6702_jpeg_be16(data + pos + 5);
			if (!header->width || !header->height ||
			    data[pos + 7] != 3)
				return -ENOTSUPP;
			have_sof = true;
		} else if ((marker >= 0xc1 && marker <= 0xcf) &&
			   marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
			/* Arithmetic/lossless/differential SOFs are not supported. */
			return -ENOTSUPP;
		}

		pos += length;
	}

	return -EINVAL;
}

static int gx6702_jpeg_wait(u32 *status_out)
{
	ulong start = get_timer(0);
	u32 status;

	do {
		status = readl(GX6702_JPEG_BASE + JPEG_STATUS);
		*status_out = status;
		if (status & JPEG_STATUS_DONE) {
			/* All asserted status bits are write-one-to-clear. */
			writel(status, GX6702_JPEG_BASE + JPEG_STATUS);
			*status_out = status;
			return 0;
		}
		if (status & JPEG_STATUS_FRAME_ERROR) {
			writel(status, GX6702_JPEG_BASE + JPEG_STATUS);
			return -ENOSPC;
		}
		if (status & JPEG_STATUS_BS_ERROR) {
			writel(status, GX6702_JPEG_BASE + JPEG_STATUS);
			return -ENODATA;
		}
		if (status & JPEG_STATUS_DECODE_ERROR) {
			writel(status, GX6702_JPEG_BASE + JPEG_STATUS);
			return -EBADMSG;
		}
		if (status & JPEG_STATUS_NEED_DATA) {
			/*
			 * GxLoader clears bit 9, publishes the bytes resident in its
			 * fixed input window at +0x10, then resumes polling.  +0x14 is
			 * the decoder's consumed-byte counter.
			 */
			writel(status, GX6702_JPEG_BASE + JPEG_STATUS);
			writel(GX6702_JPEG_BS_SIZE & GENMASK(27, 0),
			       GX6702_JPEG_BASE + JPEG_BS_AVAILABLE);
			continue;
		}
		udelay(50);
	} while (get_timer(start) < GX6702_JPEG_TIMEOUT_MS);

	return -ETIMEDOUT;
}

/*
 * libgxav's GX3211 JPEG stop path resets module 26 after every transaction:
 * cold reset (CRG +0x04/+0x08 bit 2), then hot-reset set/clear
 * (CRG +0x1d0 bit 2).  The GxLoader splash routine is only used once and
 * omits this teardown, but without it the decoder retains its bitstream
 * parser position and a second decode starts part-way through the old input.
 */
static void gx6702_jpeg_reset(void)
{
	gx6702_rmw(GX6702_CRG_BASE + CRG_COLD_RESET_SET, 0,
		   CRG_COLD_RESET_JPEG);
	udelay(1000);
	gx6702_rmw(GX6702_CRG_BASE + CRG_COLD_RESET_CLEAR, 0,
		   CRG_COLD_RESET_JPEG);
	udelay(1000);

	gx6702_rmw(GX6702_CRG_BASE + CRG_HOT_RESET, 0,
		   CRG_HOT_RESET_JPEG);
	gx6702_rmw(GX6702_CRG_BASE + CRG_HOT_RESET,
		   CRG_HOT_RESET_JPEG, 0);
	sync();
}

static int gx6702_jpeg_decode(struct gx6702_video_priv *priv, const u8 *data,
			      u32 size, u32 *width, u32 *height, u32 *format)
{
	ulong plane_end = (ulong)priv->jpeg_plane[2] +
			  GX6702_JPEG_PLANE_SIZE;
	u32 ctrl, decoded, source, status;
	int ret;

	memset(priv->jpeg_bs, 0, GX6702_JPEG_BS_SIZE);
	memcpy(priv->jpeg_bs, data, size);

	/* Recreate the vendor driver's post-stop idle state for every decode. */
	gx6702_jpeg_reset();

	/* Stop the engine and acknowledge any complete W1C status word. */
	ctrl = readl(GX6702_JPEG_BASE + JPEG_CTRL);
	ctrl &= ~(JPEG_CTRL_MODE_MASK | JPEG_CTRL_START);
	writel(ctrl, GX6702_JPEG_BASE + JPEG_CTRL);
	sync();
	status = readl(GX6702_JPEG_BASE + JPEG_STATUS);
	if (status)
		writel(status, GX6702_JPEG_BASE + JPEG_STATUS);
	sync();

	/* Unlike the display/SVPU, the JPEG DMA consumes the 0x9... DDR alias. */
	writel((ulong)priv->jpeg_bs,
	       GX6702_JPEG_BASE + JPEG_BS_ADDR);
	writel(GX6702_JPEG_BS_SIZE, GX6702_JPEG_BASE + JPEG_BS_SIZE);
	writel(0, GX6702_JPEG_BASE + JPEG_BS_POS);
	writel(0, GX6702_JPEG_BASE + JPEG_BS_AVAILABLE);
	writel((ulong)priv->jpeg_plane[0],
	       GX6702_JPEG_BASE + JPEG_Y_ADDR);
	writel((ulong)priv->jpeg_plane[1],
	       GX6702_JPEG_BASE + JPEG_CB_ADDR);
	writel((ulong)priv->jpeg_plane[2],
	       GX6702_JPEG_BASE + JPEG_CR_ADDR);
	writel(GX6702_JPEG_STRIDE, GX6702_JPEG_BASE + JPEG_FRAME_STRIDE);
	writel((GX6702_JPEG_LINES << 16) | GX6702_JPEG_STRIDE,
	       GX6702_JPEG_BASE + JPEG_FRAME_BOUND);

	writel(ctrl | JPEG_CTRL_DECODE, GX6702_JPEG_BASE + JPEG_CTRL);
	flush_dcache_range((ulong)priv->jpeg_bs,
			   (ulong)priv->jpeg_bs + GX6702_JPEG_BS_SIZE);
	flush_dcache_range((ulong)priv->jpeg_plane[0], plane_end);
	writel(ctrl | JPEG_CTRL_DECODE | JPEG_CTRL_START,
	       GX6702_JPEG_BASE + JPEG_CTRL);

	ret = gx6702_jpeg_wait(&status);
	ctrl = readl(GX6702_JPEG_BASE + JPEG_CTRL);
	writel(ctrl & ~JPEG_CTRL_START, GX6702_JPEG_BASE + JPEG_CTRL);
	if (ret) {
		log_err("gx6702-jpeg: engine error status=%08x available=%08x consumed=%08x\n",
			status,
			readl(GX6702_JPEG_BASE + JPEG_BS_AVAILABLE),
			readl(GX6702_JPEG_BASE + JPEG_BS_CONSUMED));
		return ret;
	}

	invalidate_dcache_range((ulong)priv->jpeg_plane[0], plane_end);
	source = readl(GX6702_JPEG_BASE + JPEG_SOURCE_SIZE);
	decoded = readl(GX6702_JPEG_BASE + JPEG_DECODED_SIZE);
	*width = decoded & 0xfffc;
	*height = decoded >> 16;
	*format = readl(GX6702_JPEG_BASE + JPEG_FORMAT) & JPEG_FORMAT_MASK;

	if (!*width || !*height || *width > GX6702_FB_MAX_W ||
	    *height > GX6702_JPEG_LINES) {
		log_err("gx6702-jpeg: invalid result src=%ux%u out=%ux%u fmt=%u status=%08x\n",
			source & 0xffff, source >> 16, *width, *height,
			*format, status);
		return -EIO;
	}
	if (*format != JPEG_FORMAT_444) {
		log_err("gx6702-jpeg: sampling format %u is outside the U-Boot splash scope\n",
			*format);
		return -ENOTSUPP;
	}

	return 0;
}

static u32 gx6702_jpeg_ycbcr_to_rgb(u8 y, u8 cb, u8 cr)
{
	int c = (int)y - 16;
	int d = (int)cb - 128;
	int e = (int)cr - 128;
	int r, g, b;

	c = max(c, 0);
	r = clamp((298 * c + 409 * e + 128) >> 8, 0, 255);
	g = clamp((298 * c - 100 * d - 208 * e + 128) >> 8, 0, 255);
	b = clamp((298 * c + 516 * d + 128) >> 8, 0, 255);

	return (r << 16) | (g << 8) | b;
}

static void gx6702_jpeg_sample(struct gx6702_video_priv *priv, u32 x, u32 y,
			       u8 *luma, u8 *cb, u8 *cr)
{
	const u8 *yp = priv->jpeg_plane[0];
	const u8 *cbp = priv->jpeg_plane[1];
	const u8 *crp = priv->jpeg_plane[2];

	*luma = yp[y * GX6702_JPEG_STRIDE + x];
	*cb = cbp[y * GX6702_JPEG_STRIDE + x];
	*cr = crp[y * GX6702_JPEG_STRIDE + x];
}

static void gx6702_jpeg_to_shadow(struct udevice *dev,
				  struct gx6702_video_priv *priv,
				  u32 width, u32 height,
				  enum gx6702_jpeg_scale scale)
{
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	u32 src_x = 0, src_y = 0, src_w = width, src_h = height;
	u32 dst_x = 0, dst_y = 0;
	u32 dst_w = uc_priv->xsize, dst_h = uc_priv->ysize;
	u32 *fb = uc_priv->fb;
	u32 x, y;

	if (scale == GX6702_JPEG_NATIVE) {
		dst_w = min_t(u32, width, uc_priv->xsize);
		dst_h = min_t(u32, height, uc_priv->ysize);
		src_w = dst_w;
		src_h = dst_h;
		src_x = (width - src_w) / 2;
		src_y = (height - src_h) / 2;
		dst_x = (uc_priv->xsize - dst_w) / 2;
		dst_y = (uc_priv->ysize - dst_h) / 2;
	} else if (scale == GX6702_JPEG_FIT) {
		if (width * uc_priv->ysize > uc_priv->xsize * height) {
			dst_h = max_t(u32, 1, height * uc_priv->xsize / width);
			dst_y = (uc_priv->ysize - dst_h) / 2;
		} else {
			dst_w = max_t(u32, 1, width * uc_priv->ysize / height);
		}
	}
	/* Keep both sides of the image on UYVY chroma-pair boundaries. */
	dst_w = max_t(u32, 2, dst_w & ~1U);
	dst_x = ((uc_priv->xsize - dst_w) / 2) & ~1U;

	/*
	 * The decoder planes temporarily occupy the RGB shadow, so first pack
	 * them into the independent UYVY scanout surface. Once the planes are no
	 * longer needed, reconstruct the RGB shadow from that surface. This avoids
	 * another 6.3 MiB permanent reservation on a 64 MiB board while keeping
	 * later console damage updates coherent with the displayed JPEG.
	 */
	for (y = 0; y < uc_priv->ysize; y++) {
		u8 *dst = priv->hw_fb + y * priv->hw_line_length;

		for (x = 0; x < uc_priv->xsize; x += 2) {
			u8 luma0 = 16, luma1 = 16;
			u8 cb0 = 128, cb1 = 128, cr0 = 128, cr1 = 128;

			if (y >= dst_y && y < dst_y + dst_h) {
				u32 sy = src_y + (y - dst_y) * src_h / dst_h;

				if (x >= dst_x && x < dst_x + dst_w)
					gx6702_jpeg_sample(priv,
							   src_x + (x - dst_x) *
							   src_w / dst_w, sy,
							   &luma0, &cb0, &cr0);
				if (x + 1 >= dst_x && x + 1 < dst_x + dst_w)
					gx6702_jpeg_sample(priv,
							   src_x + (x + 1 - dst_x) *
							   src_w / dst_w,
							   sy, &luma1, &cb1, &cr1);
			}

			*dst++ = ((u16)cb0 + cb1) >> 1;
			*dst++ = luma0;
			*dst++ = ((u16)cr0 + cr1) >> 1;
			*dst++ = luma1;
		}
	}
	flush_dcache_range((ulong)priv->hw_fb,
			   (ulong)priv->hw_fb +
			   priv->hw_line_length * uc_priv->ysize);

	for (y = 0; y < uc_priv->ysize; y++) {
		const u8 *src = priv->hw_fb + y * priv->hw_line_length;
		u32 *dst = (u32 *)((u8 *)fb + y * uc_priv->line_length);

		for (x = 0; x < uc_priv->xsize; x += 2) {
			u8 cb = *src++;
			u8 luma0 = *src++;
			u8 cr = *src++;
			u8 luma1 = *src++;

			dst[x] = gx6702_jpeg_ycbcr_to_rgb(luma0, cb, cr);
			dst[x + 1] = gx6702_jpeg_ycbcr_to_rgb(luma1, cb, cr);
		}
	}
	video_damage(dev, 0, 0, uc_priv->xsize, uc_priv->ysize);
}

int gx6702_video_show_jpeg(ulong addr, u32 size,
			   enum gx6702_jpeg_scale scale)
{
	struct gx6702_jpeg_header header = { 0 };
	struct gx6702_video_priv *priv;
	struct video_priv *uc_priv;
	struct udevice *dev;
	const u8 *data;
	u32 width, height, format;
	int ret;

	if (!size || size > GX6702_JPEG_BS_SIZE)
		return -EFBIG;
	if (scale > GX6702_JPEG_STRETCH)
		return -EINVAL;
	ret = uclass_first_device_err(UCLASS_VIDEO, &dev);
	if (ret)
		return ret;

	data = map_sysmem(addr, size);
	ret = gx6702_jpeg_parse_header(data, size, &header);
	if (ret)
		goto out_unmap;
	if (header.width > GX6702_FB_MAX_W ||
	    header.height > GX6702_FB_MAX_H) {
		ret = -E2BIG;
		goto out_unmap;
	}

	priv = dev_get_priv(dev);
	uc_priv = dev_get_uclass_priv(dev);
	/* video-uclass publishes the RGB framebuffer after driver probe. */
	priv->jpeg_plane[0] = uc_priv->fb;
	priv->jpeg_plane[1] = (u8 *)priv->jpeg_plane[0] +
			      GX6702_JPEG_PLANE_SIZE;
	priv->jpeg_plane[2] = (u8 *)priv->jpeg_plane[1] +
			      GX6702_JPEG_PLANE_SIZE;
	priv->fb_busy = true;
	ret = gx6702_jpeg_decode(priv, data, size, &width, &height, &format);
	if (!ret)
		gx6702_jpeg_to_shadow(dev, priv, width, height, scale);
	priv->fb_busy = false;
	if (ret) {
		/* Decoder errors may leave partial planes in the RGB shadow. */
		gx6702_video_show_logo(dev);
		goto out_unmap;
	}

	video_sync(dev, true);
	gx6702_video_console_home(dev);
	log_info("gx6702-jpeg: HW decoded %ux%u baseline JPEG -> %ux%u format %u\n",
		 header.width, header.height,
		 width, height, format);

out_unmap:
	unmap_sysmem(data);
	return ret;
}

static void gx6702_video_show_logo(struct udevice *dev)
{
	if (!IS_ENABLED(CONFIG_VIDEO_LOGO))
		return;

	video_clear(dev);
	video_bmp_display(dev, map_to_sysmem(video_get_u_boot_logo()),
			  BMP_ALIGN_CENTER, BMP_ALIGN_CENTER, true);
	/* force: bypass VIDEO_SYNC_MS throttle and clear damage */
	video_sync(dev, true);
	gx6702_video_console_home(dev);
}

static int gx6702_video_bind(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);

	/*
	 * Always reserve a 1920x1080 RGB shadow + UYVY surface, GxLoader's
	 * 812 KiB SVPU working area, and the JPEG engine's 2 MiB input buffer so
	 * runtime mode switches and decoding never depend on the 1 MiB heap.
	 * During decode the idle RGB shadow is reused for the three output planes.
	 */
	plat->size = gx6702_max_shadow_size() +
		     gx6702_max_hw_size() + GX6702_SVPU_MEM_SIZE +
		     GX6702_JPEG_MEM_SIZE;
	plat->align = SZ_1M;

	return 0;
}

static void gx6702_video_hpd_poll(struct cyclic_info *c)
{
	struct gx6702_video_priv *priv =
		container_of(c, struct gx6702_video_priv, cyclic);
	const struct gx6702_video_mode_info *mode_info;
	bool plugged;
	unsigned int mode;

	if (priv->fb_busy)
		return;

	plugged = gx6702_hdmi_hpd_connected();
	if (plugged == priv->hpd_connected) {
		priv->hpd_debounce = 0;
		return;
	}

	priv->hpd_debounce++;
	if (priv->hpd_debounce < GX6702_HPD_DEBOUNCE)
		return;

	priv->hpd_debounce = 0;
	priv->hpd_connected = plugged;
	gx6702_hdmi_invalidate_edid();

	if (!plugged) {
		if (priv->auto_mode && priv->cvbs_requested &&
		    priv->mode &&
		    priv->mode->cvbs_standard == GX6702_CVBS_NTSC) {
			mode_info = gx6702_video_mode_info(GX6702_MODE_1080I50);
			log_info("gx6702-video: HDMI unplug; auto -> %s for PAL CVBS\n",
				 mode_info->name);
			priv->fb_busy = true;
			gx6702_video_program(priv->dev, mode_info);
			priv->fb_busy = false;
			gx6702_video_show_logo(priv->dev);
		} else {
			log_info("gx6702-video: HDMI unplug (analogue keeps %s)\n",
				 priv->mode ? priv->mode->name : "?");
		}
		return;
	}

	log_info("gx6702-video: HDMI plug\n");
	if (!priv->auto_mode)
		return;

	/*
	 * Hold off video_idle sync for the whole apply: logging + mode
	 * reprogram otherwise races mid-glyph and leaves fringed characters.
	 */
	priv->fb_busy = true;
	mode = gx6702_video_pick_auto_mode_policy(priv->cvbs_requested);
	mode_info = gx6702_video_mode_info(mode);
	log_info("gx6702-video: hotplug auto -> %s\n", mode_info->name);

	if (mode == priv->mode_index && priv->mode) {
		/* Same timing: restore TMDS only, keep the framebuffer. */
		gx6702_hdmi_enable(priv->mode);
		priv->fb_busy = false;
		video_sync(priv->dev, true);
		return;
	}

	gx6702_video_program(priv->dev, mode_info);
	priv->fb_busy = false;
	gx6702_video_show_logo(priv->dev);
}

static int gx6702_video_probe(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	struct gx6702_video_priv *priv = dev_get_priv(dev);
	unsigned int want = gx6702_video_dt_mode_index(dev);
	const struct gx6702_video_mode_info *mode;
	unsigned int concrete;

	priv->dev = dev;
	priv->cvbs_requested = gx6702_video_dt_cvbs_requested(dev);
	priv->hw_fb = map_sysmem(plat->base + gx6702_max_shadow_size(),
				 gx6702_max_hw_size());
	priv->svpu_fb = map_sysmem(plat->base + gx6702_max_shadow_size() +
				   gx6702_max_hw_size(), GX6702_SVPU_MEM_SIZE);
	priv->jpeg_bs =
		map_sysmem(plat->base + gx6702_jpeg_mem_offset(),
			   GX6702_JPEG_BS_SIZE);

	uc_priv->bpix = VIDEO_BPP32;
	uc_priv->format = VIDEO_X8R8G8B8;
	uc_priv->flush_dcache = true;

	priv->auto_mode = (want == GX6702_MODE_AUTO);
	if (priv->auto_mode) {
		concrete =
			gx6702_video_pick_auto_mode_policy(priv->cvbs_requested);
		log_info("gx6702-video: boot auto -> %s\n",
			 gx6702_video_mode_info(concrete)->name);
	} else {
		concrete = want;
	}

	mode = gx6702_video_mode_info(concrete);
	gx6702_video_program(dev, mode);

	priv->hpd_connected = gx6702_hdmi_hpd_connected();
	priv->hpd_debounce = 0;
	cyclic_register(&priv->cyclic, gx6702_video_hpd_poll,
			GX6702_HPD_POLL_US, "gx6702-hdmi-hpd");

	return 0;
}

int gx6702_video_set_mode(unsigned int index, bool force)
{
	struct udevice *dev;
	struct gx6702_video_priv *priv;
	const struct gx6702_video_mode_info *mode;
	bool edid_ok = false;
	bool want_auto = (index == GX6702_MODE_AUTO);
	unsigned int concrete = index;
	int ret;

	ret = uclass_first_device_err(UCLASS_VIDEO, &dev);
	if (ret)
		return ret;

	priv = dev_get_priv(dev);
	if (want_auto) {
		concrete =
			gx6702_video_pick_auto_mode_policy(priv->cvbs_requested);
		force = true; /* Auto already filtered by EDID */
	}

	mode = gx6702_video_mode_info(concrete);
	if (!mode)
		return -EINVAL;

	if (!force && mode->vic &&
	    !gx6702_hdmi_sink_supports_vic(mode->vic, &edid_ok) &&
	    edid_ok) {
		log_warning("gx6702-video: sink EDID rejects VIC %u (%s); "
			    "use 'gxvideo mode %u force'\n",
			    mode->vic, mode->name, index);
		return -ENOTSUPP;
	}

	priv->auto_mode = want_auto;
	priv->fb_busy = true;
	gx6702_video_program(dev, mode);
	priv->fb_busy = false;
	gx6702_video_show_logo(dev);
	priv->hpd_connected = gx6702_hdmi_hpd_connected();

	return 0;
}

static const struct udevice_id gx6702_video_ids[] = {
	{ .compatible = "nationalchip,gx6702-video" },
	{ }
};

U_BOOT_DRIVER(gx6702_video) = {
	.name		= "gx6702_video",
	.id		= UCLASS_VIDEO,
	.of_match	= gx6702_video_ids,
	.ops		= &gx6702_video_ops,
	.bind		= gx6702_video_bind,
	.probe		= gx6702_video_probe,
	.priv_auto	= sizeof(struct gx6702_video_priv),
};
