// SPDX-License-Identifier: GPL-2.0+
/*
 * gxcec - drive the DesignWare HDMI CEC engine from U-Boot.
 *
 * `gxvideo cec` stays a read-only snapshot.  This command clocks the block,
 * claims a tuner logical address, and prints the LXDVB501 opcodes.
 *
 * Philips 24PHH4000/88 (EasyLink, October 2016) live gate.  Before the test:
 * Setup, TV settings, General settings, EasyLink.  Leave EasyLink, EasyLink
 * Remote Control, One-touch play, and One-touch standby On.  Keep the box
 * awake and log `gxcec poll` in this order:
 *
 * 1. `gxcec on` then `gxcec viewon` with the TV in standby.  Pass: the TV
 *    wakes on this input.  viewon posts Image View On through the LPC engine.
 * 2. SOURCES, select this HDMI device, then arrows, OK, and digits.  Pass:
 *    opcode 0x44.  Home and Options are not forwarded by this set.
 * 3. Put the TV into standby.  Pass: opcode 0x36.
 * 4. With the TV in standby, press Play, then try SOURCES.  Record every
 *    opcode.  0x44, 0x86, or 0x04 here is the TV trying to wake the source.
 * 5. Turn the TV on with the power key only and leave it on the tuner.
 *    Give Physical Address or Give Power Status can show up from discovery.
 *    An empty wake-opcode log on this step is a property of this TV.
 *
 * If Image View On does not wake the TV and steps 2-4 stay empty, the pin is
 * unwired or the CEC clock does not reach the pad.  Stop.  Do not enter standby.
 *
 * `gxlp sleep` with cecmode 1 or 2 arms the LPC pin listener.  A matching
 * Set Stream Path or Active Source, or a directed Image/Text View On, cold-boots
 * the CK610 and does not post another Image View On.  `gxcec snap` prints the
 * LPC peripheral snapshot (XDATA 0x8000, SFR 0xab) captured when a status bit
 * other than TX-done is set.  That snapshot is diagnostic; wake does not decode it.
 * cecmode at shared XDATA 0x90 selects the LPC engine policy.  ABI is 1.10.
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

	if (argc < 2)
		return CMD_RET_USAGE;
	cmd = argv[1];

	if (!strcmp(cmd, "on")) {
		gx_lpc8051_set_cecmode(1);
		return gx6702_cec_set_mode(1) ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
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
	if (!strcmp(cmd, "poll")) {
		int polled;

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
	   "GX6702 DesignWare HDMI CEC",
	   "on|off|dump|snap|poll|viewon|standby|tx|mode\n"
	   "gxcec on                         - cecmode 1: standby posts 0x36, power-on posts 0x04\n"
	   "gxcec off                        - cecmode 0, and gate the HDMI CEC clock\n"
	   "gxcec mode [0|1|2]               - 1 wakes the TV on box power; 2 standby only\n"
	   "gxcec dump                       - clock, status, address; WKUPCTRL is read only\n"
	   "gxcec snap                       - LPC XDATA 0x8000 snapshot and last GPIO frame\n"
	   "gxcec poll [seconds]             - drain RX (default 15s, 0 drains once)\n"
	   "gxcec viewon                     - LPC engine posts Image View On (0x04)\n"
	   "gxcec standby                    - LPC engine posts System Standby (0x36)\n"
	   "gxcec tx <hex bytes>             - raw frame, header included\n"
	   "Philips 24PHH4000/88 gate, box awake, EasyLink/remote/one-touch On:\n"
	   "  1 viewon with the TV in standby: LPC posts Image View On and the TV wakes\n"
	   "  2 SOURCES then this HDMI device: arrows/OK/digits are 0x44\n"
	   "  3 TV standby is 0x36\n"
	   "  4 Play then SOURCES from TV standby: record 0x44/0x86/0x04\n"
	   "  5 power key only, left on the tuner: empty wake log is normal\n"
	   "Stop if Image View On does not wake the TV and steps 2-4 stay empty.\n"
	   "gxlp sleep after gxcec on cold-boots on 0x86/0x82 with this physical\n"
	   "address, or on a directed 0x04/0x0d.  0x36 does not wake.\n"
	   "gxcec snap shows a non-TX-done LPC status capture.  Wake uses P0.5\n"
	   "timing, not that capture.\n");
