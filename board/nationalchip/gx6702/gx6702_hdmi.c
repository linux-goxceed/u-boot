// SPDX-License-Identifier: GPL-2.0+
/*
 * GX6702 glue for the Synopsys DesignWare HDMI transmitter.
 *
 * The vendor loader carries the DWC "software api 2.12" core, so
 * drivers/video/dw_hdmi.c drives the same register set; only the bus access
 * width and the PHY tables are board specific.  The configuration routine
 * this is derived from is at 0x931728D8 in BOOT.bin.
 */

#include <dm.h>
#include <dw_hdmi.h>
#include <edid.h>
#include <errno.h>
#include <fdtdec.h>
#include <log.h>
#include <media_bus_format.h>
#include <time.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/string.h>

#include "gx6702_video.h"
#include "gx6702_ecos_goldens.h"

/*
 * The vendor accessors at 0x93171EB8 and 0x93171E84 address the core as a
 * byte at base + (reg << 2).  dw_hdmi.c only offers reg_io_width 1 (byte at
 * base + reg) and 4 (word at base + (reg << 2)), so neither fits and the
 * driver takes these callbacks instead.
 */
static void gx6702_hdmi_write(struct dw_hdmi *hdmi, u8 val, int offset)
{
	writeb(val, hdmi->ioaddr + ((ulong)offset << 2));
}

static u8 gx6702_hdmi_read(struct dw_hdmi *hdmi, int offset)
{
	return readb(hdmi->ioaddr + ((ulong)offset << 2));
}

/*
 * PHY settings recovered from the three pixel-clock cases at
 * 0x931731E0-0x93173246.  The loader only ever selects 27.00, 74.25 or
 * 148.50 MHz, which covers every mode the DVE can generate, so the ~0 guard
 * entry reuses the fastest set rather than inventing values.
 */
static const struct hdmi_mpll_config gx6702_mpll_cfg[] = {
	/* GX6701 eCos phy_Configure(): 27 MHz / 8-bit path. */
	{  27000000, 0x01e0, 0x0000, 0x0210 },
	{  74250000, 0x0140, 0x0005, 0x06dc },
	{ 148500000, 0x00a0, 0x000a, 0x06dc },
	{	~0UL, 0x00a0, 0x000a, 0x06dc },
};

/* Identical for all three clocks in the vendor sequence. */
static const struct hdmi_phy_config gx6702_phy_cfg[] = {
	{  27000000, 0x8009, 0x0006, 0x0273 },
	{  74250000, 0x8009, 0x0006, 0x0210 },
	{ 148500000, 0x8009, 0x0006, 0x0210 },
	{	~0UL, 0x8009, 0x0006, 0x0210 },
};

/*
 * Extra PHY register the vendor programs and dw_hdmi.c has no name for.
 * Written before the PLL registers, along with the two values below that
 * differ from the generic sequence.
 */
#define GX6702_PHY_REG1E		0x1E
#define GX6702_PHY_REG1E_VAL		0x0070
#define GX6702_PHY_PLLPHBYCTRL_VAL	0x0800	/* GX6702 PHY probe fallback */
#define GX6702_PHY_PLLCLKBISTPHASE_VAL	0x0006	/* eCos phy_Configure */

/* Missing from U-Boot's older DWC header. */
#define GX6702_IH_I2CMPHY_ERROR		BIT(0)
#define GX6702_IH_I2CMPHY_DONE		BIT(1)

/* The PLL needs far longer than the 5 ms the generic wait allows. */
#define GX6702_PHY_LOCK_TIMEOUT_MS	100

static void gx6702_hdmi_mod(struct dw_hdmi *hdmi, u8 val, u8 mask, int offset)
{
	u8 reg = gx6702_hdmi_read(hdmi, offset) & ~mask;

	gx6702_hdmi_write(hdmi, reg | (val & mask), offset);
}

static void gx6702_phy_i2c_write(struct dw_hdmi *hdmi, u16 data, u8 addr)
{
	ulong start;

	gx6702_hdmi_write(hdmi, 0xff, HDMI_IH_I2CMPHY_STAT0);
	gx6702_hdmi_write(hdmi, addr, HDMI_PHY_I2CM_ADDRESS_ADDR);
	gx6702_hdmi_write(hdmi, data >> 8, HDMI_PHY_I2CM_DATAO_1_ADDR);
	gx6702_hdmi_write(hdmi, data, HDMI_PHY_I2CM_DATAO_0_ADDR);
	gx6702_hdmi_write(hdmi, HDMI_PHY_I2CM_OPERATION_ADDR_WRITE,
			  HDMI_PHY_I2CM_OPERATION_ADDR);

	start = get_timer(0);
	do {
		u8 stat = gx6702_hdmi_read(hdmi, HDMI_IH_I2CMPHY_STAT0);

		if (stat & GX6702_IH_I2CMPHY_ERROR) {
			gx6702_hdmi_write(hdmi, stat, HDMI_IH_I2CMPHY_STAT0);
			log_warning("gx6702-hdmi: PHY I2C write %02x=%04x failed\n",
				    addr, data);
			return;
		}
		if (stat & GX6702_IH_I2CMPHY_DONE) {
			gx6702_hdmi_write(hdmi, stat, HDMI_IH_I2CMPHY_STAT0);
			return;
		}
		udelay(100);
	} while (get_timer(start) < 10);

	log_warning("gx6702-hdmi: PHY I2C write %02x=%04x timed out\n",
		    addr, data);
}

/*
 * The generic dw_hdmi_phy_cfg() leaves the PLL unlocked on this part: it
 * writes 0x0000 to PLLPHBYCTRL and 0x0006 to PLLCLKBISTPHASE, and never
 * touches register 0x1E.  This is the sequence the vendor loader runs at
 * 0x931731E0-0x93173246 instead, which is otherwise the same power-down,
 * reset, program, power-up order.
 */
static int gx6702_hdmi_phy_set(struct dw_hdmi *hdmi, uint mpixelclock)
{
	const struct hdmi_mpll_config *mpll;
	const struct hdmi_phy_config *phy;
	ulong start;
	uint i;

	for (i = 0; gx6702_mpll_cfg[i].mpixelclock != ~0UL; i++)
		if (mpixelclock <= gx6702_mpll_cfg[i].mpixelclock)
			break;
	mpll = &gx6702_mpll_cfg[i];

	for (i = 0; gx6702_phy_cfg[i].mpixelclock != ~0UL; i++)
		if (mpixelclock <= gx6702_phy_cfg[i].mpixelclock)
			break;
	phy = &gx6702_phy_cfg[i];

	gx6702_hdmi_mod(hdmi, 0, HDMI_PHY_CONF0_GEN2_TXPWRON_MASK,
			HDMI_PHY_CONF0);
	gx6702_hdmi_mod(hdmi, HDMI_PHY_CONF0_GEN2_PDDQ_MASK,
			HDMI_PHY_CONF0_GEN2_PDDQ_MASK, HDMI_PHY_CONF0);

	gx6702_hdmi_write(hdmi, HDMI_MC_PHYRSTZ_DEASSERT, HDMI_MC_PHYRSTZ);
	gx6702_hdmi_write(hdmi, HDMI_MC_PHYRSTZ_ASSERT, HDMI_MC_PHYRSTZ);
	gx6702_hdmi_write(hdmi, HDMI_MC_HEACPHY_RST_ASSERT, HDMI_MC_HEACPHY_RST);

	gx6702_hdmi_mod(hdmi, HDMI_PHY_TST0_TSTCLR_MASK,
			HDMI_PHY_TST0_TSTCLR_MASK, HDMI_PHY_TST0);
	gx6702_hdmi_write(hdmi, HDMI_PHY_I2CM_SLAVE_ADDR_PHY_GEN2,
			  HDMI_PHY_I2CM_SLAVE_ADDR);
	gx6702_hdmi_mod(hdmi, 0, HDMI_PHY_TST0_TSTCLR_MASK, HDMI_PHY_TST0);

	gx6702_phy_i2c_write(hdmi, GX6702_PHY_REG1E_VAL, GX6702_PHY_REG1E);
	gx6702_phy_i2c_write(hdmi, GX6702_PHY_PLLPHBYCTRL_VAL, PHY_PLLPHBYCTRL);
	gx6702_phy_i2c_write(hdmi, GX6702_PHY_PLLCLKBISTPHASE_VAL,
			     PHY_PLLCLKBISTPHASE);
	gx6702_phy_i2c_write(hdmi, phy->term, PHY_TXTERM);
	gx6702_phy_i2c_write(hdmi, 0x8000, PHY_CKCALCTRL);

	gx6702_phy_i2c_write(hdmi, mpll->cpce, PHY_OPMODE_PLLCFG);
	gx6702_phy_i2c_write(hdmi, mpll->curr, PHY_PLLCURRCTRL);
	gx6702_phy_i2c_write(hdmi, mpll->gmp, PHY_PLLGMPCTRL);
	gx6702_phy_i2c_write(hdmi, phy->sym_ctr, PHY_CKSYMTXCTRL);
	gx6702_phy_i2c_write(hdmi, phy->vlev_ctr, PHY_VLEVCTRL);

	if (mpixelclock <= 27000000) {
		/*
		 * eCos finishes the Gen2 sequence as ENHPDRXSENSE=1, TXPWRON=1,
		 * PDDQ=0 and then waits for lock.  The previous generic sequence
		 * only wrote the resulting 0x0e after lock.
		 */
		gx6702_hdmi_write(hdmi, 0x0e, HDMI_PHY_CONF0);
	} else {
		/* Preserve the known-good HD/FHD power-up sequence. */
		gx6702_hdmi_mod(hdmi, HDMI_PHY_CONF0_PDZ_MASK,
				HDMI_PHY_CONF0_PDZ_MASK, HDMI_PHY_CONF0);
		gx6702_hdmi_mod(hdmi, 0, HDMI_PHY_CONF0_ENTMDS_MASK,
				HDMI_PHY_CONF0);
		gx6702_hdmi_mod(hdmi, HDMI_PHY_CONF0_ENTMDS_MASK,
				HDMI_PHY_CONF0_ENTMDS_MASK, HDMI_PHY_CONF0);
		gx6702_hdmi_mod(hdmi, HDMI_PHY_CONF0_SPARECTRL_MASK,
				HDMI_PHY_CONF0_SPARECTRL_MASK, HDMI_PHY_CONF0);
		gx6702_hdmi_mod(hdmi, HDMI_PHY_CONF0_GEN2_TXPWRON_MASK,
				HDMI_PHY_CONF0_GEN2_TXPWRON_MASK, HDMI_PHY_CONF0);
		gx6702_hdmi_mod(hdmi, 0, HDMI_PHY_CONF0_GEN2_PDDQ_MASK,
				HDMI_PHY_CONF0);
	}

	/*
	 * dw_hdmi.c returns as soon as the lock bit reads back clear, so it
	 * always reports success without waiting.  Wait for the bit to be set
	 * instead: an unlocked PLL means no TMDS clock and a blank sink.
	 */
	start = get_timer(0);
	do {
		if (gx6702_hdmi_read(hdmi, HDMI_PHY_STAT0) &
		    HDMI_PHY_TX_PHY_LOCK)
			return 0;
		udelay(100);
	} while (get_timer(start) < GX6702_PHY_LOCK_TIMEOUT_MS);

	log_warning("gx6702-hdmi: PHY PLL did not lock for %u Hz\n",
		    mpixelclock);

	return -ETIMEDOUT;
}

static const struct dw_hdmi_phy_ops gx6702_hdmi_phy_ops = {
	.phy_set	= gx6702_hdmi_phy_set,
};

static struct dw_hdmi gx6702_hdmi = {
	.ioaddr		= GX6702_HDMI_BASE,
	.mpll_cfg	= gx6702_mpll_cfg,
	.phy_cfg	= gx6702_phy_cfg,
	/* eCos post-EDID I2CM snapshot: SCL HCNT=0xad, LCNT=0xc8. */
	.i2c_clk_high	= 0xad,
	.i2c_clk_low	= 0xc8,
	.reg_io_width	= 4,
	.ops		= &gx6702_hdmi_phy_ops,
	.write_reg	= gx6702_hdmi_write,
	.read_reg	= gx6702_hdmi_read,

	/*
	 * Stock golden TX_INVID0 is 0x09 (YUV8/444), not 0x16 (UYVY422).
	 * The DVE presents a 444-mapped bus; claiming UYVY made CSC enable
	 * chroma interpolation (CSC_CFG=0x10) and left 1080i as "unsupported"
	 * on the Samsung while progressive still locked.
	 */
	.hdmi_data	= {
		.enc_in_bus_format	= MEDIA_BUS_FMT_YUV8_1X24,
		.enc_out_bus_format	= MEDIA_BUS_FMT_RGB888_1X24,
	},
};

/*
 * hdmi_enable_video_path() bypasses the converter and leaves its clock gated
 * whatever the formats say, so the coefficients hdmi_video_csc() programmed
 * only take effect once the block is put back in the path here.
 */
static void gx6702_hdmi_csc_enable(struct dw_hdmi *hdmi)
{
	gx6702_hdmi_write(hdmi, HDMI_MC_FLOWCTRL_FEED_THROUGH_OFF_CSC_IN_PATH,
			  HDMI_MC_FLOWCTRL);
	gx6702_hdmi_mod(hdmi, 0, HDMI_MC_CLKDIS_CSCCLK_DISABLE, HDMI_MC_CLKDIS);
}

/* Not in U-Boot's dw_hdmi.h; match Linux include/drm/dw_hdmi.h. */
#define GX6702_HDMI_FC_DATAUTO0		0x10B3
#define GX6702_HDMI_FC_DATAUTO1		0x10B4
#define GX6702_HDMI_FC_DATAUTO2		0x10B5

static bool gx6702_vic_is_sd(u8 vic)
{
	return vic == 2 || vic == 6 || vic == 17 || vic == 21;
}

/*
 * dw_hdmi.c never programs an AVI InfoFrame.  After a DVI bring-up (no audio
 * regenerator), promote to HDMI and publish VIC so sinks accept 1080i.
 */
static void gx6702_hdmi_avi_infoframe(struct dw_hdmi *hdmi, u8 vic)
{
	/*
	 * HD AVI is 0x40 / 0xa8 / 0x00 + VIC (16:9 + ITUR_BT709).
	 * SD VICs 2/6/17/21 are 4:3 + SMPTE/BT601 (eCos dumps).
	 */
	u8 conf0 = HDMI_FC_AVICONF0_PIX_FMT_RGB |
		   HDMI_FC_AVICONF0_ACTIVE_FMT_INFO_PRESENT |
		   HDMI_FC_AVICONF0_SCAN_INFO_NODATA;
	u8 conf1;
	u8 conf2 = HDMI_FC_AVICONF2_SCALING_NONE |
		   HDMI_FC_AVICONF2_RGB_QUANT_DEFAULT;
	u8 inv;
	uint count;

	if (gx6702_vic_is_sd(vic))
		conf1 = HDMI_FC_AVICONF1_CODED_ASPECT_RATIO_4_3 |
			HDMI_FC_AVICONF1_ACTIVE_ASPECT_RATIO_USE_CODED |
			HDMI_FC_AVICONF1_COLORIMETRY_SMPTE;
	else
		conf1 = HDMI_FC_AVICONF1_CODED_ASPECT_RATIO_16_9 |
			HDMI_FC_AVICONF1_ACTIVE_ASPECT_RATIO_USE_CODED |
			HDMI_FC_AVICONF1_COLORIMETRY_ITUR;

	gx6702_hdmi_write(hdmi, conf0, HDMI_FC_AVICONF0);
	gx6702_hdmi_write(hdmi, conf1, HDMI_FC_AVICONF1);
	gx6702_hdmi_write(hdmi, conf2, HDMI_FC_AVICONF2);
	gx6702_hdmi_write(hdmi, 0, HDMI_FC_AVICONF3);
	gx6702_hdmi_write(hdmi, vic & 0x7f, HDMI_FC_AVIVID);

	/*
	 * Stock programs DATAUTO1/2.  For HD, keep AVI+GCP auto-send (0x03)
	 * so VIC 20 is on the wire.  For VIC 21, GxLoader leaves DATAUTO0=0;
	 * forcing 0x03 made Samsung OSD "1440x576i" (CEA name for VIC 21)
	 * instead of the measured "720x576i" GxLoader gets.
	 */
	gx6702_hdmi_write(hdmi, 1, GX6702_HDMI_FC_DATAUTO1);
	gx6702_hdmi_write(hdmi, 0x11, GX6702_HDMI_FC_DATAUTO2);
	gx6702_hdmi_write(hdmi, gx6702_vic_is_sd(vic) ? 0x00 : 0x03,
			  GX6702_HDMI_FC_DATAUTO0);

	gx6702_hdmi_mod(hdmi, HDMI_FC_INVIDCONF_DVI_MODEZ_HDMI_MODE,
			HDMI_FC_INVIDCONF_DVI_MODEZ_MASK, HDMI_FC_INVIDCONF);

	/* Re-latch INVIDCONF like hdmi_clear_overflow(), without audio. */
	gx6702_hdmi_write(hdmi, (u8)~HDMI_MC_SWRSTZ_TMDSSWRST_REQ,
			  HDMI_MC_SWRSTZ);
	inv = gx6702_hdmi_read(hdmi, HDMI_FC_INVIDCONF);
	for (count = 0; count < 4; count++)
		gx6702_hdmi_write(hdmi, inv, HDMI_FC_INVIDCONF);
}

static void gx6702_hdmi_apply_ecos_fc(struct dw_hdmi *hdmi, u8 vic)
{
	const struct gx6702_ecos_golden *g = NULL;
	unsigned int mode, i;

	for (mode = GX6702_MODE_480I60; mode < GX6702_MODE_COUNT; mode++) {
		g = gx6702_ecos_golden(mode);
		if (g && g->vic == vic)
			break;
		g = NULL;
	}
	if (!g)
		return;

	for (i = 0; i < GX6702_ECOS_HDMI_FC_BYTES; i++)
		gx6702_hdmi_write(hdmi, g->hdmi_fc[i], 0x1000 + i);
}

static void gx6702_hdmi_apply_ecos_extra(struct dw_hdmi *hdmi, u8 vic)
{
	const struct gx6702_ecos_golden *g = NULL;
	unsigned int mode, i;

	for (mode = GX6702_MODE_480I60; mode < GX6702_MODE_COUNT; mode++) {
		g = gx6702_ecos_golden(mode);
		if (g && g->vic == vic)
			break;
		g = NULL;
	}
	if (!g)
		return;

	for (i = 0; i < g->hdmi_extra_count; i++)
		gx6702_hdmi_write(hdmi, g->hdmi_extra[i].val,
				  g->hdmi_extra[i].reg);
}

int gx6702_hdmi_enable(const struct gx6702_video_mode_info *mode)
{
	struct display_timing timing = mode->timing;
	int ret;

	dw_hdmi_init(&gx6702_hdmi);

	/* Configure video first, then overlay the deterministic eCos bytes. */
	ret = dw_hdmi_detect_hpd(&gx6702_hdmi);
	if (ret)
		log_debug("gx6702-hdmi: no HPD, continuing\n");
	/* U-Boot emits video only; do not configure the unused I2S/ACR path. */
	timing.hdmi_monitor = false;

	/*
	 * GxLoader SD interlaced HDMI FC is 1440-wide (CEA 720(1440)x576i)
	 * while the DVE/layer stay at 720.  Double the horizontal timing so
	 * dw_hdmi programs the same FC as gxloader-576p-reg-dump.txt.
	 */
	if ((timing.flags & DISPLAY_FLAGS_INTERLACED) &&
	    timing.hactive.typ == 720 &&
	    timing.pixelclock.typ <= 27000000) {
		timing.hactive.typ = 1440;
		timing.hfront_porch.typ *= 2;
		timing.hsync_len.typ *= 2;
		timing.hback_porch.typ *= 2;
	}

	ret = dw_hdmi_enable(&gx6702_hdmi, &timing);
	if (ret) {
		log_warning("gx6702-hdmi: TX setup failed (%d)\n", ret);
		return ret;
	}

	/* Match eCos's steady Gen2 control state after every successful lock. */
	gx6702_hdmi_write(&gx6702_hdmi, 0x0e, HDMI_PHY_CONF0);

	gx6702_hdmi_csc_enable(&gx6702_hdmi);
	/* Always run this to match eCos AVI contents and auto-send settings. */
	gx6702_hdmi_avi_infoframe(&gx6702_hdmi, mode->vic);

	/* Overlay FC timing + AVI bytes from the live eCos dump. */
	gx6702_hdmi_apply_ecos_fc(&gx6702_hdmi, mode->vic);
	/* Overlay deterministic packetizer/auxiliary bytes from eCos. */
	gx6702_hdmi_apply_ecos_extra(&gx6702_hdmi, mode->vic);

	/*
	 * The golden overlay includes MC_CLKDIS=0x64, which enables AUDCLK.  Gate
	 * it again for video-only HDMI.  HDMI mode itself was restored by the AVI
	 * setup.
	 */
	gx6702_hdmi_mod(&gx6702_hdmi,
			HDMI_MC_CLKDIS_AUDCLK_DISABLE,
			HDMI_MC_CLKDIS_AUDCLK_DISABLE, HDMI_MC_CLKDIS);

	return 0;
}

/* Cached last successful EDID read (block 0 + optional CEA). */
static u8 gx6702_edid_cache[256];
static int gx6702_edid_len;
static bool gx6702_edid_valid;

void gx6702_hdmi_invalidate_edid(void)
{
	gx6702_edid_valid = false;
	gx6702_edid_len = 0;
}

bool gx6702_hdmi_hpd_connected(void)
{
	/*
	 * PHY_STAT0.HPD is live once the TX has been inited.  Avoid the
	 * blocking dw_hdmi_detect_hpd() wait so cyclic/hotplug can poll.
	 */
	dw_hdmi_init(&gx6702_hdmi);
	return !!(gx6702_hdmi_read(&gx6702_hdmi, HDMI_PHY_STAT0) &
		  HDMI_PHY_HPD);
}

int gx6702_hdmi_read_edid(u8 *buf, int buf_size)
{
	int ret;

	if (!buf || buf_size < 128)
		return -EINVAL;

	dw_hdmi_init(&gx6702_hdmi);

	ret = dw_hdmi_detect_hpd(&gx6702_hdmi);
	if (ret) {
		log_debug("gx6702-hdmi: EDID: no HPD\n");
		gx6702_edid_valid = false;
		gx6702_edid_len = 0;
		return ret;
	}

	ret = dw_hdmi_read_edid(&gx6702_hdmi, buf, buf_size);
	if (ret < 0) {
		log_warning("gx6702-hdmi: EDID read failed\n");
		gx6702_edid_valid = false;
		gx6702_edid_len = 0;
		return ret;
	}

	if (edid_check_checksum(buf)) {
		log_warning("gx6702-hdmi: EDID block 0 checksum bad\n");
		gx6702_edid_valid = false;
		gx6702_edid_len = 0;
		return -EIO;
	}

	gx6702_edid_len = ret > (int)sizeof(gx6702_edid_cache) ?
			  (int)sizeof(gx6702_edid_cache) : ret;
	memcpy(gx6702_edid_cache, buf, gx6702_edid_len);
	gx6702_edid_valid = true;

	return gx6702_edid_len;
}

/*
 * Walk CEA-861 data blocks for Video Data Block SVDs.  Native bit (bit 7)
 * is ignored; only VIC 1..127 matter for our eCos mode table.
 */
static bool gx6702_cea_has_vic(const u8 *edid, int len, u8 vic)
{
	const struct edid_cea861_info *cea;
	unsigned int offset, db_end, i, db_len, db_type;
	u8 svd;

	if (len < 256 || edid[0x7e] == 0)
		return false;

	cea = (const struct edid_cea861_info *)(edid + 128);
	if (cea->extension_tag != EDID_CEA861_EXTENSION_TAG)
		return false;

	db_end = cea->dtd_offset ? cea->dtd_offset : 127;
	if (db_end > 127)
		db_end = 127;

	for (offset = 0; offset + 1 < db_end; ) {
		db_type = EDID_CEA861_DB_TYPE(*cea, offset);
		db_len = EDID_CEA861_DB_LEN(*cea, offset);
		offset++;

		if (offset + db_len > db_end)
			break;

		if (db_type == EDID_CEA861_DB_VIDEO) {
			for (i = 0; i < db_len; i++) {
				svd = cea->data[offset + i] & 0x7f;
				if (svd == vic)
					return true;
			}
		}

		offset += db_len;
	}

	return false;
}

bool gx6702_hdmi_sink_supports_vic(u8 vic, bool *edid_ok)
{
	u8 buf[256];
	int len;
	bool has_cea_vdb = false;
	const struct edid_cea861_info *cea;
	unsigned int offset, db_end, db_len, db_type;

	if (edid_ok)
		*edid_ok = false;

	if (!vic)
		return true;

	if (gx6702_edid_valid && gx6702_edid_len >= 128) {
		len = gx6702_edid_len;
		memcpy(buf, gx6702_edid_cache, len);
	} else {
		len = gx6702_hdmi_read_edid(buf, sizeof(buf));
		if (len < 0)
			return true; /* fail-open: no sink / DDC fail */
	}

	if (edid_ok)
		*edid_ok = true;

	/* Prefer CEA SVDs when a Video Data Block exists. */
	if (len >= 256 && buf[0x7e] != 0) {
		cea = (const struct edid_cea861_info *)(buf + 128);
		if (cea->extension_tag == EDID_CEA861_EXTENSION_TAG) {
			db_end = cea->dtd_offset ? cea->dtd_offset : 127;
			if (db_end > 127)
				db_end = 127;
			for (offset = 0; offset + 1 < db_end; ) {
				db_type = EDID_CEA861_DB_TYPE(*cea, offset);
				db_len = EDID_CEA861_DB_LEN(*cea, offset);
				offset++;
				if (offset + db_len > db_end)
					break;
				if (db_type == EDID_CEA861_DB_VIDEO &&
				    db_len > 0)
					has_cea_vdb = true;
				offset += db_len;
			}
		}
	}

	if (has_cea_vdb)
		return gx6702_cea_has_vic(buf, len, vic);

	/* No VIDEO DB: cannot prove unsupported — allow. */
	return true;
}

u8 gx6702_hdmi_cea_native_vic(bool *edid_ok)
{
	u8 buf[256];
	const struct edid_cea861_info *cea;
	unsigned int offset, db_end, i, db_len, db_type;
	int len;

	if (edid_ok)
		*edid_ok = false;

	if (gx6702_edid_valid && gx6702_edid_len >= 128) {
		len = gx6702_edid_len;
		memcpy(buf, gx6702_edid_cache, len);
	} else {
		len = gx6702_hdmi_read_edid(buf, sizeof(buf));
		if (len < 0)
			return 0;
	}

	if (edid_ok)
		*edid_ok = true;

	if (len < 256 || buf[0x7e] == 0)
		return 0;

	cea = (const struct edid_cea861_info *)(buf + 128);
	if (cea->extension_tag != EDID_CEA861_EXTENSION_TAG)
		return 0;

	db_end = cea->dtd_offset ? cea->dtd_offset : 127;
	if (db_end > 127)
		db_end = 127;

	for (offset = 0; offset + 1 < db_end; ) {
		db_type = EDID_CEA861_DB_TYPE(*cea, offset);
		db_len = EDID_CEA861_DB_LEN(*cea, offset);
		offset++;
		if (offset + db_len > db_end)
			break;
		if (db_type == EDID_CEA861_DB_VIDEO) {
			for (i = 0; i < db_len; i++) {
				if (cea->data[offset + i] & 0x80)
					return cea->data[offset + i] & 0x7f;
			}
		}
		offset += db_len;
	}

	return 0;
}
