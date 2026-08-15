// SPDX-License-Identifier: GPL-2.0+
/*
 * Dump GX6702 video MMIO in the same line shapes as gxtest/stock_regdump.c
 * so gxtest/capture_video_regs.py can --diff stock vs U-Boot captures.
 *
 * Also: `gxvideo mode <1-10> [force]` switches DVE/HDMI/layer at runtime and
 * redraws the centered splash (framebuffer is reserved for 1920x1080).
 * `gxvideo edid` reads the sink EDID over the DW HDMI DDC.
 */

#include <command.h>
#include <edid.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/delay.h>
#include <asm/io.h>
#include <fdtdec.h>

#include "gx6702_video.h"

static void put_nibble(u32 v)
{
	v &= 0xf;
	putc(v < 10 ? '0' + v : 'a' + (v - 10));
}

static void put_hex8(u32 v)
{
	put_nibble(v >> 4);
	put_nibble(v);
}

static void put_hex16(u32 v)
{
	put_hex8(v >> 8);
	put_hex8(v);
}

static void put_hex32(u32 v)
{
	put_hex16(v >> 16);
	put_hex16(v);
}

static void dump_words(const char *name, ulong first, u32 bytes)
{
	ulong addr;
	u32 i;

	printf("\n-- %s --\n", name);
	for (addr = first; addr < first + bytes; addr += 0x10) {
		put_hex32(addr);
		putc(':');
		for (i = 0; i < 4; i++) {
			putc(' ');
			put_hex32(readl(addr + i * 4));
		}
		putc('\n');
	}
}

static void dump_hdmi_regs(u32 first, u32 count)
{
	u32 reg, i;

	for (reg = first; reg < first + count; reg += 0x10) {
		put_hex16(reg);
		putc(':');
		for (i = 0; i < 0x10; i++) {
			putc(' ');
			put_hex8(readb(GX6702_HDMI_BASE + ((reg + i) << 2)));
		}
		putc('\n');
	}
}

static void dump_hdmi(void)
{
	printf("\n-- HDMI TX 0xA4F00000 (byte regs at base + reg*4) --\n");
	dump_hdmi_regs(0x0000, 0x0010);
	dump_hdmi_regs(0x0100, 0x0100);
	dump_hdmi_regs(0x0200, 0x0010);
	dump_hdmi_regs(0x0800, 0x0030);
	dump_hdmi_regs(0x1000, 0x0100);
	dump_hdmi_regs(0x3000, 0x0010);
	dump_hdmi_regs(0x4000, 0x0010);
	dump_hdmi_regs(0x4100, 0x0020);
	dump_hdmi_regs(0x7E00, 0x0030);
}

static void print_cea_vics(const u8 *edid, int len)
{
	const struct edid_cea861_info *cea;
	unsigned int offset, db_end, i, db_len, db_type;
	u8 svd;
	int printed = 0;

	if (len < 256 || edid[0x7e] == 0) {
		printf("CEA: (no extension)\n");
		return;
	}

	cea = (const struct edid_cea861_info *)(edid + 128);
	if (cea->extension_tag != EDID_CEA861_EXTENSION_TAG) {
		printf("CEA: tag 0x%02x (not CEA-861)\n", cea->extension_tag);
		return;
	}

	printf("CEA: rev %u  YCbCr444=%u  YCbCr422=%u  audio=%u\n",
	       cea->revision,
	       EDID_CEA861_SUPPORTS_YUV444(*cea) ? 1 : 0,
	       EDID_CEA861_SUPPORTS_YUV422(*cea) ? 1 : 0,
	       EDID_CEA861_SUPPORTS_BASIC_AUDIO(*cea) ? 1 : 0);

	db_end = cea->dtd_offset ? cea->dtd_offset : 127;
	if (db_end > 127)
		db_end = 127;

	printf("VICs:");
	for (offset = 0; offset + 1 < db_end; ) {
		db_type = EDID_CEA861_DB_TYPE(*cea, offset);
		db_len = EDID_CEA861_DB_LEN(*cea, offset);
		offset++;
		if (offset + db_len > db_end)
			break;
		if (db_type == EDID_CEA861_DB_VIDEO) {
			for (i = 0; i < db_len; i++) {
				svd = cea->data[offset + i] & 0x7f;
				printf(" %u", svd);
				printed++;
			}
		}
		offset += db_len;
	}
	if (!printed)
		printf(" (none)");
	putc('\n');
}

static int do_gxvideo_edid(void)
{
	u8 edid[256];
	struct edid1_info *info = (struct edid1_info *)edid;
	int len, i;

	len = gx6702_hdmi_read_edid(edid, sizeof(edid));
	if (len < 0) {
		printf("gxvideo: EDID read failed (%d) - no HPD or DDC NACK\n",
		       len);
		return CMD_RET_FAILURE;
	}

	printf("EDID: %d bytes\n", len);
	if (!edid_check_info(info))
		edid_print_info(info);
	else
		printf("(base block failed edid_check_info)\n");

	print_cea_vics(edid, len);

	printf("raw:");
	for (i = 0; i < len; i++) {
		if ((i & 0xf) == 0)
			printf("\n%02x:", i);
		printf(" %02x", edid[i]);
	}
	putc('\n');

	return CMD_RET_SUCCESS;
}

static int do_gxvideo_hpd(void)
{
	bool plugged = gx6702_hdmi_hpd_connected();
	enum gx6702_cvbs_state cvbs = gx6702_video_cvbs_state();
	unsigned int cur = gx6702_video_current_mode();
	const struct gx6702_video_mode_info *mode =
		gx6702_video_mode_info(cur);

	printf("HDMI HPD: %s\n", plugged ? "connected" : "disconnected");
	printf("policy: %s\n",
	       gx6702_video_auto_enabled() ? "auto" : "manual");
	if (mode)
		printf("current: %u  %s\n", cur, mode->name);
	if (cvbs == GX6702_CVBS_ACTIVE_PAL)
		printf("CVBS: PAL 720x576i@50 active\n");
	else if (cvbs == GX6702_CVBS_ACTIVE_NTSC)
		printf("CVBS: NTSC 720x480i@60 active\n");
	else
		printf("CVBS: disabled (nationalchip,ypbpr-only)\n");
	if (gx6702_video_auto_enabled() || plugged) {
		unsigned int best = gx6702_video_pick_auto_mode();

		mode = gx6702_video_mode_info(best);
		if (mode)
			printf("auto would pick: %u  %s\n", best, mode->name);
	}

	return CMD_RET_SUCCESS;
}

static int do_gxvideo_mode(int argc, char *const argv[])
{
	unsigned long index;
	const struct gx6702_video_mode_info *mode;
	bool force = false;
	bool edid_ok = false;
	unsigned int cur = gx6702_video_current_mode();
	unsigned int auto_pick;
	int ret;

	if (argc < 1) {
		auto_pick = gx6702_video_pick_auto_mode();
		mode = gx6702_video_mode_info(auto_pick);
		printf("modes:  (HPD %s, policy %s, current %u)\n",
		       gx6702_hdmi_hpd_connected() ? "in" : "out",
		       gx6702_video_auto_enabled() ? "auto" : "manual",
		       cur);
		printf("  0  auto%s%s\n",
		       mode ? " -> " : "",
		       mode ? mode->name : "");
		for (index = GX6702_MODE_480I60; index < GX6702_MODE_COUNT;
		     index++) {
			mode = gx6702_video_mode_info(index);
			if (!mode)
				continue;
			printf("  %lu  %s", index, mode->name);
			if (mode->cvbs_standard == GX6702_CVBS_PAL)
				printf("  [PAL CVBS]");
			else if (mode->cvbs_standard == GX6702_CVBS_NTSC)
				printf("  [NTSC CVBS]");
			if (index == cur)
				printf("  *");
			if (mode->vic &&
			    gx6702_hdmi_sink_supports_vic(mode->vic, &edid_ok) &&
			    edid_ok)
				printf("  [ok]");
			else if (edid_ok && mode->vic)
				printf("  [unsupported]");
			putc('\n');
		}
		return CMD_RET_SUCCESS;
	}

	if (!strcmp(argv[0], "auto"))
		index = GX6702_MODE_AUTO;
	else
		index = simple_strtoul(argv[0], NULL, 10);

	if (index != GX6702_MODE_AUTO && !gx6702_video_mode_info(index)) {
		printf("gxvideo: mode %lu out of range (0=auto, 1..10)\n",
		       index);
		return CMD_RET_FAILURE;
	}

	if (argc >= 2 && !strcmp(argv[1], "force"))
		force = true;

	if (index == GX6702_MODE_AUTO) {
		printf("gxvideo: auto...\n");
		mode = NULL;
	} else {
		mode = gx6702_video_mode_info(index);
		printf("gxvideo: switching to %s%s...\n", mode->name,
		       force ? " (forced)" : "");
	}

	ret = gx6702_video_set_mode(index, force);
	if (ret == -ENOTSUPP) {
		printf("gxvideo: sink EDID does not list VIC %u; "
		       "retry with 'gxvideo mode %lu force'\n",
		       mode ? mode->vic : 0, index);
		return CMD_RET_FAILURE;
	}
	if (ret) {
		printf("gxvideo: set_mode failed: %d\n", ret);
		return CMD_RET_FAILURE;
	}

	if (index == GX6702_MODE_AUTO) {
		mode = gx6702_video_mode_info(gx6702_video_current_mode());
		if (mode)
			printf("gxvideo: auto selected %u %s\n",
			       gx6702_video_current_mode(), mode->name);
	}

	return CMD_RET_SUCCESS;
}

static int do_gxvideo_jpeg(int argc, char *const argv[])
{
	enum gx6702_jpeg_scale scale = GX6702_JPEG_STRETCH;
	const char *scale_name = "stretch";
	ulong addr, size;
	int ret;

	if (argc < 2 || argc > 3)
		return CMD_RET_USAGE;
	addr = hextoul(argv[0], NULL);
	size = hextoul(argv[1], NULL);
	if (argc == 3) {
		scale_name = argv[2];
		if (!strcmp(scale_name, "native"))
			scale = GX6702_JPEG_NATIVE;
		else if (!strcmp(scale_name, "fit"))
			scale = GX6702_JPEG_FIT;
		else if (!strcmp(scale_name, "stretch"))
			scale = GX6702_JPEG_STRETCH;
		else
			return CMD_RET_USAGE;
	}

	printf("gxvideo: hardware JPEG decode %lx bytes @ 0x%08lx (%s)...\n",
	       size, addr, scale_name);
	ret = gx6702_video_show_jpeg(addr, size, scale);
	if (ret) {
		printf("gxvideo: JPEG decode failed: %d\n", ret);
		return CMD_RET_FAILURE;
	}
	printf("gxvideo: hardware JPEG decode complete\n");

	return CMD_RET_SUCCESS;
}

static int do_gxvideo(struct cmd_tbl *cmdtp, int flag, int argc,
		      char *const argv[])
{
	const char *which = argc > 1 ? argv[1] : "all";

	if (!strcmp(which, "mode"))
		return do_gxvideo_mode(argc - 2, argv + 2);

	if (!strcmp(which, "edid"))
		return do_gxvideo_edid();

	if (!strcmp(which, "hpd"))
		return do_gxvideo_hpd();

	if (!strcmp(which, "jpeg"))
		return do_gxvideo_jpeg(argc - 2, argv + 2);

	if (!strcmp(which, "all") || !strcmp(which, "v")) {
		dump_words("CRG 0xA030A000", GX6702_CRG_BASE, 0x200);
		dump_words("video clock route 0xA0601000", 0xA0601000, 0x80);
		dump_words("DISP0 0xA4800000", GX6702_GFX_BASE, 0x200);
		dump_words("DISP0 0xA4800400", GX6702_GFX_BASE + 0x400, 0x500);
		dump_words("DISP0 0xA4804000", GX6702_DVE_BASE, 0x200);
		dump_hdmi();
		dump_words("DVE 0xA4400000", 0xA4400000, 0x100);
		dump_words("DISP1 0xA4900000", GX6702_VP_BASE, 0x200);
		dump_words("DISP1 0xA4900400", GX6702_VP_BASE + 0x400, 0x500);
		dump_words("DISP1 0xA4904000", GX6702_VP_BASE + 0x4000, 0x200);
		return 0;
	}
	if (!strcmp(which, "hdmi")) {
		dump_hdmi();
		return 0;
	}
	if (!strcmp(which, "dve")) {
		dump_words("DISP0 0xA4804000", GX6702_DVE_BASE, 0x200);
		dump_words("DVE 0xA4400000", 0xA4400000, 0x100);
		return 0;
	}

	return CMD_RET_USAGE;
}

U_BOOT_CMD(gxvideo, 5, 1, do_gxvideo,
	   "GX6702 video / mode / EDID / HPD / hardware JPEG",
	   "[all|hdmi|dve|edid|hpd]\n"
	   "gxvideo edid                    - read sink EDID + list CEA VICs\n"
	   "gxvideo hpd                     - HDMI hotplug status + auto pick\n"
	   "gxvideo mode                    - list modes 0..10 (+ EDID support)\n"
	   "gxvideo mode <0-10|auto> [force]\n"
	   "                                - 0/auto picks best VIC; enables\n"
	   "                                  hotplug reselect. force skips EDID.\n"
	   "gxvideo jpeg <addr> <size> [native|fit|stretch]\n"
	   "                                - HW-decode baseline 4:4:4 JPEG; native\n"
	   "                                  centres 1:1, fit preserves aspect,\n"
	   "                                  stretch fills screen (default).\n");
