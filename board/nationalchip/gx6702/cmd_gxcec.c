// SPDX-License-Identifier: GPL-2.0+
/*
 * gxcec - HDMI CEC control.
 *
 * The CEC wire is P0.5 of the always-on 8051, so the open LPC firmware does
 * the work: it posts Image View On / System Standby, decodes bus frames,
 * ACKs the addresses we own, answers Give Device Power Status, and in soft
 * standby cold-boots the CK610 on a Set Stream Path, Active Source or Routing
 * Change for our HDMI physical address.  The DesignWare block at 0xA4F00000
 * is only used to read the EDID physical address and claim a logical address;
 * it is not wired to the bus on the GX6702 boards tested (a ping to the TV
 * at address 0 is never ACKed).
 *
 * Typical use:
 *   gxcec on              cecmode 1: standby posts 0x36, power-on posts 0x04
 *   gxcec poll 30         print frames the 8051 decodes (-v shows everything)
 *   gxlp sleep after      then switch the TV to this input to wake the box
 *
 * Diagnostics: `gxcec dump` (clock word and Timer0 rate; 0x016e3600 is a
 * 24 MHz 8051), `gxcec edges` (record one raw frame for
 * gxtest/tools/cec/cec_edges_decode.py) and `gxcec snap`.  ABI is 1.11.
 */

#include <command.h>
#include <errno.h>
#include <fdtdec.h>
#include <stdio.h>
#include <vsprintf.h>
#include <asm/io.h>
#include <linux/string.h>

#include "../../../cmd/gx_lpc.h"
#include "gx6702_video.h"

static int gxcec_snap(void)
{
	void __iomem *base = (void __iomem *)(GX_LPC_SHARED + 0x17c);
	void __iomem *status_reg = (void __iomem *)(GX_LPC_SHARED + 0x108);
	u8 raw[52];
	u32 status;
	u8 seq, istat, rx_seq, rx_len, i;

	status = readl(status_reg);
	if ((status & 0xff) != GX_LPC_STATUS_READY ||
	    ((status >> 8) & 0xff) != GX_LPC_ABI_MAJOR ||
	    ((status >> 16) & 0xff) != GX_LPC_ABI_MINOR) {
		printf("gxcec: snap needs open LPC ABI %u.%u\n",
		       GX_LPC_ABI_MAJOR, GX_LPC_ABI_MINOR);
		return CMD_RET_FAILURE;
	}
	for (i = 0; i < sizeof(raw); i += 4) {
		u32 word = readl(base + i);

		raw[i] = word;
		raw[i + 1] = word >> 8;
		raw[i + 2] = word >> 16;
		raw[i + 3] = word >> 24;
	}
	seq = raw[0];
	istat = raw[1];
	printf("gxcec: LPC engine snap seq %u SFR 0xab %02x\n", seq, istat);
	printf("gxcec: XDATA 8000");
	for (i = 0; i < 48; i++)
		printf(" %02x", raw[2 + i]);
	printf("\n");
	rx_seq = raw[0x1ae - 0x17c];
	rx_len = raw[0x1af - 0x17c];
	printf("gxcec: GPIO frame seq %u len %u", rx_seq, rx_len);
	if (rx_len > 16)
		rx_len = 16;
	for (i = 0; i < rx_len; i++) {
		u32 word = readl((void __iomem *)(GX_LPC_SHARED + 0x1b0 +
						    (i & ~3)));

		printf(" %02x", (word >> (8 * (i & 3))) & 0xff);
	}
	printf("\n");
	return CMD_RET_SUCCESS;
}

static int do_gxcec_tx(int argc, char *const argv[])
{
	u8 frame[16];
	int i, n;

	n = argc - 2;
	if (n < 1 || n > 16)
		return CMD_RET_USAGE;
	for (i = 0; i < n; i++) {
		char *end = NULL;
		ulong v = simple_strtoul(argv[i + 2], &end, 16);

		if (!end || end == argv[i + 2] || *end || v > 0xff) {
			printf("gxcec: bad byte '%s'\n", argv[i + 2]);
			return CMD_RET_USAGE;
		}
		frame[i] = v;
	}
	return gx6702_cec_tx(frame, n) ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

static int do_gxcec(struct cmd_tbl *cmdtp, int flag, int argc,
		    char *const argv[])
{
	const char *cmd;
	ulong seconds;
	char *end;
	ulong mode;

	bool verbose;

	if (argc < 2)
		return CMD_RET_USAGE;
	verbose = argc > 2 && !strcmp(argv[argc - 1], "-v");
	if (verbose)
		argc--;
	gx6702_cec_set_verbose(verbose);
	cmd = argv[1];

	if (!strcmp(cmd, "on")) {
		u16 pa = 0;
		int have;
		int announced;

		if (gx_lpc_ensure_open("boot"))
			return CMD_RET_FAILURE;
		gx_lpc8051_set_cecmode(1);
		if (gx6702_cec_set_mode(1))
			return CMD_RET_FAILURE;
		have = gx6702_cec_physical(&pa);
		announced = gx_lpc8051_cec_announce(pa, have);
		if (announced)
			printf("gxcec: LPC Image View On failed (%d)\n",
			       announced);
		else if (verbose)
			printf("gxcec: LPC Image View On posted\n");
		return announced ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
	}
	if (!strcmp(cmd, "off")) {
		gx_lpc8051_set_cecmode(0);
		return gx6702_cec_set_mode(0) ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
	}
	if (!strcmp(cmd, "dump")) {
		gx6702_cec_dump();
		return CMD_RET_SUCCESS;
	}
	if (!strcmp(cmd, "snap"))
		return gxcec_snap();
	if (!strcmp(cmd, "edges")) {
		int rec;

		if (gx_lpc_ensure_open("boot"))
			return CMD_RET_FAILURE;
		gx_lpc8051_set_cecmode(1);
		rec = gx_lpc8051_cec_edges();
		if (rec == -EINTR)
			return CMD_RET_SUCCESS;
		return rec ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
	}
	if (!strcmp(cmd, "poll")) {
		int polled;

		if (gx_lpc_ensure_open("boot"))
			return CMD_RET_FAILURE;
		gx_lpc8051_set_cecmode(1);

		seconds = 15;
		if (argc >= 3) {
			seconds = simple_strtoul(argv[2], &end, 10);
			if (!end || end == argv[2] || *end)
				return CMD_RET_USAGE;
		}
		polled = gx6702_cec_poll(seconds);
		if (polled == -EINTR)
			return CMD_RET_SUCCESS;
		return polled ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
	}
	if (!strcmp(cmd, "viewon")) {
		int posted;

		if (gx_lpc_ensure_open("boot"))
			return CMD_RET_FAILURE;
		posted = gx_lpc8051_cec_post(0x04);
		if (posted)
			printf("gxcec: LPC Image View On failed (%d)\n", posted);
		else
			printf("gxcec: LPC Image View On posted\n");
		return posted ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
	}
	if (!strcmp(cmd, "standby")) {
		int posted;

		if (gx_lpc_ensure_open("boot"))
			return CMD_RET_FAILURE;
		posted = gx_lpc8051_cec_post(0x36);

		if (posted)
			printf("gxcec: LPC System Standby failed (%d)\n", posted);
		else
			printf("gxcec: LPC System Standby posted\n");
		return posted ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
	}
	if (!strcmp(cmd, "tx"))
		return do_gxcec_tx(argc, argv);
	if (!strcmp(cmd, "mode")) {
		if (argc < 3) {
			printf("gxcec: mode %d\n", gx6702_cec_mode());
			return CMD_RET_SUCCESS;
		}
		mode = simple_strtoul(argv[2], &end, 10);
		if (!end || end == argv[2] || *end || mode > 2)
			return CMD_RET_USAGE;
		gx_lpc8051_set_cecmode(mode);
		return gx6702_cec_set_mode(mode) ? CMD_RET_FAILURE :
		       CMD_RET_SUCCESS;
	}
	return CMD_RET_USAGE;
}

U_BOOT_CMD(gxcec, 18, 0, do_gxcec,
	   "GX6702 HDMI CEC",
	   "on|off|mode|poll|viewon|standby|tx|dump|snap|edges [-v]\n"
	   "gxcec on                         - cecmode 1: standby posts 0x36, power-on posts 0x04\n"
	   "gxcec off                        - cecmode 0, and gate the HDMI CEC clock\n"
	   "gxcec mode [0|1|2]               - 1 wakes the TV on box power; 2 standby only\n"
	   "gxcec poll [seconds]             - print frames decoded by the LPC (default 15s)\n"
	   "gxcec viewon                     - LPC engine posts Image View On (0x04)\n"
	   "gxcec standby                    - LPC engine posts System Standby (0x36)\n"
	   "gxcec tx <hex bytes>             - raw frame through the DesignWare block\n"
	   "gxcec dump                       - clock, status, address, LPC Timer0 rate\n"
	   "gxcec snap                       - LPC XDATA 0x8000 snapshot and last GPIO frame\n"
	   "gxcec edges                      - record the next bus frame as raw level times\n"
	   "                                   (decode: gxtest/tools/cec/cec_edges_decode.py)\n"
	   "-v shows address claim, hardware status and header-only polls.\n"
	   "gxlp sleep after gxcec on cold-boots when the TV selects this input\n"
	   "(Set Stream Path, Active Source or Routing Change for our address) or sends\n"
	   "a directed Image/Text View On.  System Standby does not wake.\n");
