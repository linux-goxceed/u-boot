// SPDX-License-Identifier: GPL-2.0+
/*
 * hd2015 - GX6702 front-panel display via the open LPC 8051 firmware.
 *
 * The 8051 owns the TM1650 bus.  This command only updates the shared mailbox.
 */

#include <command.h>
#include <errno.h>
#include <vsprintf.h>
#include <linux/ctype.h>
#include <linux/string.h>
#include "gx_lpc.h"

static int do_hd2015(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	const char *text = "boot";
	int dry_run = 0;
	int do_write = 0;
	int do_brightness = 0;
	int do_aux = 0;
	int do_clock_display = 0;
	unsigned int brightness = 0;
	unsigned int aux = gx_lpc_get_aux();
	bool clock_display = false;
	int argi = 1;

	while (argi < argc && argv[argi][0] == '-') {
		if (!strcmp(argv[argi], "-n")) {
			dry_run = 1;
		} else if (!strcmp(argv[argi], "-w")) {
			do_write = 1;
		} else if (!strcmp(argv[argi], "-b") && argi + 1 < argc) {
			char *end;
			ulong value = simple_strtoul(argv[++argi], &end, 0);

			if (*end || value > GX_TM1650_BRIGHT_MAX)
				return CMD_RET_USAGE;
			do_brightness = 1;
			brightness = value;
		} else if (!strcmp(argv[argi], "-i") && argi + 1 < argc) {
			char *end;
			ulong value = simple_strtoul(argv[++argi], &end, 0);

			if (*end || value > GX_LPC_AUX_MAX)
				return CMD_RET_USAGE;
			do_aux = 1;
			aux = value;
		} else if ((!strcmp(argv[argi], "-c") ||
			    !strcmp(argv[argi], "-s") ||
			    !strcmp(argv[argi], "-p")) && argi + 1 < argc) {
			char option = argv[argi][1];
			char *end;
			ulong value = simple_strtoul(argv[++argi], &end, 0);
			unsigned int bit;

			if (*end || value > 1)
				return CMD_RET_USAGE;
			if (option == 'c')
				bit = GX_LPC_AUX_CLOCK_COLON;
			else if (option == 's')
				bit = GX_LPC_AUX_STATUS_GREEN;
			else
				bit = GX_LPC_AUX_POWER;
			if (value)
				aux |= bit;
			else
				aux &= ~bit;
			do_aux = 1;
		} else if (!strcmp(argv[argi], "-k") && argi + 1 < argc) {
			char *end;
			ulong value = simple_strtoul(argv[++argi], &end, 0);

			if (*end || value > 1)
				return CMD_RET_USAGE;
			do_clock_display = 1;
			clock_display = value;
		} else {
			return CMD_RET_USAGE;
		}
		argi++;
	}

	if (argc > argi)
		text = argv[argi];

	if (do_brightness)
		printf("hd2015: brightness %u%s\n", brightness,
		       dry_run ? " (dry run)" : "");
	else if (do_aux)
		printf("hd2015: AUX mask %x%s\n", aux,
		       dry_run ? " (dry run)" : "");
	else if (do_clock_display)
		printf("hd2015: clock display %u%s\n", clock_display,
		       dry_run ? " (dry run)" : "");
	else
		printf("hd2015: text='%s'%s\n", text,
		       dry_run ? " (dry run)" : "");

	if (dry_run)
		return CMD_RET_SUCCESS;

	if (gx_lpc_ensure_open(text))
		return CMD_RET_FAILURE;

	if (do_clock_display)
		return gx_lpc8051_set_rtc_display(clock_display) ?
			CMD_RET_FAILURE : CMD_RET_SUCCESS;
	if (do_aux) {
		gx_lpc8051_set_aux(aux);
		printf("hd2015: queued AUX mask %x\n", aux);
		return CMD_RET_SUCCESS;
	}
	if (do_brightness) {
		gx_lpc8051_set_brightness(brightness);
		printf("hd2015: queued brightness %u\n", brightness);
		return CMD_RET_SUCCESS;
	}
	if (do_write || argc > 1) {
		gx_lpc8051_write_text(text);
		printf("hd2015: queued '%s'%s\n", text,
		       gx_lpc_get_scroll_length() > 4 ? " (scrolling)" : "");
		return CMD_RET_SUCCESS;
	}

	/* Bare "hd2015" with no args: ensure LPC and show default text. */
	gx_lpc8051_write_text(text);
	printf("hd2015: queued '%s'\n", text);
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(hd2015, 6, 0, do_hd2015,
	   "GX6702 front-panel display (open LPC 8051)",
	   "[-n] [text]\n"
	   "hd2015 -w [text]\n"
	   "hd2015 -b <0..8>\n"
	   "hd2015 -i <0..15>\n"
	   "hd2015 -c|-s|-p <0|1>\n"
	   "hd2015 -k <0|1>\n"
	   "    Auto-starts open LPC firmware if needed (GxLoader-like).\n"
	   "    -w: queue text ( >4 chars scroll).\n"
	   "    -b: brightness 0=off .. 8=max.\n"
	   "    -i: raw AUX0..AUX3 mask; -c colon; -s status; -p power LED.\n"
	   "    -k: disable/enable RTC clock-display mode.\n"
	   "    Sleep/RTC/alarm: see 'gxlp'.\n");
