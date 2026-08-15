/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * NationalChip GX6702 video output register map.
 *
 * Recovered from the 64 KiB vendor GxLoader (BOOT.bin, resident at
 * 0x931693E0).  Addresses quoted in the comments are in that image, so they
 * can be checked against
 *
 *   csky-linux-objdump -D -b binary -m csky:ck610 -EL \
 *       --adjust-vma=0x931693E0 BOOT.bin
 *
 * The bootloader calls its own entry points gx3201_*, which is the vendor's
 * name for this display macro-cell; the names here follow that.
 */

#ifndef __GX6702_VIDEO_H
#define __GX6702_VIDEO_H

/*
 * Clock and reset generator.  Same block the IPL uses for the DDR PHY, so it
 * is already mapped uncached by the stage-1 MMU.
 */
#define GX6702_CRG_BASE			0xA030A000
#define CRG_RESET0			0x000
#define  CRG_RESET0_DVE			BIT(7)
#define CRG_COLD_RESET_SET		0x004
#define CRG_COLD_RESET_CLEAR		0x008
#define  CRG_COLD_RESET_JPEG		BIT(2)
#define CRG_RESET3			0x00C
#define  CRG_RESET3_DISPLAY		BIT(28)
/*
 * Pixel-clock source select / gate registers touched by the per-mode
 * paths at 0x93171602-0x93171730 after FUN_93171174 programs GFX_PIXCLK.
 */
#define CRG_PIXCLK_SEL			0x024
#define  CRG_PIXCLK_SEL_DIV_MASK	0x3F
#define  CRG_PIXCLK_SEL_HD		0x07	/* 720p / 1080i / 1080p		*/
#define  CRG_PIXCLK_SEL_SD_INTERLACED	0x2B
#define  CRG_PIXCLK_SEL_SD_PROGRESSIVE	0x15
/* Additional source bits selected by the live eCos SD and HD paths. */
#define  CRG_PIXCLK_SD_PROG_EXTRA_MASK	0x00061000
#define  CRG_PIXCLK_SD_EXTRA_VAL		0x00021000
#define  CRG_PIXCLK_NONPROG_EXTRA_VAL	0x00040000
#define  CRG_PIXCLK_SEL_BIT6		BIT(6)
#define  CRG_PIXCLK_SEL_BIT7		BIT(7)
#define CRG_CLK_GATE0			0x170
#define  CRG_CLK_GATE0_BIT3		BIT(3)
#define CRG_CLK_GATE1			0x17C
#define  CRG_CLK_GATE1_BIT3		BIT(3)
#define  CRG_CLK_GATE1_BIT4		BIT(4)
#define  CRG_CLK_GATE1_BIT5		BIT(5)
#define  CRG_CLK_GATE1_BIT6		BIT(6)
#define CRG_HOT_RESET			0x1D0
#define  CRG_HOT_RESET_JPEG		BIT(2)
/* Analogue/DVE clock setup from GxLoader FUN_93170350. */
#define CRG_DVE_CLK_CFG			0x1F0
#define  CRG_DVE_CLK_CFG_CLEAR		(BIT(0) | 0x00000F70)
#define  CRG_DVE_CLK_CFG_ENABLE	BIT(1)

/*
 * Graphics block: the OSD layers that carry the boot logo, and the analogue
 * video DACs.  Layer 0 is at 0x40, layer 1 mirrors it at 0x98.
 */
#define GX6702_GFX_BASE			0xA4800000
#define GFX_L0_CTRL			0x040
#define  GFX_L0_CTRL_EN			BIT(0)
/*
 * Setting this doubles the bytes the layer consumes per line (32-bit pixel).
 * Stock logo path stays on 16-bit UYVY (Cb Y0 Cr Y1); see FUN_93170b98.
 */
#define  GFX_L0_CTRL_PIX32		BIT(2)
#define  GFX_L0_CTRL_VSCALE_EN		BIT(3)
/*
 * Not an alpha register: the vendor loads half the vertical scale factor
 * here and leaves it at zero whenever the scaler is bypassed.
 */
#define GFX_L0_VPHASE			0x044	/* (vscale / 2) << 16		*/
#define GFX_L0_DST_POS			0x048	/* (y << 16) | x		*/
#define GFX_L0_SRC_SIZE			0x04C	/* (h << 16) | w		*/
#define  GFX_L0_SRC_SIZE_H_SHIFT	16
/*
 * Destination height is in the same line units as SRC.  Vendor FUN_93171c80
 * uses vscale = (src_h << 12) / dst_h, so unity is src_h == dst_h with
 * SCALE = 0x1000.  Width stays in whole pixels.
 */
#define GFX_L0_DST_SIZE			0x050	/* (h << 16) | w		*/
#define  GFX_L0_DST_SIZE_H_SHIFT	16
#define GFX_L0_SCALE			0x054	/* (v << 16) | h, 12.12		*/
#define  GFX_SCALE_UNITY		0x1000
#define GFX_L0_STRIDE			0x058
#define  GFX_L0_STRIDE_MASK		0x1FFF
/*
 * Bits [26:16] are cleared then (width >> 5) is written at bit 19
 * (FUN_93171c80: asri 5 / lsli 0x13).  Bits [13:0] hold the byte pitch.
 * Bit 29 bypasses the horizontal scaler; the vendor still programs both.
 */
#define  GFX_L0_STRIDE_PITCH_CLEAR	0x07FF0000
#define  GFX_L0_STRIDE_PITCH_SHIFT	19
#define  GFX_L0_STRIDE_UNSCALED		BIT(29)
#define  GFX_L0_STRIDE_EN		BIT(31)
/*
 * Horizontal and vertical filter control.  Bit 8 picks the interpolating
 * taps, which the vendor only selects when the matching scaler is engaged.
 */
#define GFX_L0_HFILTER			0x060
#define GFX_L0_VFILTER			0x064
#define  GFX_L0_FILTER_BYPASS		0x00FF0000
#define GFX_L0_FB_TOP			0x068	/* bus address of field 0	*/
#define GFX_L0_FB_BOTTOM		0x06C	/* bus address of field 1	*/
/*
 * Pixel-clock divider written by gx3201_videoout_init() via FUN_93171174.
 * Bits [23:16] plus CRG+0x24 sel (vendor 0x931715bc / 7165c / 716ba):
 *   0xC0 + sel 0x2B  27.00 MHz SD interlaced (vendor BOOT)
 *   0x00 + sel 0x15  27.00 MHz SD progressive (eCos live dumps)
 *   0x30 + sel 0x07  74.25 MHz (720p, 1080i)
 *   0x18 + sel 0x07 148.50 MHz (1080p) — GATE1 bits 3..5 cleared
 * Jumptable slot 0x60/sel 0x15 (FUN_93171602) is a different path; using it
 * for 1080p blanks the sink.
 */
#define GFX_PIXCLK			0x0F4
#define  GFX_PIXCLK_DIV_MASK		0x00FF0000
#define  GFX_PIXCLK_DIV_SHIFT		16
#define  GFX_PIXCLK_DIV_27M_PROGRESSIVE	0x00
#define  GFX_PIXCLK_DIV_27M		0xC0
#define  GFX_PIXCLK_DIV_74M		0x30
#define  GFX_PIXCLK_DIV_148M		0x18

/*
 * Video DACs.  GAIN packs four 6-bit trims that the vendor reads from eFuse
 * words 0x126..0x129 (table at 0x93176774) and falls back to the defaults at
 * 0x93176784 when the eFuse is blank.
 */
#define GFX_DAC_GAIN			0x130
#define GFX_DAC_ENABLE			0x134
#define  GFX_DAC_ENABLE_YPBPR		0x18	/* video_dac_mode 1		*/
#define  GFX_DAC_ENABLE_ALL		0x1F	/* video_dac_mode 0 or 2	*/
#define GFX_DAC_CFG			0x138

#define GX6702_DAC_GAIN_DEFAULT		0x1E
#define GX6702_DAC_GAIN_DEFAULT_3	0x20

/*
 * Hardware JPEG decoder used by GxLoader's gx3211_svpu_jpeg_decode() at
 * 0x93170c70. Despite the old function name, this is a separate DMA engine;
 * it produces planar Y/Cb/Cr and does not share the CVBS SVPU registers.
 */
#define GX6702_JPEG_BASE		0xA4400000
#define JPEG_CTRL			0x000
#define  JPEG_CTRL_START		BIT(0)
#define  JPEG_CTRL_MODE_MASK		GENMASK(9, 8)
#define  JPEG_CTRL_DECODE		BIT(8)
#define JPEG_BS_ADDR			0x004
#define JPEG_BS_SIZE			0x008
#define JPEG_BS_POS			0x00C
#define JPEG_BS_AVAILABLE		0x010
#define JPEG_BS_CONSUMED		0x014
#define JPEG_Y_ADDR			0x018
#define JPEG_CB_ADDR			0x01C
#define JPEG_CR_ADDR			0x020
#define JPEG_FRAME_STRIDE		0x024
#define JPEG_FRAME_BOUND		0x028
#define JPEG_FORMAT			0x03C
#define  JPEG_FORMAT_MASK		GENMASK(2, 0)
#define  JPEG_FORMAT_444		3
#define JPEG_SOURCE_SIZE		0x044
#define JPEG_DECODED_SIZE		0x048
#define JPEG_STATUS			0x054
#define  JPEG_STATUS_DONE		BIT(0)
#define  JPEG_STATUS_FRAME_ERROR	BIT(3)
#define  JPEG_STATUS_BS_ERROR		BIT(5)
#define  JPEG_STATUS_DECODE_ERROR	BIT(6)
#define  JPEG_STATUS_NEED_DATA		BIT(9)

/*
 * Primary digital video encoder.  This feeds HDMI and YPbPr.  Composite is
 * generated independently by the PAL DVE behind the secondary VPU below.
 * Programmed by gx3201_videoout_yuv_config() at 0x9317121C.
 */
#define GX6702_DVE_BASE			0xA4804000
#define DVE_MODE			0x000
#define  DVE_MODE_CODE_MASK		0x7F
#define  DVE_MODE_SYNC_MASK		0x000E0000	/* [19:17]		*/
#define  DVE_MODE_SYNC_VAL		0x00060000	/* bits 17 and 18 set	*/
#define  DVE_MODE_TRIM_MASK		0x3E000000	/* [29:25]		*/
#define  DVE_MODE_TRIM_HD		(6 << 25)	/* golden cc060026 / hd2015	*/
#define  DVE_MODE_TRIM_SD_INTERLACED	BIT(25)	/* secondary PAL DVE c3... */
#define  DVE_MODE_TRIM_SD		(2 << 25)	/* GxLoader 576i/p c4060000	*/
#define  DVE_MODE_TRIM_STOCK		DVE_MODE_TRIM_HD /* legacy alias		*/
#define  DVE_MODE_EN			BIT(31)
#define DVE_SOFT_RESET			0x004
#define  DVE_SOFT_RESET_MAGIC		0x5A
#define DVE_TIMING_SEL			0x070
#define  DVE_TIMING_SEL_HD		(BIT(3) | BIT(4))
#define DVE_LINE_LEN			0x098
#define DVE_REG_0DC			0x0DC
#define  DVE_REG_0DC_BIT12		BIT(12)	/* clear → stock 00000557	*/
#define DVE_REG_164			0x164
#define  DVE_REG_164_LO_MASK		0x000000FF /* clear → stock 00f13c00	*/
#define DVE_REG_1B0			0x1B0
#define DVE_REG_1B8			0x1B8
#define  DVE_REG_1B0_STOCK		0x00163863 /* hd2015 + golden		*/

/*
 * Secondary VPU (SVPU).  GxLoader uses it to downscale the primary display
 * into a fixed PAL raster, then sends that raster to the DVE at +0x4000.
 * Three identical six-pointer banks occupy +0x00..+0x44.  Writing 1 then 0
 * to COMMIT latches its shadowed registers.
 */
#define GX6702_SVPU_BASE		0xA4900000
#define GX6702_VP_BASE			GX6702_SVPU_BASE /* legacy dump name */
#define SVPU_PTR_BANK_STRIDE		0x018
#define SVPU_PTR_BANKS			3
#define SVPU_CTRL			0x050
#define  SVPU_CTRL_EN			BIT(0)
#define  SVPU_CTRL_ROUTE_DIS		BIT(1)
#define  SVPU_CTRL_STD_MASK		(BIT(12) | BIT(9) | BIT(8))
#define  SVPU_CTRL_STD_PAL		(BIT(12) | BIT(8))
#define  SVPU_CTRL_STD_NTSC		(BIT(9) | BIT(8))
#define  SVPU_CTRL_SOURCE_MASK		0x1F000000
#define  SVPU_CTRL_SOURCE_SHIFT	24
#define  SVPU_CTRL_SHADOW		BIT(31)
#define SVPU_REG_054			0x054
#define SVPU_REG_058			0x058
#define SVPU_REG_05C			0x05C
#define SVPU_REG_060			0x060
#define SVPU_REG_064			0x064
#define SVPU_ROUTE0			0x068
#define SVPU_REG_070			0x070
#define SVPU_REG_074			0x074
#define SVPU_COMMIT			0x078
#define SVPU_ROUTE1			0x080
#define  SVPU_ROUTE_FIELD_MASK		0x07FF0000
#define  SVPU_ROUTE_CVBS		BIT(25)
#define SVPU_ENABLE			0x084
#define  SVPU_ENABLE_EN			BIT(31)
#define SVPU_REG_088			0x088
#define SVPU_REG_08C			0x08C
#define SVPU_OUTPUT			0x090
#define  SVPU_OUTPUT_ACTIVE		(BIT(1) | BIT(0))

#define GX6702_CVBS_DVE_BASE		(GX6702_SVPU_BASE + 0x4000)
#define CVBS_DVE_STD_CTRL		0x00C
#define CVBS_DVE_ROUTE			0x02C
#define  CVBS_DVE_ROUTE_MASK		0xFFF00000
#define  CVBS_DVE_ROUTE_CVBS		0xE0100000
#define CVBS_DVE_TIMING			0x070
#define CVBS_DVE_OUTPUT			0x090
#define CVBS_DVE_SHADOW0		0x110
#define CVBS_DVE_SHADOW1		0x130
#define CVBS_DVE_SHADOW2		0x150
#define  CVBS_DVE_SHADOW_EN		0x10000001
#define CVBS_DVE_REG_16C		0x16C
#define CVBS_DVE_REG_1A8		0x1A8

/* Synopsys DesignWare HDMI TX, byte-wide registers at base + (reg << 2). */
#define GX6702_HDMI_BASE		0xA4F00000

/*
 * DDR is mapped at 0x90000000 for the CPU but the display DMA masters see the
 * raw 0x10000000 window, which is what gx3201_videoout_init() writes when it
 * programs the plane pointers with "framebuffer - 0x80000000" at 0x93171890.
 */
#define GX6702_DDR_CPU_TO_BUS		0x80000000

/**
 * enum gx6702_video_mode - eCos UI video>mode index (0=Auto, 1..10)
 *
 * Matches the eCos settings path (config.ini / VIDEO_MODE_VA), not the
 * older GxLoader hdmi_mode 1..8 numbering.  Register values come from
 * gxtest/goldens/dumps/ via gx6702_ecos_goldens.c.  Mode 0 picks the
 * best CEA VIC the sink advertises (native SVD, else preference order).
 */
enum gx6702_video_mode {
	GX6702_MODE_AUTO = 0,
	GX6702_MODE_480I60 = 1,
	GX6702_MODE_480P60,
	GX6702_MODE_576I50,
	GX6702_MODE_576P50,
	GX6702_MODE_720P50,
	GX6702_MODE_720P60,
	GX6702_MODE_1080I50,
	GX6702_MODE_1080I60,
	GX6702_MODE_1080P50,
	GX6702_MODE_1080P60,
	GX6702_MODE_COUNT
};

/**
 * enum gx6702_cvbs_standard - composite standard generated for a source mode
 */
enum gx6702_cvbs_standard {
	GX6702_CVBS_NONE,
	GX6702_CVBS_PAL,
	GX6702_CVBS_NTSC,
};

/**
 * struct gx6702_video_mode_info - primary timing and CVBS clone metadata
 * @name:	human readable name used in the boot log
 * @vic:	CEA-861 video identification code sent in the AVI InfoFrame
 * @dve_code:	value for the low bits of DVE_MODE
 * @dve_hd:	set DVE_TIMING_SEL_HD rather than clearing it
 * @dve_line:	DVE_LINE_LEN, or 0 when the mode does not program it
 * @cvbs_standard: PAL for 50 Hz sources, NTSC for 60 Hz sources
 * @svpu_source: GxLoader SVPU downscale selector for this source raster
 * @timing:	CEA timing driven onto HDMI and YPbPr
 *
 * @vic and @dve_code come from the per-mode routines at 0x9317141C-0x931714E2
 * and the jump table in gx3201_videoout_yuv_config() at 0x93176C90.
 */
struct gx6702_video_mode_info {
	const char *name;
	u8 vic;
	u8 dve_code;
	bool dve_hd;
	u32 dve_line;
	u8 cvbs_standard;
	u8 svpu_source;
	struct display_timing timing;
};

enum gx6702_cvbs_state {
	GX6702_CVBS_DISABLED,
	GX6702_CVBS_ACTIVE_PAL,
	GX6702_CVBS_ACTIVE_NTSC,
};

/** JPEG placement policy for the boot splash. */
enum gx6702_jpeg_scale {
	GX6702_JPEG_NATIVE,	/* centred 1:1 pixels, clipped if necessary */
	GX6702_JPEG_FIT,	/* scale to fit while preserving aspect ratio */
	GX6702_JPEG_STRETCH,	/* fill the raster, changing aspect if needed */
};

const struct gx6702_video_mode_info *gx6702_video_mode_info(unsigned int mode);

/**
 * gx6702_hdmi_enable() - bring the HDMI TX up on an already programmed DVE
 * @mode: mode description returned by gx6702_video_mode_info()
 *
 * Return: 0 on success, or a negative error code.  A disconnected or
 * unreadable sink is not an error: the analogue outputs can still work, so
 * the caller should keep going.
 */
int gx6702_hdmi_enable(const struct gx6702_video_mode_info *mode);

/**
 * gx6702_hdmi_read_edid() - read sink EDID over the DW HDMI internal DDC
 * @buf: caller buffer (at least 256 bytes for block 0 + CEA extension)
 * @buf_size: size of @buf
 *
 * Initializes the TX I2CM if needed, waits for HPD, then reads EDID.
 * Return: bytes read (128 or 256), or a negative errno.
 */
int gx6702_hdmi_read_edid(u8 *buf, int buf_size);

/**
 * gx6702_hdmi_sink_supports_vic() - CEA SVD check against cached/fresh EDID
 * @vic: CEA-861 VIC to look for
 * @edid_ok: optional; set true if a usable EDID was obtained
 *
 * Return: true if the sink advertises @vic, or if EDID is missing/unusable
 * (fail-open so CVBS-only boards still switch modes).  When @edid_ok is
 * true and the return is false, the VIC is definitively unsupported.
 */
bool gx6702_hdmi_sink_supports_vic(u8 vic, bool *edid_ok);

/**
 * gx6702_hdmi_cea_native_vic() - first SVD with the native bit set
 * @edid_ok: optional; set true if a usable EDID was obtained
 *
 * Return: VIC 1..127, or 0 if none / no EDID.
 */
u8 gx6702_hdmi_cea_native_vic(bool *edid_ok);

/** gx6702_hdmi_hpd_connected() - non-blocking PHY HPD sample */
bool gx6702_hdmi_hpd_connected(void);

/** gx6702_hdmi_invalidate_edid() - drop cached EDID (e.g. on unplug) */
void gx6702_hdmi_invalidate_edid(void);

/**
 * gx6702_video_pick_auto_mode() - choose best eCos mode 1..10 for the sink
 *
 * When CVBS is requested, prefers an EDID-supported 50 Hz mode so PAL remains
 * the default. A 60-Hz-only sink selects its best mode and uses NTSC CVBS.
 * Without EDID/HPD, falls back to 1080i50.
 */
unsigned int gx6702_video_pick_auto_mode(void);

/**
 * gx6702_video_set_mode() - switch DVE/HDMI/layer at runtime and redraw splash
 * @index: eCos mode 0=Auto or 1..10 (see enum gx6702_video_mode)
 * @force: skip HDMI EDID VIC check when true (ignored for Auto)
 *
 * Framebuffer is reserved for 1920x1080 so any mode fits.  Return 0 on
 * success, -ENOTSUPP if the sink rejects the VIC (unless @force), or
 * another negative errno.  Mode 0 enables hotplug auto-reselect.
 */
int gx6702_video_set_mode(unsigned int index, bool force);

/** gx6702_video_current_mode() - last programmed eCos index (1..10) */
unsigned int gx6702_video_current_mode(void);

/** gx6702_video_auto_enabled() - true while Auto policy is active */
bool gx6702_video_auto_enabled(void);

/** gx6702_video_cvbs_state() - current PAL/NTSC CVBS output state */
enum gx6702_cvbs_state gx6702_video_cvbs_state(void);

/**
 * gx6702_video_show_jpeg() - decode a JPEG with the GX6702 engine and show it
 * @addr: CPU address of the complete JPEG bitstream
 * @size: number of valid bytes at @addr (maximum 2 MiB)
 * @scale: native, aspect-preserving fit, or full-raster stretch
 *
 * This deliberately narrow boot-splash path accepts baseline, 8-bit,
 * three-component JPEGs up to 1920x1080 whose decoder output is 4:4:4. The
 * selected placement policy maps the image onto the active raster, then keeps
 * it in U-Boot's RGB shadow framebuffer so later console updates do not erase
 * it.
 *
 * Return: 0 on success or a negative errno.
 */
int gx6702_video_show_jpeg(ulong addr, u32 size,
			   enum gx6702_jpeg_scale scale);

#endif /* __GX6702_VIDEO_H */
