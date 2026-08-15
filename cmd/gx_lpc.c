// SPDX-License-Identifier: GPL-2.0+
/*
 * GX6702 LPC / always-on 8051 shared helpers for hd2015 and gxlp.
 */

#include <command.h>
#include <cpu_func.h>
#include <errno.h>
#include <vsprintf.h>
#include <wdt.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/ctype.h>
#include <linux/string.h>
#include <linux/bitops.h>
#include "gx_lpc.h"

#define GX_GPIO_RAW0		0xA0305000UL
#define GX_GPIO_RAW1		0xA0306000UL
#define GX_GPIO_RAW2		0xA0307000UL
#define GX_GPIO_DIR_OUT		0x04
#define GX_GPIO_DIR_IN		0x08
#define GX_GPIO_OUT_HI		0x14
#define GX_GPIO_OUT_LO		0x18
#define GX_PANEL_PWR		0xA0102000UL
#define GX_PANEL_GPIO		0xA0102008UL
#define GX_PANEL_PWR_MAGIC	0x6770696fUL
#define GX_LPC_CTRL		(GX_LPC_SHARED + 0x300)
#define GX_LPC_CODE_PORT	(GX_LPC_SHARED + 0x304)
#define GX_LPC_SHARED_SIZE	0x98
#define GX_LPC_FW_MAX_SIZE	0x2000
#define GX_LPC_SHARED_MAGIC	0x54410003UL
#define GX_LPC_CLOCK_HZ		24000000UL
#define GX_LPC_CTRL_RUN		BIT(0)
#define GX_LPC_CTRL_RESET	BIT(4)
#define GX_LPC_CTRL_ENABLE	BIT(16)
#define GX_LPC_PANEL_CLK	13
#define GX_LPC_PANEL_DAT	14
#define GX_LPC_MAILBOX_OFF	0x100
#define GX_LPC_STATUS_OFF	0x108
#define GX_LPC_SCROLL_OFF	0x110
#define GX_LPC_RTC_COMMAND_OFF	0x134
#define GX_LPC_RTC_STATE_OFF	0x13c
#define GX_LPC_ALARM_COMMAND_OFF	0x144
#define GX_LPC_ALARM_STATE_OFF	0x14c
#define GX_LPC_SUSPEND_COMMAND_OFF 0x154
#define GX_LPC_SUSPEND_STATE_OFF	0x15c
#define GX_LPC_LAST_KEY_OFF	0x10e
#define GX_LPC_LAST_IR_LO_OFF	0x10f
#define GX_LPC_LAST_IR_HI_OFF	0x164
#define GX_CK610_STOP_ENTRY	0x00100400UL
#define GX_WDT_BASE		0xA020B000UL
#define GX_WDT_CTRL		0x00
#define GX_WDT_RESTART		0x0c
#define GX_WDT_CTRL_ENABLE	BIT(0)
#define GX_WDT_CTRL_RESET	BIT(1)
#define GX_TM1650_CTRL_ON	BIT(0)
#define GX_TM1650_BRIGHT_SHIFT	4
#define HD2015_DIGITS		4
#define HD2015_DOT		0x80

static void gx_panel_raw_gpio_apply(void);
static void gx_raw_dir_output(unsigned int hw);
static void gx_raw_dir_input(unsigned int hw);
static void gx_raw_set_value(unsigned int hw, int val);
static void __iomem *gx_raw_gpio_bank(unsigned int pin, unsigned int *bit);

/* ---- from hd2015.c:257-261 ---- */
static const u32 gx_panel_pin_table[] = {
	0x00010e00, 0x00070d01, 0x00070b03, 0x0005190f,
	0x00070c10, 0x00051811, 0x00050f12, 0x00051013,
	0x0000ffff,
};

/* ---- from hd2015.c:334-371 ---- */
static u8 gx_lpc_seg_for_char(char c)
{
	switch (tolower(c)) {
	case '0': return 0x3f;
	case '1': return 0x06;
	case '2': return 0x5b;
	case '3': return 0x4f;
	case '4': return 0x66;
	case '5': return 0x6d;
	case '6': return 0x7d;
	case '7': return 0x07;
	case '8': return 0x7f;
	case '9': return 0x6f;
	case 'a': return 0x5f;
	case 'b': return 0x7c;
	case 'c': return 0x58;
	case 'd': return 0x5e;
	case 'e': return 0x79;
	case 'f': return 0x71;
	case 'g': return 0x6f;
	case 'h': return 0x74;
	case 'i': return 0x04;
	case 'j': return 0x0e;
	case 'l': return 0x38;
	case 'n': return 0x54;
	case 'o': return 0x5c;
	case 't': return 0x78;
	case 'p': return 0x73;
	case 'q': return 0x67;
	case 'r': return 0x50;
	case 's': return 0x6d;
	case 'u': return 0x1c;
	case 'y': return 0x6e;
	case '-': return 0x40;
	case ' ': return 0x00;
	default:  return 0x40;
	}
}

/* ---- from hd2015.c:540-566 ---- */
static void gx_panel_power_init(void)
{
	void __iomem *ctl = (void __iomem *)GX_PANEL_PWR;
	void __iomem *gpio = (void __iomem *)GX_PANEL_GPIO;
	int i, term = 0;

	for (i = 0; i < ARRAY_SIZE(gx_panel_pin_table); i++) {
		const u32 w = gx_panel_pin_table[i];
		u8 b0 = w & 0xff;
		u8 b1 = (w >> 8) & 0xff;

		if ((b0 & b1) == 0xff) {
			term = i;
			break;
		}
	}

	writel(GX_PANEL_PWR_MAGIC, ctl);
	writeb(term, ctl + 5);
	writeb(term ? 1 : 0, ctl + 4);

	/* Vendor copies the valid records, not the terminator. */
	for (i = 0; i < term; i++)
		writel(gx_panel_pin_table[i], gpio + i * 4);

	gx_panel_raw_gpio_apply();
}

/* ---- from hd2015.c:575-1314 ---- */
/* Keep AUX/brightness-only commands deterministic before the first -w. */
static u8 gx_lpc_segments[HD2015_DIGITS] = {
	0x7c, 0x5c, 0x5c, 0x78,	/* "boot" */
};

static u8 gx_lpc_scroll_data[GX_LPC_SCROLL_MAX] = {
	0x7c, 0x5c, 0x5c, 0x78,	/* "boot" */
};

unsigned int gx_lpc_scroll_length = HD2015_DIGITS;
unsigned int gx_lpc_brightness = GX_TM1650_BRIGHT_MAX;
unsigned int gx_lpc_aux;
static u8 gx_lpc_rtc_control;
static u8 gx_lpc_rtc_set_sequence;
static u8 gx_lpc_rtc_set_hour;
static u8 gx_lpc_rtc_set_minute;
static u8 gx_lpc_rtc_set_second;
static bool gx_lpc_rtc_cache_valid;
static u8 gx_lpc_alarm_control;
static u8 gx_lpc_alarm_set_sequence;
static u8 gx_lpc_alarm_set_hour;
static u8 gx_lpc_alarm_set_minute;
static u8 gx_lpc_alarm_set_second;
static bool gx_lpc_alarm_cache_valid;
static u8 gx_lpc_suspend_control;
static u8 gx_lpc_suspend_sequence;
/* Soft-standby RTC wake countdown; 0 with WAKE_ALARM uses armed absolute alarm. */
static u32 gx_lpc_suspend_seconds;
/* 0 = LPC default (TM1650 DIG4/KI2 = 0x4f); else exact scan code for power. */
u8 gx_lpc_wake_key;
static bool gx_lpc_suspend_cache_valid;

static u8 gx_lpc8051_control(void)
{
	if (!gx_lpc_brightness)
		return 0;

	/* TM1650 encodes maximum (step 8) with a zero brightness field. */
	if (gx_lpc_brightness == GX_TM1650_BRIGHT_MAX)
		return GX_TM1650_CTRL_ON;

	return GX_TM1650_CTRL_ON |
		(gx_lpc_brightness << GX_TM1650_BRIGHT_SHIFT);
}

void gx_lpc8051_publish(void)
{
	void __iomem *mailbox =
		(void __iomem *)(GX_LPC_SHARED + GX_LPC_MAILBOX_OFF);
	void __iomem *scroll =
		(void __iomem *)(GX_LPC_SHARED + GX_LPC_SCROLL_OFF);
	u32 word0, word1, meta, word;
	u32 rtc_word0, rtc_word1, alarm_word0, alarm_word1;
	u32 suspend_word0;
	int i;

	for (i = 0; i < GX_LPC_SCROLL_MAX; i += sizeof(u32)) {
		word = (u32)gx_lpc_scroll_data[i] |
			(u32)gx_lpc_scroll_data[i + 1] << 8 |
			(u32)gx_lpc_scroll_data[i + 2] << 16 |
			(u32)gx_lpc_scroll_data[i + 3] << 24;
		writel(word, scroll + 4 + i);
	}
	meta = gx_lpc_scroll_length |
		(gx_lpc_scroll_length > HD2015_DIGITS ?
		 GX_LPC_SCROLL_ENABLE << 8 : 0) |
		GX_LPC_SCROLL_PERIOD << 16;
	writel(meta, scroll);
	rtc_word0 = gx_lpc_rtc_control |
		(u32)gx_lpc_rtc_set_sequence << 8 |
		(u32)gx_lpc_rtc_set_hour << 16 |
		(u32)gx_lpc_rtc_set_minute << 24;
	rtc_word1 = gx_lpc_rtc_set_second;
	writel(rtc_word0, (void __iomem *)(GX_LPC_SHARED +
						 GX_LPC_RTC_COMMAND_OFF));
	writel(rtc_word1, (void __iomem *)(GX_LPC_SHARED +
						 GX_LPC_RTC_COMMAND_OFF + 4));
	alarm_word0 = gx_lpc_alarm_control |
		(u32)gx_lpc_alarm_set_sequence << 8 |
		(u32)gx_lpc_alarm_set_hour << 16 |
		(u32)gx_lpc_alarm_set_minute << 24;
	alarm_word1 = gx_lpc_alarm_set_second;
	writel(alarm_word0, (void __iomem *)(GX_LPC_SHARED +
						   GX_LPC_ALARM_COMMAND_OFF));
	writel(alarm_word1, (void __iomem *)(GX_LPC_SHARED +
						   GX_LPC_ALARM_COMMAND_OFF + 4));
	suspend_word0 = gx_lpc_suspend_control |
		(u32)gx_lpc_suspend_sequence << 8;
	if (gx_lpc_suspend_control) {
		suspend_word0 |= (u32)GX_LPC_SUSPEND_GUARD0 << 16 |
			(u32)GX_LPC_SUSPEND_GUARD1 << 24;
	}
	writel(suspend_word0, (void __iomem *)(GX_LPC_SHARED +
						 GX_LPC_SUSPEND_COMMAND_OFF));
	writel(gx_lpc_wake_key, (void __iomem *)(GX_LPC_SHARED +
						 GX_LPC_SUSPEND_COMMAND_OFF + 4));
	/* 0x160: soft-standby wake countdown (little-endian); LPC may echo it. */
	writel(gx_lpc_suspend_seconds, (void __iomem *)(GX_LPC_SHARED +
						 GX_LPC_SUSPEND_COMMAND_OFF + 0xc));

	/* This shared window requires aligned CK610 stores.  Publish flag last. */
	word0 = (u32)gx_lpc_segments[0] << 8 |
		(u32)gx_lpc_segments[1] << 16 |
		(u32)gx_lpc_segments[2] << 24;
	word1 = (u32)gx_lpc_segments[3] |
		(u32)gx_lpc8051_control() << 8 |
		(u32)gx_lpc_aux << 16;
	writel(word0, mailbox);
	writel(word1, mailbox + 4);
	writel(word0 | 1, mailbox);
}

static void gx_lpc8051_adopt_rtc_command(void);

void gx_lpc8051_write_text(const char *text)
{
	int i, n = 0;

	gx_lpc8051_adopt_rtc_command();
	memset(gx_lpc_segments, 0, sizeof(gx_lpc_segments));
	memset(gx_lpc_scroll_data, 0, sizeof(gx_lpc_scroll_data));
	for (i = 0; text[i] && n < GX_LPC_SCROLL_MAX; i++) {
		if (text[i] == '.') {
			if (n > 0)
				gx_lpc_scroll_data[n - 1] |= HD2015_DOT;
			continue;
		}
		gx_lpc_scroll_data[n++] = gx_lpc_seg_for_char(text[i]);
	}
	for (i = 0; i < HD2015_DIGITS; i++)
		gx_lpc_segments[i] = gx_lpc_scroll_data[i];
	gx_lpc_scroll_length = n;
	/* An explicit text command leaves clock-display mode. */
	gx_lpc_rtc_control &= ~GX_LPC_RTC_DISPLAY;

	gx_lpc8051_publish();
}

bool gx_lpc8051_open_ready(u8 required_caps)
{
	u32 status = readl((void __iomem *)(GX_LPC_SHARED + GX_LPC_STATUS_OFF));
	u8 capabilities = status >> 24;

	if ((status & 0xff) != GX_LPC_STATUS_READY ||
	    ((status >> 8) & 0xff) != GX_LPC_ABI_MAJOR ||
	    (capabilities & required_caps) != required_caps)
		return false;
	return true;
}

static void gx_lpc8051_adopt_rtc_command(void)
{
	u32 word0;
	u32 word1;

	if (gx_lpc_rtc_cache_valid && gx_lpc_alarm_cache_valid &&
	    gx_lpc_suspend_cache_valid)
		return;
	if (!gx_lpc_rtc_cache_valid &&
	    gx_lpc8051_open_ready(GX_LPC_CAP_RTC)) {
		word0 = readl((void __iomem *)(GX_LPC_SHARED +
						    GX_LPC_RTC_COMMAND_OFF));
		word1 = readl((void __iomem *)(GX_LPC_SHARED +
						    GX_LPC_RTC_COMMAND_OFF + 4));
		gx_lpc_rtc_control = word0;
		gx_lpc_rtc_set_sequence = word0 >> 8;
		gx_lpc_rtc_set_hour = word0 >> 16;
		gx_lpc_rtc_set_minute = word0 >> 24;
		gx_lpc_rtc_set_second = word1;
	}
	if (!gx_lpc_rtc_cache_valid)
		gx_lpc_rtc_cache_valid = true;

	if (!gx_lpc_alarm_cache_valid &&
	    gx_lpc8051_open_ready(GX_LPC_CAP_ALARM)) {
		word0 = readl((void __iomem *)(GX_LPC_SHARED +
						    GX_LPC_ALARM_COMMAND_OFF));
		word1 = readl((void __iomem *)(GX_LPC_SHARED +
						    GX_LPC_ALARM_COMMAND_OFF + 4));
		gx_lpc_alarm_control = word0;
		gx_lpc_alarm_set_sequence = word0 >> 8;
		gx_lpc_alarm_set_hour = word0 >> 16;
		gx_lpc_alarm_set_minute = word0 >> 24;
		gx_lpc_alarm_set_second = word1;
	}
	if (!gx_lpc_alarm_cache_valid)
		gx_lpc_alarm_cache_valid = true;

	if (!gx_lpc_suspend_cache_valid &&
	    gx_lpc8051_open_ready(GX_LPC_CAP_SUSPEND)) {
		word0 = readl((void __iomem *)(GX_LPC_SHARED +
						    GX_LPC_SUSPEND_COMMAND_OFF));
		gx_lpc_suspend_control = word0;
		gx_lpc_suspend_sequence = word0 >> 8;
	}
	if (!gx_lpc_suspend_cache_valid)
		gx_lpc_suspend_cache_valid = true;
}

int gx_lpc8051_parse_time(const char *text, u8 *hour, u8 *minute,
			  u8 *second)
{
	char *end;
	ulong value;

	value = simple_strtoul(text, &end, 10);
	if (end == text || *end != ':' || value > 23)
		return -EINVAL;
	*hour = value;
	text = end + 1;
	value = simple_strtoul(text, &end, 10);
	if (end == text || value > 59)
		return -EINVAL;
	*minute = value;
	if (!*end) {
		*second = 0;
		return 0;
	}
	if (*end != ':')
		return -EINVAL;
	text = end + 1;
	value = simple_strtoul(text, &end, 10);
	if (end == text || *end || value > 59)
		return -EINVAL;
	*second = value;
	return 0;
}

int gx_lpc8051_print_rtc(void)
{
	void __iomem *state =
		(void __iomem *)(GX_LPC_SHARED + GX_LPC_RTC_STATE_OFF);
	void __iomem *alarm =
		(void __iomem *)(GX_LPC_SHARED + GX_LPC_ALARM_STATE_OFF);
	u32 clock, tail0, tail1, alarm_state = 0, alarm_tail = 0;
	u8 generation;
	int retries;

	if (!gx_lpc8051_open_ready(GX_LPC_CAP_RTC)) {
		printf("gxlpc: RTC-capable open LPC firmware is not running\n");
		return -ENODEV;
	}
	for (retries = 0; retries < 8; retries++) {
		tail0 = readl(state + 4);
		generation = tail0 >> 24;
		if (generation & 1)
			continue;
		clock = readl(state);
		if (gx_lpc8051_open_ready(GX_LPC_CAP_ALARM)) {
			alarm_state = readl(alarm);
			alarm_tail = readl(alarm + 4);
		}
		tail1 = readl(state + 4);
		if (tail0 == tail1)
			break;
	}
	if (retries == 8) {
		printf("gxlpc: RTC snapshot remained busy\n");
		return -EAGAIN;
	}
	printf("gxlpc: RTC %02u:%02u:%02u day=%u running=%u display=%u valid=%u set-ack=%u\n",
	       clock & 0xff, (clock >> 8) & 0xff, (clock >> 16) & 0xff,
	       tail1 & 0xffff,
	       !!((clock >> 24) & GX_LPC_RTC_STATUS_RUNNING),
	       !!((clock >> 24) & GX_LPC_RTC_STATUS_DISPLAY),
	       !!((clock >> 24) & GX_LPC_RTC_STATUS_TIME_VALID),
	       (tail1 >> 16) & 0xff);
	if (gx_lpc8051_open_ready(GX_LPC_CAP_ALARM))
		printf("gxlpc: alarm %02u:%02u:%02u armed=%u active=%u set-ack=%u triggers=%u\n",
		       (alarm_state >> 8) & 0xff,
		       (alarm_state >> 16) & 0xff,
		       (alarm_state >> 24) & 0xff,
		       !!((alarm_state & 0xff) & GX_LPC_ALARM_STATUS_ARMED),
		       !!((alarm_state & 0xff) & GX_LPC_ALARM_STATUS_ACTIVE),
		       alarm_tail & 0xff, (alarm_tail >> 8) & 0xff);
	return 0;
}

int gx_lpc8051_dump_shared(void)
{
	void __iomem *shared = (void __iomem *)GX_LPC_SHARED;
	unsigned int off;

	/*
	 * Shared XDATA is safe to read while the LPC core is stopped or running;
	 * unlike GX_LPC_CTRL/code-port reads, this is normal eCos ABI memory.
	 * Do not write anything here: this command is intended to preserve and
	 * inspect a stock standby snapshot after a warm CK610-only reset.
	 */
	printf("gxlpc: read-only stock shared-XDATA snapshot\n");
	for (off = 0; off + 16 <= GX_LPC_SHARED_SIZE; off += 16)
		printf("  +%02x: %08x %08x %08x %08x\n", off,
		       readl(shared + off), readl(shared + off + 4),
		       readl(shared + off + 8), readl(shared + off + 12));
	if (off < GX_LPC_SHARED_SIZE)
		printf("  +%02x: %08x %08x\n", off,
		       readl(shared + off), readl(shared + off + 4));
	return 0;
}

int gx_lpc8051_set_rtc(u8 hour, u8 minute, u8 second)
{
	gx_lpc8051_adopt_rtc_command();
	gx_lpc_rtc_set_sequence++;
	gx_lpc_rtc_set_hour = hour;
	gx_lpc_rtc_set_minute = minute;
	gx_lpc_rtc_set_second = second;
	gx_lpc_rtc_control |= GX_LPC_RTC_TIME_VALID | GX_LPC_RTC_DISPLAY;
	gx_lpc8051_publish();
	mdelay(20);
	return gx_lpc8051_print_rtc();
}

int gx_lpc8051_set_rtc_display(bool enable)
{
	gx_lpc8051_adopt_rtc_command();
	if (enable)
		gx_lpc_rtc_control |= GX_LPC_RTC_DISPLAY;
	else
		gx_lpc_rtc_control &= ~GX_LPC_RTC_DISPLAY;
	gx_lpc8051_publish();
	mdelay(20);
	return gx_lpc8051_print_rtc();
}

int gx_lpc8051_set_alarm(u8 hour, u8 minute, u8 second)
{
	gx_lpc8051_adopt_rtc_command();
	gx_lpc_alarm_set_sequence++;
	gx_lpc_alarm_set_hour = hour;
	gx_lpc_alarm_set_minute = minute;
	gx_lpc_alarm_set_second = second;
	gx_lpc_alarm_control = GX_LPC_ALARM_ARMED;
	gx_lpc8051_publish();
	mdelay(20);
	return gx_lpc8051_print_rtc();
}

int gx_lpc8051_cancel_alarm(void)
{
	gx_lpc8051_adopt_rtc_command();
	gx_lpc_alarm_set_sequence++;
	gx_lpc_alarm_control = 0;
	gx_lpc8051_publish();
	mdelay(20);
	return gx_lpc8051_print_rtc();
}

/*
 * eCos 0x900f15b0: psrclr; clear CR18 bits 0/1; jsr 0x00100400; br .
 * Architectural STOP must run from ISRAM (DDR-resident STOP does not hold).
 * The readable sequence lives in gx6702_ck610_lpc.S and is copied to the
 * eCos low-power entry at 0x00100400 here.
 */
static void gx_lpc_ck610_prepare_entry(const u8 *stub, unsigned int stub_len)
{
	memcpy((void *)GX_CK610_STOP_ENTRY, stub, stub_len);
	flush_cache(GX_CK610_STOP_ENTRY, stub_len);
}

static void gx_lpc_ck610_prepare_stop(void)
{
	gx_lpc_ck610_prepare_entry(gx6702_ck610_stop_blob,
				   gx6702_ck610_stop_blob_end -
				   gx6702_ck610_stop_blob);
}

static void gx_lpc_ck610_enter_stop(void)
{
	/* Whole-cache wbinv so the ISRAM body is fetch-coherent. */
	{
		unsigned int op = (1u << 0) | (1u << 1) | (1u << 4) | (1u << 5);

		asm volatile("mtcr %0, cr17" : : "r"(op) : "memory");
		asm volatile("sync" ::: "memory");
		asm volatile("idly4" ::: "memory");
	}

	gx6702_ck610_enter_isram();

	for (;;)
		;
}

static void gx_lpc_ck610_stop_watchdog(void)
{
	void __iomem *wdt = (void __iomem *)GX_WDT_BASE;
	u32 ctrl;

	/* Prefer the DM helper; fall back to the recovered MMIO sequence. */
	if (wdt_stop_all() == 0)
		return;

	writel(0, wdt + GX_WDT_RESTART);
	ctrl = readl(wdt + GX_WDT_CTRL);
	ctrl &= ~(GX_WDT_CTRL_ENABLE | GX_WDT_CTRL_RESET);
	writel(ctrl, wdt + GX_WDT_CTRL);
}

static void gx_lpc_ck610_handoff_stop(bool quiet)
{
	u32 entry0 = readl((void __iomem *)GX_CK610_STOP_ENTRY);

	gx_lpc_ck610_stop_watchdog();
	if (!quiet) {
		printf("gxlpc: CK610 jsr @00100400 entry=%08x\n", entry0);
		/* Short drain; LPC waits before bit1/bit2. */
		mdelay(20);
	}
	gx_lpc_ck610_enter_stop();
}

/*
 * Soft standby: STOP+bit1, live dimmed clock.  Wake sources:
 *   wake_seconds > 0  — cold-boot after countdown (WAKE_ALARM)
 *   arm_alarm         — cold-boot at HH:MM:SS (WAKE_ALARM, seconds=0)
 *   otherwise         — power key / IR only
 */
int gx_lpc8051_suspend_soft(u32 wake_seconds, bool arm_alarm,
			    u8 alarm_hour, u8 alarm_minute,
			    u8 alarm_second)
{
	void __iomem *suspend =
		(void __iomem *)(GX_LPC_SHARED + GX_LPC_SUSPEND_STATE_OFF);
	u32 suspend_state;

	gx_lpc8051_adopt_rtc_command();
	if (!gx_lpc8051_open_ready(GX_LPC_CAP_SUSPEND)) {
		printf("gxlpc: ABI %u.%u suspend-capable open LPC firmware is not running\n",
		       GX_LPC_ABI_MAJOR, GX_LPC_ABI_MINOR);
		return -ENODEV;
	}
	if ((wake_seconds || arm_alarm) &&
	    !gx_lpc8051_open_ready(GX_LPC_CAP_ALARM | GX_LPC_CAP_RTC)) {
		printf("gxlpc: ABI %u.%u RTC/alarm-capable open LPC firmware is not running\n",
		       GX_LPC_ABI_MAJOR, GX_LPC_ABI_MINOR);
		return -ENODEV;
	}
	if (wake_seconds && wake_seconds < GX_LPC_SUSPEND_MIN_SECONDS) {
		printf("gxlpc: wake countdown must be 0 or >= %u seconds\n",
		       GX_LPC_SUSPEND_MIN_SECONDS);
		return -EINVAL;
	}

	printf("gxlpc: DESTRUCTIVE SOFT STANDBY\n");
	printf("gxlpc: CK610 RAM and this U-Boot session will be lost\n");
	printf("gxlpc: STOP+bit1 (dim clock; IR/power-key cold-boot, key=0x%02x)\n",
	       gx_lpc_wake_key ? gx_lpc_wake_key : GX_LPC_TM1650_KEY_POWER);
	if (wake_seconds)
		printf("gxlpc: RTC wake in %u seconds\n", wake_seconds);
	else if (arm_alarm)
		printf("gxlpc: RTC wake at %02u:%02u:%02u\n",
		       alarm_hour, alarm_minute, alarm_second);
	else
		printf("gxlpc: press power key or IR power to cold-boot\n");

	gx_lpc_ck610_prepare_stop();
	gx_lpc_suspend_sequence++;
	gx_lpc_suspend_seconds = wake_seconds;
	gx_lpc_suspend_control = GX_LPC_SUSPEND_REQUEST |
		GX_LPC_SUSPEND_NO_POWEROFF;
	if (wake_seconds || arm_alarm)
		gx_lpc_suspend_control |= GX_LPC_SUSPEND_WAKE_ALARM;
	if (arm_alarm) {
		gx_lpc_alarm_set_sequence++;
		gx_lpc_alarm_set_hour = alarm_hour;
		gx_lpc_alarm_set_minute = alarm_minute;
		gx_lpc_alarm_set_second = alarm_second;
		gx_lpc_alarm_control = GX_LPC_ALARM_ARMED;
	}
	gx_lpc8051_publish();
	gx_lpc_ck610_handoff_stop(false);

	/* Unreachable if STOP held. */
	suspend_state = readl(suspend);
	if ((suspend_state & 0xff) & GX_LPC_SUSPEND_STATUS_REJECTED)
		printf("gxlpc: 8051 rejected standby: ack=%u error=%u\n",
		       (suspend_state >> 8) & 0xff,
		       (suspend_state >> 16) & 0xff);
	else if ((suspend_state & 0xff) & GX_LPC_SUSPEND_STATUS_PREPARED)
		printf("gxlpc: 8051 prepared standby, but CK610 power-off did not take effect\n");
	else
		printf("gxlpc: 8051 did not acknowledge standby (state=%08x)\n",
		       suspend_state);
	return -EIO;
}

int gx_lpc8051_suspend_button(void)
{
	return gx_lpc8051_suspend_soft(0, false, 0, 0, 0);
}

int gx_lpc8051_suspend_after(u32 delay_seconds)
{
	u32 left = delay_seconds;

	printf("gxlpc: entering soft standby in %u seconds\n", delay_seconds);
	while (left) {
		mdelay(1000);
		left--;
		if (left <= 10 || (left % 10) == 0)
			printf("gxlpc: %u...\n", left);
	}
	return gx_lpc8051_suspend_button();
}

/* Discriminator: CK610 STOP only — no LPC suspend / SFR bit2. */
int gx_lpc_ck610_stop_only(void)
{
	printf("gxlpc: CK610 STOP-only (no LPC power-off)\n");
	printf("gxlpc: pass = UART silent, no GxLoader for >=30s\n");
	printf("gxlpc: fail = rebound without button (STOP/CR18 path itself resets)\n");
	gx_lpc_ck610_prepare_stop();
	gx_lpc_ck610_handoff_stop(false);
	printf("gxlpc: STOP returned unexpectedly\n");
	return -EIO;
}

/* Discriminator: STOP, then LPC bit1 only — never bit2. */
int gx_lpc8051_suspend_bit1_only(void)
{
	gx_lpc8051_adopt_rtc_command();
	if (!gx_lpc8051_open_ready(GX_LPC_CAP_SUSPEND)) {
		printf("gxlpc: ABI %u.%u suspend-capable open LPC firmware is not running\n",
		       GX_LPC_ABI_MAJOR, GX_LPC_ABI_MINOR);
		return -ENODEV;
	}

	printf("gxlpc: STOP + LPC bit1 only (no SFR bit2)\n");
	printf("gxlpc: hold = bit2 is the rebound; rebound = bit1 breaks STOP\n");
	gx_lpc_ck610_prepare_stop();
	gx_lpc_suspend_sequence++;
	gx_lpc_suspend_control = GX_LPC_SUSPEND_REQUEST | GX_LPC_SUSPEND_NO_POWEROFF;
	gx_lpc8051_publish();
	gx_lpc_ck610_handoff_stop(false);
	printf("gxlpc: STOP returned unexpectedly\n");
	return -EIO;
}

void gx_lpc8051_set_brightness(unsigned int brightness)
{
	gx_lpc8051_adopt_rtc_command();
	gx_lpc_brightness = brightness;
	gx_lpc8051_publish();
}

void gx_lpc8051_set_aux(unsigned int aux)
{
	gx_lpc8051_adopt_rtc_command();
	gx_lpc_aux = aux;
	gx_lpc8051_publish();
}

int gx_lpc8051_start(const u8 *fw, ulong fw_size,
		     const char *source, const char *text)
{
	void __iomem *shared = (void __iomem *)GX_LPC_SHARED;
	u32 ctrl, status;
	u8 byte;
	ulong i;

	if (!fw || !fw_size || fw_size > GX_LPC_FW_MAX_SIZE) {
		printf("gxlpc: invalid 8051 firmware size (%lu, max %x)\n",
		       fw_size, GX_LPC_FW_MAX_SIZE);
		return -EINVAL;
	}

	/*
	 * Do not read this register: a running or malformed LPC image can stall
	 * the CK610 bridge on reads.  Loading is only safe from a cold/stopped LPC
	 * state; these known-value writes also avoid preserving unknown bits.
	 */
	ctrl = GX_LPC_CTRL_ENABLE;
	writel(ctrl, (void __iomem *)GX_LPC_CTRL);
	udelay(10);

	/* Hold the core in reset and enable its low-power domain. */
	ctrl |= GX_LPC_CTRL_RESET;
	writel(ctrl, (void __iomem *)GX_LPC_CTRL);
	udelay(10);

	printf("gxlpc: programming %lu bytes of 8051 code from %s...\n",
	       fw_size, source);
	for (i = 0; i < fw_size; i++) {
		byte = fw[i];
		writel((i << 8) | byte, (void __iomem *)GX_LPC_CODE_PORT);
	}

	/* eCos clears exactly 0x98 bytes before publishing this structure. */
	for (i = 0; i < GX_LPC_SHARED_SIZE; i += sizeof(u32))
		writel(0, shared + i);
	writel(GX_LPC_SHARED_MAGIC, shared + 0x00);
	writel(GX_LPC_CLOCK_HZ, shared + 0x74);
	/* panelio=<clock>,<data>; consumed by firmware function 0x12f5. */
	writel(GX_LPC_PANEL_CLK, shared + 0x78);
	writel(GX_LPC_PANEL_DAT, shared + 0x7c);
	writel(~0U, shared + 0x8c);
	gx_lpc_rtc_control = 0;
	gx_lpc_rtc_set_sequence = 0;
	gx_lpc_rtc_set_hour = 0;
	gx_lpc_rtc_set_minute = 0;
	gx_lpc_rtc_set_second = 0;
	gx_lpc_rtc_cache_valid = true;
	gx_lpc_alarm_control = 0;
	gx_lpc_alarm_set_sequence = 0;
	gx_lpc_alarm_set_hour = 0;
	gx_lpc_alarm_set_minute = 0;
	gx_lpc_alarm_set_second = 0;
	gx_lpc_alarm_cache_valid = true;
	gx_lpc_suspend_control = 0;
	gx_lpc_suspend_sequence = 0;
	gx_lpc_suspend_seconds = 0;
	gx_lpc_wake_key = 0;
	gx_lpc_suspend_cache_valid = true;
	gx_lpc8051_write_text(text);

	/* GxLoader supplies the matching LPC GPIO descriptor table. */
	gx_panel_power_init();

	/* Exact eCos release callback: clear reset bit 4, then set run bit 0. */
	ctrl &= ~GX_LPC_CTRL_RESET;
	writel(ctrl, (void __iomem *)GX_LPC_CTRL);
	ctrl |= GX_LPC_CTRL_RUN;
	writel(ctrl, (void __iomem *)GX_LPC_CTRL);

	/* Publish again in case a new firmware's C runtime touched XDATA. */
	gx_lpc8051_publish();
	mdelay(20);
	/* A4D00304/A4D00308 are not safe to read once the 8051 is running. */
	printf("gxlpc: 8051 panel core released: ctrl=%08x text='%s' brightness=%u aux=%x\n",
	       ctrl, text, gx_lpc_brightness, gx_lpc_aux);
	status = readl(shared + GX_LPC_STATUS_OFF);
	printf("gxlpc: open LPC status=%02x ABI=%u.%u caps=%02x\n",
	       status & 0xff, (status >> 8) & 0xff,
	       (status >> 16) & 0xff, (status >> 24) & 0xff);
	if ((status & 0xff) != GX_LPC_STATUS_READY ||
	    ((status >> 8) & 0xff) != GX_LPC_ABI_MAJOR ||
	    ((status >> 16) & 0xff) != GX_LPC_ABI_MINOR ||
	    ((status >> 24) & (GX_LPC_CAP_DISPLAY |
			 GX_LPC_CAP_BRIGHTNESS |
			 GX_LPC_CAP_AUX |
			 GX_LPC_CAP_SCROLL |
			 GX_LPC_CAP_RTC |
			 GX_LPC_CAP_ALARM |
			 GX_LPC_CAP_SUSPEND)) !=
	    (GX_LPC_CAP_DISPLAY | GX_LPC_CAP_BRIGHTNESS |
	     GX_LPC_CAP_AUX | GX_LPC_CAP_SCROLL |
	     GX_LPC_CAP_RTC | GX_LPC_CAP_ALARM |
	     GX_LPC_CAP_SUSPEND)) {
		printf("gxlpc: open LPC firmware did not report ready\n");
		return -EIO;
	}

	return 0;
}

/* ---- from hd2015.c:1606-1615 ---- */
static void gx_raw_dir_output(unsigned int hw)
{
	void __iomem *base;
	unsigned int bit;

	base = gx_raw_gpio_bank(hw, &bit);
	if (!base)
		return;
	writel(BIT(bit), base + GX_GPIO_DIR_OUT);
}

static void gx_raw_dir_input(unsigned int hw)
{
	void __iomem *base;
	unsigned int bit;

	base = gx_raw_gpio_bank(hw, &bit);
	if (!base)
		return;
	writel(BIT(bit), base + GX_GPIO_DIR_IN);
}

/* ---- from hd2015.c:1632-1645 ---- */
static void gx_raw_set_value(unsigned int hw, int val)
{
	void __iomem *base;
	unsigned int bit;

	base = gx_raw_gpio_bank(hw, &bit);
	if (!base)
		return;

	if (val)
		writel(BIT(bit), base + GX_GPIO_OUT_HI);
	else
		writel(BIT(bit), base + GX_GPIO_OUT_LO);
}

/* ---- from hd2015.c:1929-1944 ---- */
static void __iomem *gx_raw_gpio_bank(unsigned int pin, unsigned int *bit)
{
	if (pin <= 31) {
		*bit = pin;
		return (void __iomem *)GX_GPIO_RAW0;
	}
	if (pin <= 63) {
		*bit = pin - 32;
		return (void __iomem *)GX_GPIO_RAW1;
	}
	if (pin <= 95) {
		*bit = pin - 64;
		return (void __iomem *)GX_GPIO_RAW2;
	}
	return NULL;
}

/* ---- from hd2015.c:2003-2025 ---- */
static void gx_panel_raw_gpio_apply(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(gx_panel_pin_table); i++) {
		const u32 w = gx_panel_pin_table[i];
		u8 b0 = w & 0xff;
		u8 b1 = (w >> 8) & 0xff;
		u8 flags = (w >> 16) & 0xff;

		if ((b0 & b1) == 0xff)
			break;
		if (!(flags & 1))
			continue;

		if (flags & 2) {
			gx_raw_dir_output(b1);
			gx_raw_set_value(b1, !!(flags & 4));
		} else {
			gx_raw_dir_input(b1);
		}
	}
}

/* ---- from hd2015.c:2452-2491 ---- */
int gx_lpc8051_probe_keys(void)
{
	void __iomem *keyp =
		(void __iomem *)(GX_LPC_SHARED + GX_LPC_LAST_KEY_OFF);
	void __iomem *ir_lo =
		(void __iomem *)(GX_LPC_SHARED + GX_LPC_LAST_IR_LO_OFF);
	void __iomem *ir_hi =
		(void __iomem *)(GX_LPC_SHARED + GX_LPC_LAST_IR_HI_OFF);
	u8 last_key = 0xff;
	u16 last_ir = 0xffff;
	int i;

	if (!gx_lpc8051_open_ready(0)) {
		printf("gxlpc: open LPC firmware is not running (hd2015 boot first)\n");
		return -ENODEV;
	}

	printf("gxlpc: probing keys/IR for 8s (power TM1650=0x%02x IR=0059/0101/bfaf/...)\n",
	       GX_LPC_TM1650_KEY_POWER);
	for (i = 0; i < 80; i++) {
		u8 key = readb(keyp);
		u16 ir = readb(ir_lo) | ((u16)readb(ir_hi) << 8);

		if (key != last_key) {
			printf("gxlpc: key=0x%02x%s\n", key,
			       (key & 0x40) ? " pressed" : "");
			last_key = key;
		}
		if (ir != last_ir && ir != 0) {
			printf("gxlpc: ir=0x%04x%s\n", ir,
			       (ir == 0x0059 || ir == 0x0101 ||
				ir == 0xbfaf || ir == 0xbbaf || ir == 0xff65) ?
			       " power" : "");
			last_ir = ir;
		}
		mdelay(100);
	}
	printf("gxlpc: key/IR probe done\n");
	return 0;
}

int gx_lpc_ensure_open(const char *text)
{
	if (gx_lpc8051_open_ready(GX_LPC_CAP_DISPLAY | GX_LPC_CAP_SCROLL |
				   GX_LPC_CAP_BRIGHTNESS | GX_LPC_CAP_AUX |
				   GX_LPC_CAP_RTC | GX_LPC_CAP_ALARM |
				   GX_LPC_CAP_SUSPEND))
		return 0;

	return gx_lpc8051_start(gxopen_fw, gxopen_fw_end - gxopen_fw,
				"embedded open gx6702-lpc.bin",
				text ? text : "boot");
}

unsigned int gx_lpc_get_aux(void)
{
	return gx_lpc_aux;
}

unsigned int gx_lpc_get_brightness(void)
{
	return gx_lpc_brightness;
}

unsigned int gx_lpc_get_scroll_length(void)
{
	return gx_lpc_scroll_length;
}
