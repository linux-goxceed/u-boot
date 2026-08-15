// SPDX-License-Identifier: GPL-2.0+
/*
 * gxlp - GX6702 LPC timekeeping, soft standby, and LPC diagnostics.
 */

#include <command.h>
#include <errno.h>
#include <vsprintf.h>
#include <linux/ctype.h>
#include <linux/string.h>
#include "gx_lpc.h"

static int do_gxlp_rtc(int argc, char *const argv[])
{
	u8 hour, minute, second;

	if (gx_lpc_ensure_open("boot"))
		return CMD_RET_FAILURE;
	if (argc == 0)
		return gx_lpc8051_print_rtc() ?
			CMD_RET_FAILURE : CMD_RET_SUCCESS;
	if (argc != 1 || gx_lpc8051_parse_time(argv[0], &hour, &minute, &second))
		return CMD_RET_USAGE;
	printf("gxlp: RTC set %02u:%02u:%02u\n", hour, minute, second);
	return gx_lpc8051_set_rtc(hour, minute, second) ?
		CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

static int do_gxlp_alarm(int argc, char *const argv[])
{
	u8 hour, minute, second;

	if (argc != 1)
		return CMD_RET_USAGE;
	if (gx_lpc_ensure_open("boot"))
		return CMD_RET_FAILURE;
	if (!strcmp(argv[0], "off")) {
		printf("gxlp: alarm cancel\n");
		return gx_lpc8051_cancel_alarm() ?
			CMD_RET_FAILURE : CMD_RET_SUCCESS;
	}
	if (gx_lpc8051_parse_time(argv[0], &hour, &minute, &second))
		return CMD_RET_USAGE;
	printf("gxlp: alarm set %02u:%02u:%02u\n", hour, minute, second);
	return gx_lpc8051_set_alarm(hour, minute, second) ?
		CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

static int do_gxlp_sleep(int argc, char *const argv[])
{
	u32 wake_seconds = 0;
	u32 after_seconds = 0;
	bool arm_alarm = false;
	bool delayed = false;
	u8 ah = 0, am = 0, as = 0;

	if (argc < 1)
		return CMD_RET_USAGE;

	if (!strcmp(argv[0], "button")) {
		if (argc >= 2) {
			char *end;
			ulong value = simple_strtoul(argv[1], &end, 0);

			if (*end || value > 0xff)
				return CMD_RET_USAGE;
			gx_lpc_wake_key = value;
		}
	} else if (!strcmp(argv[0], "until") && argc >= 2) {
		arm_alarm = true;
		if (gx_lpc8051_parse_time(argv[1], &ah, &am, &as))
			return CMD_RET_USAGE;
	} else if (!strcmp(argv[0], "for") && argc >= 2) {
		char *end;
		ulong value = simple_strtoul(argv[1], &end, 0);

		if (*end || value < GX_LPC_SUSPEND_MIN_SECONDS || value > 86400)
			return CMD_RET_USAGE;
		wake_seconds = value;
	} else if (!strcmp(argv[0], "after") && argc >= 2) {
		char *end;
		ulong value = simple_strtoul(argv[1], &end, 0);

		if (*end || value < GX_LPC_SUSPEND_MIN_SECONDS || value > 86400)
			return CMD_RET_USAGE;
		delayed = true;
		after_seconds = value;
	} else {
		return CMD_RET_USAGE;
	}

	if (gx_lpc_ensure_open("boot"))
		return CMD_RET_FAILURE;

	if (delayed)
		return gx_lpc8051_suspend_after(after_seconds) ?
			CMD_RET_FAILURE : CMD_RET_SUCCESS;
	return gx_lpc8051_suspend_soft(wake_seconds, arm_alarm, ah, am, as) ?
		CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

static int do_gxlp_start(int argc, char *const argv[])
{
	const char *text = argc ? argv[0] : "boot";

	return gx_lpc8051_start(gxopen_fw, gxopen_fw_end - gxopen_fw,
				"embedded open gx6702-lpc.bin", text) ?
		CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

static int do_gxlp_load(int argc, char *const argv[])
{
	char *end;
	ulong addr, size;
	const char *text = "boot";

	if (argc < 2)
		return CMD_RET_USAGE;
	addr = simple_strtoul(argv[0], &end, 16);
	if (*end)
		return CMD_RET_USAGE;
	size = simple_strtoul(argv[1], &end, 16);
	if (*end)
		return CMD_RET_USAGE;
	if (argc >= 3)
		text = argv[2];
	printf("gxlp: DIAGNOSTIC external LPC load @%lx size %lx\n", addr, size);
	return gx_lpc8051_start((const u8 *)addr, size, "external memory", text) ?
		CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

static int do_gxlp_diag(int argc, char *const argv[])
{
	if (argc != 1)
		return CMD_RET_USAGE;
	if (!strcmp(argv[0], "stop"))
		return gx_lpc_ck610_stop_only() ?
			CMD_RET_FAILURE : CMD_RET_SUCCESS;
	if (!strcmp(argv[0], "bit1")) {
		if (gx_lpc_ensure_open("boot"))
			return CMD_RET_FAILURE;
		return gx_lpc8051_suspend_bit1_only() ?
			CMD_RET_FAILURE : CMD_RET_SUCCESS;
	}
	return CMD_RET_USAGE;
}

static int do_gxlp(struct cmd_tbl *cmdtp, int flag, int argc,
		   char *const argv[])
{
	const char *sub;

	if (argc < 2)
		return CMD_RET_USAGE;
	sub = argv[1];
	argc -= 2;
	argv += 2;

	if (!strcmp(sub, "rtc"))
		return do_gxlp_rtc(argc, argv);
	if (!strcmp(sub, "alarm"))
		return do_gxlp_alarm(argc, argv);
	if (!strcmp(sub, "sleep"))
		return do_gxlp_sleep(argc, argv);
	if (!strcmp(sub, "keys")) {
		if (gx_lpc_ensure_open("boot"))
			return CMD_RET_FAILURE;
		return gx_lpc8051_probe_keys() ?
			CMD_RET_FAILURE : CMD_RET_SUCCESS;
	}
	if (!strcmp(sub, "start"))
		return do_gxlp_start(argc, argv);
	if (!strcmp(sub, "load"))
		return do_gxlp_load(argc, argv);
	if (!strcmp(sub, "dump"))
		return gx_lpc8051_dump_shared() ?
			CMD_RET_FAILURE : CMD_RET_SUCCESS;
	if (!strcmp(sub, "diag"))
		return do_gxlp_diag(argc, argv);

	return CMD_RET_USAGE;
}

U_BOOT_CMD(gxlp, 6, 0, do_gxlp,
	   "GX6702 LPC RTC, soft standby, and diagnostics",
	   "rtc [HH:MM[:SS]]\n"
	   "gxlp alarm <HH:MM[:SS]|off>\n"
	   "gxlp sleep button [0xNN]\n"
	   "gxlp sleep until <HH:MM[:SS]>\n"
	   "gxlp sleep for <5..86400>\n"
	   "gxlp sleep after <5..86400>\n"
	   "gxlp keys\n"
	   "gxlp start [text]          (diagnostic: force open LPC load)\n"
	   "gxlp load <addr_hex> <size_hex> [text]  (diagnostic)\n"
	   "gxlp dump                  (diagnostic: shared XDATA)\n"
	   "gxlp diag stop|bit1        (diagnostic: STOP probes)\n"
	   "    Product path auto-starts open LPC if needed.\n");