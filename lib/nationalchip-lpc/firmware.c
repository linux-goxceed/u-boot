/* SPDX-License-Identifier: MIT */
/*
 * Open display/RTC firmware for the GX6702 standby 8051.
 *
 * Soft standby: CK610 STOP with optional legacy SFR bit1.  ABI 1.6 can leave
 * bit1 clear so the 8051 and its dim HH:MM display remain clocked.  Wake and
 * cold-boot use the TM1650 power key (0x4f) or decoded NEC IR power codes;
 * bit2 is asserted only after an intentional wake event.
 */

#include "mailbox.h"

typedef unsigned char u8;
typedef unsigned int u16;
typedef unsigned long u32;

__sfr __at (0x80) P0;
__sfr __at (0x90) P1;
__sfr __at (0x88) TCON;
__sfr __at (0x89) TMOD;
__sfr __at (0x8a) TL0;
__sfr __at (0x8b) TL1;
__sfr __at (0x8c) TH0;
__sfr __at (0x8d) TH1;
__sfr __at (0x8e) GX_PWCM;
__sfr __at (0x93) GX_SYS_CTL;
__sfr __at (0x9a) GX_P0_MODE;
__sfr __at (0x9b) GX_P1_MODE;
__sfr __at (0x9e) GX_P0_ENABLE;
__sfr __at (0x9f) GX_P1_ENABLE;
__sfr __at (0xa8) IE;

#define PANEL_CLK	0x20
#define PANEL_DAT	0x40
#define PANEL_PINS	(PANEL_CLK | PANEL_DAT)
#define GX6702_WAKE_INPUT_GPIO		0x01
#define GX6702_PMU_POWER_CUT_GPIO	0x10
#define GX6702_RETENTION_GPIO		0x08

#define TM1650_CONTROL_ADDR	0x48
#define TM1650_KEY_ADDR		0x49
#define TM1650_DIGIT0_ADDR	0x68
#define TM1650_DOT		0x80
/* Display on + brightness step 1 (stock standby dims to the lowest on level). */
#define TM1650_CTRL_STANDBY	0x11
#define TM1650_KEY_PRESSED	0x40
/*
 * LXDVB501 power key is DIG4/KI2 (TM1650 scan 0x4f).  DIG4/KI1 (0x47) is
 * channel+, DIG4/KI7 (0x77) is channel-.  Override via mb_suspend_reserved0.
 */
#define TM1650_KEY_POWER	0x4f
#define GX6702_IR_GPIO		GX6702_WAKE_INPUT_GPIO

/*
 * Timer0 runs at 27 MHz / 12 = 2.25 MHz (same base as Timer1).  EXT0 is
 * falling-edge only, so intervals are mark+space between demod falling edges.
 * keymap.xml GUIK_H / power for Remotes 1, 2, and TELEFUNKEN.
 */
#define IR_LEAD_MIN		25000U
#define IR_LEAD_MAX		36000U
#define IR_BIT1_MIN		3500U
#define IR_REPEAT_MIN		20000U
#define IR_REPEAT_MAX		28000U
#define IR_ST_IDLE		0
#define IR_ST_LEAD		1
#define IR_ST_DATA		2
#define IR_POWER_REMOTE1	0xbfaf
#define IR_POWER_REMOTE2	0xbbaf
#define IR_POWER_TELEFUNKEN	0xff65
/*
 * Live NEC (addr<<8)|cmd from the demod — not the eCos keymap.xml values
 * (those are remapped later).  LXDVB501 board remote power = 0x0059.
 * Emmerson T151HE (same NationalChip/eCos family, different remote) = 0x0101.
 * Other NationalChip STBs often differ; probe with hd2015 -K and extend this
 * list (or pass a wake key) per product.
 */
#define IR_POWER_BOARD		0x0059
#define IR_POWER_ALT_STB	0x0101

#define TIMER1_RUN		0x40
#define TIMER0_RUN		0x10
#define EXT0_EDGE_TRIGGERED	0x01
#define TIMER1_MODE_MASK	0xc0
#define TIMER1_MODE_16BIT	0x10
#define TIMER0_GATE_COUNTER_MASK	0x0c
#define TIMER0_MODE_16BIT	0x01
#define EXT0_INTERRUPT		0x01
#define TIMER0_INTERRUPT	0x02
#define TIMER1_INTERRUPT	0x08
#define INTERRUPTS_ENABLE	0x80
#define TIMER1_RELOAD_LOW	0x28
#define TIMER1_RELOAD_HIGH	0xa8

#define GX_SYS_LOW_POWER_ENABLE	0x02
#define GX_SYS_CK610_POWER_OFF	0x04
#define GX_SYS_POWER_CUT_HIGH		0x20

/* Stock eCos/vendor low-power structure, shared with the always-on block. */
__xdata __at (0x0004) volatile u8 vendor_wake_ticks0;
__xdata __at (0x0005) volatile u8 vendor_wake_ticks1;
__xdata __at (0x0006) volatile u8 vendor_wake_ticks2;
__xdata __at (0x0007) volatile u8 vendor_wake_ticks3;
__xdata __at (0x0008) volatile u8 vendor_gpio_mask[4];
__xdata __at (0x000c) volatile u8 vendor_gpio_data[4];
__xdata __at (0x0070) volatile u8 vendor_suspend_reason;

__xdata __at (0x0100) volatile u8 mb_update;
__xdata __at (0x0101) volatile u8 mb_segment0;
__xdata __at (0x0102) volatile u8 mb_segment1;
__xdata __at (0x0103) volatile u8 mb_segment2;
__xdata __at (0x0104) volatile u8 mb_segment3;
__xdata __at (0x0105) volatile u8 mb_control;
__xdata __at (0x0106) volatile u8 mb_aux;
__xdata __at (0x0107) volatile u8 mb_reserved;

__xdata __at (0x0108) volatile u8 mb_status;
__xdata __at (0x0109) volatile u8 mb_abi_major;
__xdata __at (0x010a) volatile u8 mb_abi_minor;
__xdata __at (0x010b) volatile u8 mb_capabilities;
__xdata __at (0x010c) volatile u8 mb_ack_count;
__xdata __at (0x010d) volatile u8 mb_last_error;
/* Last TM1650 key-scan byte (bit6 set while pressed). */
__xdata __at (0x010e) volatile u8 mb_last_key;
/* Last decoded NEC code: little-endian (addr<<8)|cmd published for probes. */
__xdata __at (0x010f) volatile u8 mb_last_ir_lo;
__xdata __at (0x0164) volatile u8 mb_last_ir_hi;

__xdata __at (0x0110) volatile u8 mb_scroll_length;
__xdata __at (0x0111) volatile u8 mb_scroll_flags;
__xdata __at (0x0112) volatile u8 mb_scroll_period;
__xdata __at (0x0113) volatile u8 mb_scroll_position;
__xdata __at (0x0114) volatile u8 mb_scroll_data[GX_LPC_SCROLL_MAX];

__xdata __at (0x0134) volatile u8 mb_rtc_control;
__xdata __at (0x0135) volatile u8 mb_rtc_set_sequence;
__xdata __at (0x0136) volatile u8 mb_rtc_set_hour;
__xdata __at (0x0137) volatile u8 mb_rtc_set_minute;
__xdata __at (0x0138) volatile u8 mb_rtc_set_second;
__xdata __at (0x0139) volatile u8 mb_rtc_reserved0;
__xdata __at (0x013a) volatile u8 mb_rtc_reserved1;
__xdata __at (0x013b) volatile u8 mb_rtc_reserved2;

__xdata __at (0x013c) volatile u8 mb_rtc_hour;
__xdata __at (0x013d) volatile u8 mb_rtc_minute;
__xdata __at (0x013e) volatile u8 mb_rtc_second;
__xdata __at (0x013f) volatile u8 mb_rtc_status;
__xdata __at (0x0140) volatile u8 mb_rtc_day_low;
__xdata __at (0x0141) volatile u8 mb_rtc_day_high;
__xdata __at (0x0142) volatile u8 mb_rtc_set_ack;
__xdata __at (0x0143) volatile u8 mb_rtc_reserved_status;

__xdata __at (0x0144) volatile u8 mb_alarm_control;
__xdata __at (0x0145) volatile u8 mb_alarm_set_sequence;
__xdata __at (0x0146) volatile u8 mb_alarm_set_hour;
__xdata __at (0x0147) volatile u8 mb_alarm_set_minute;
__xdata __at (0x0148) volatile u8 mb_alarm_set_second;
__xdata __at (0x0149) volatile u8 mb_alarm_reserved0;
__xdata __at (0x014a) volatile u8 mb_alarm_reserved1;
__xdata __at (0x014b) volatile u8 mb_alarm_reserved2;

__xdata __at (0x014c) volatile u8 mb_alarm_status;
__xdata __at (0x014d) volatile u8 mb_alarm_hour;
__xdata __at (0x014e) volatile u8 mb_alarm_minute;
__xdata __at (0x014f) volatile u8 mb_alarm_second;
__xdata __at (0x0150) volatile u8 mb_alarm_set_ack;
__xdata __at (0x0151) volatile u8 mb_alarm_trigger_count;
__xdata __at (0x0152) volatile u8 mb_alarm_reserved_status0;
__xdata __at (0x0153) volatile u8 mb_alarm_reserved_status1;

__xdata __at (0x0154) volatile u8 mb_suspend_control;
__xdata __at (0x0155) volatile u8 mb_suspend_sequence;
__xdata __at (0x0156) volatile u8 mb_suspend_guard0;
__xdata __at (0x0157) volatile u8 mb_suspend_guard1;
__xdata __at (0x0158) volatile u8 mb_suspend_reserved0;
__xdata __at (0x0159) volatile u8 mb_suspend_reserved1;
__xdata __at (0x015a) volatile u8 mb_suspend_reserved2;
__xdata __at (0x015b) volatile u8 mb_suspend_reserved3;

__xdata __at (0x015c) volatile u8 mb_suspend_status;
__xdata __at (0x015d) volatile u8 mb_suspend_ack;
__xdata __at (0x015e) volatile u8 mb_suspend_error;
__xdata __at (0x015f) volatile u8 mb_suspend_reserved_status;
__xdata __at (0x0160) volatile u8 mb_suspend_seconds0;
__xdata __at (0x0161) volatile u8 mb_suspend_seconds1;
__xdata __at (0x0162) volatile u8 mb_suspend_seconds2;
__xdata __at (0x0163) volatile u8 mb_suspend_seconds3;

static u8 scroll_active;
static u8 scroll_length;
static u8 scroll_position;
static volatile u8 rtc_subsecond;
static volatile u8 rtc_second;
static volatile u8 rtc_minute;
static volatile u8 rtc_hour;
static volatile unsigned int rtc_days;
static volatile u8 rtc_refresh;
static u8 rtc_control;
static u8 rtc_time_valid;
static u8 rtc_last_set_sequence;
static volatile u8 alarm_armed;
static volatile u8 alarm_active;
static u8 alarm_hour;
static u8 alarm_minute;
static u8 alarm_second;
static u8 alarm_last_set_sequence;
static u8 alarm_trigger_count;
static u8 suspend_last_sequence;
static u8 soft_standby;
static u8 wake_btn_armed;
static u8 wake_key_code;
/* Soft-standby RTC wake: countdown seconds, or 0 = use armed absolute alarm. */
static u8 soft_wake_rtc;
static u32 soft_wake_left;
static volatile u8 soft_wake_due;
static volatile u8 ir_state;
static volatile u8 ir_bits;
static volatile u8 ir_acc;
static volatile u8 ir_bytes[4];
static volatile u8 ir_power_hit;
static volatile u8 ir_last_was_power;

static const __code u8 digit_segments[10] = {
	0x3f, 0x06, 0x5b, 0x4f, 0x66,
	0x6d, 0x7d, 0x07, 0x7f, 0x6f,
};

static void bus_delay(void)
{
	volatile u8 n;

	/* Conservatively slower than the vendor's 15-count delay at 27 MHz. */
	for (n = 0; n != 24; n++)
		__asm
		nop
		__endasm;
}

static void clk_high(void)
{
	P1 |= PANEL_CLK;
}

static void clk_low(void)
{
	P1 &= (u8)~PANEL_CLK;
}

static void dat_high(void)
{
	P1 |= PANEL_DAT;
}

static void dat_low(void)
{
	P1 &= (u8)~PANEL_DAT;
}

static void tm1650_start(void)
{
	dat_high();
	clk_high();
	bus_delay();
	dat_low();
	bus_delay();
	clk_low();
}

static void tm1650_stop(void)
{
	dat_low();
	clk_low();
	bus_delay();
	clk_high();
	bus_delay();
	dat_high();
	bus_delay();
}

static void tm1650_write_byte(u8 value)
{
	u8 bit;

	for (bit = 0; bit != 8; bit++) {
		if (value & 0x80)
			dat_high();
		else
			dat_low();
		bus_delay();
		clk_high();
		bus_delay();
		clk_low();
		value <<= 1;
	}

	/* Ninth clock.  The vendor transmitter releases DAT but ignores ACK. */
	dat_high();
	bus_delay();
	clk_high();
	bus_delay();
	clk_low();
}

static void tm1650_write(u8 address, u8 value)
{
	tm1650_start();
	tm1650_write_byte(address);
	tm1650_write_byte(value);
	tm1650_stop();
}

/* Read one TM1650 key-scan byte (front-panel buttons). DAT is briefly an input. */
static u8 tm1650_read_key(void)
{
	u8 value = 0;
	u8 bit;

	tm1650_start();
	tm1650_write_byte(TM1650_KEY_ADDR);
	dat_high();
	/* Mode-1 input on DAT while CLK stays an output. */
	GX_P1_MODE |= PANEL_DAT;
	bus_delay();
	for (bit = 0; bit != 8; bit++) {
		value <<= 1;
		clk_high();
		bus_delay();
		if (P1 & PANEL_DAT)
			value |= 1;
		clk_low();
		bus_delay();
	}
	GX_P1_MODE &= (u8)~PANEL_DAT;
	tm1650_stop();
	return value;
}

static void panel_send(u8 segments[4], u8 aux)
{
	u8 digit;

	aux &= GX_LPC_AUX_MASK;

	tm1650_write(TM1650_CONTROL_ADDR, mb_control);
	for (digit = 0; digit != 4; digit++) {
		if (aux & (1u << digit))
			segments[digit] |= TM1650_DOT;
		tm1650_write(TM1650_DIGIT0_ADDR + (digit << 1),
			     segments[digit]);
	}
}

static void panel_apply_static(void)
{
	u8 segments[4];

	segments[0] = mb_segment0;
	segments[1] = mb_segment1;
	segments[2] = mb_segment2;
	segments[3] = mb_segment3;
	panel_send(segments, mb_aux);
}

static void panel_apply_scroll(void)
{
	u8 segments[4];
	u8 digit;
	u8 index;

	for (digit = 0; digit != 4; digit++) {
		index = scroll_position + digit;
		segments[digit] = index < scroll_length ?
			mb_scroll_data[index] : 0;
	}
	panel_send(segments, mb_aux);
}

static void rtc_publish_snapshot(void)
{
	u8 status = GX_LPC_RTC_STATUS_RUNNING;
	u8 alarm_status = 0;

	/* Odd/even generation lets the CK610 reject a torn multi-byte read. */
	mb_rtc_reserved_status++;
	if (rtc_control & GX_LPC_RTC_DISPLAY)
		status |= GX_LPC_RTC_STATUS_DISPLAY;
	if (rtc_time_valid)
		status |= GX_LPC_RTC_STATUS_TIME_VALID;
	mb_rtc_hour = rtc_hour;
	mb_rtc_minute = rtc_minute;
	mb_rtc_second = rtc_second;
	mb_rtc_status = status;
	mb_rtc_day_low = rtc_days;
	mb_rtc_day_high = rtc_days >> 8;
	mb_rtc_set_ack = rtc_last_set_sequence;
	if (alarm_armed)
		alarm_status |= GX_LPC_ALARM_STATUS_ARMED;
	if (alarm_active)
		alarm_status |= GX_LPC_ALARM_STATUS_ACTIVE;
	mb_alarm_status = alarm_status;
	mb_alarm_hour = alarm_hour;
	mb_alarm_minute = alarm_minute;
	mb_alarm_second = alarm_second;
	mb_alarm_set_ack = alarm_last_set_sequence;
	mb_alarm_trigger_count = alarm_trigger_count;
	mb_alarm_reserved_status0 = 0;
	mb_alarm_reserved_status1 = 0;
	mb_rtc_reserved_status++;
}

static void panel_apply_clock(void)
{
	u8 segments[4];
	u8 saved_ie;
	u8 hour;
	u8 minute;
	u8 second;
	u8 aux;

	saved_ie = IE;
	IE &= (u8)~TIMER1_INTERRUPT;
	hour = rtc_hour;
	minute = rtc_minute;
	second = rtc_second;
	IE = saved_ie;

	segments[0] = digit_segments[hour / 10];
	segments[1] = digit_segments[hour % 10];
	segments[2] = digit_segments[minute / 10];
	segments[3] = digit_segments[minute % 10];
	aux = mb_aux & (u8)~GX_LPC_AUX_CLOCK_COLON;
	if (!(second & 1))
		aux |= GX_LPC_AUX_CLOCK_COLON;
	panel_send(segments, aux);
}

static void panel_apply_alarm(void)
{
	u8 segments[4];
	u8 aux;

	if (rtc_second & 1) {
		panel_apply_clock();
		return;
	}
	segments[0] = 0x77;	/* A */
	segments[1] = 0x38;	/* L */
	segments[2] = 0x50;	/* r */
	segments[3] = 0x78;	/* t */
	aux = (mb_aux & (u8)~GX_LPC_AUX_CLOCK_COLON) |
		GX_LPC_AUX_STATUS_GREEN | GX_LPC_AUX_POWER;
	panel_send(segments, aux);
}

static void rtc_apply_mailbox(u8 force)
{
	u8 control = mb_rtc_control;
	u8 sequence = mb_rtc_set_sequence;
	u8 hour = mb_rtc_set_hour;
	u8 minute = mb_rtc_set_minute;
	u8 second = mb_rtc_set_second;
	u8 saved_ie;

	saved_ie = IE;
	IE &= (u8)~TIMER1_INTERRUPT;
	if ((control & GX_LPC_RTC_TIME_VALID) &&
	    (force || sequence != rtc_last_set_sequence)) {
		rtc_last_set_sequence = sequence;
		if (hour < 24 && minute < 60 && second < 60) {
			rtc_hour = hour;
			rtc_minute = minute;
			rtc_second = second;
			rtc_subsecond = 0;
			rtc_days = 0;
			rtc_time_valid = 1;
			mb_last_error = GX_LPC_ERROR_NONE;
		} else {
			mb_last_error = GX_LPC_ERROR_RTC_TIME;
		}
	}
	rtc_control = control;
	rtc_publish_snapshot();
	IE = saved_ie;
}

static void alarm_apply_mailbox(u8 force)
{
	u8 control = mb_alarm_control;
	u8 sequence = mb_alarm_set_sequence;
	u8 hour = mb_alarm_set_hour;
	u8 minute = mb_alarm_set_minute;
	u8 second = mb_alarm_set_second;
	u8 saved_ie = IE;

	IE &= (u8)~TIMER1_INTERRUPT;
	if (force || sequence != alarm_last_set_sequence) {
		alarm_last_set_sequence = sequence;
		alarm_active = 0;
		if (control & GX_LPC_ALARM_ARMED) {
			if (hour < 24 && minute < 60 && second < 60) {
				alarm_hour = hour;
				alarm_minute = minute;
				alarm_second = second;
				alarm_armed = 1;
				mb_last_error = GX_LPC_ERROR_NONE;
			} else {
				alarm_armed = 0;
				mb_last_error = GX_LPC_ERROR_RTC_TIME;
			}
		} else {
			alarm_armed = 0;
		}
	}
	rtc_publish_snapshot();
	IE = saved_ie;
}

static void suspend_publish_u32(u32 seconds, u32 ticks)
{
	mb_suspend_seconds0 = seconds;
	mb_suspend_seconds1 = seconds >> 8;
	mb_suspend_seconds2 = seconds >> 16;
	mb_suspend_seconds3 = seconds >> 24;
	vendor_wake_ticks0 = ticks;
	vendor_wake_ticks1 = ticks >> 8;
	vendor_wake_ticks2 = ticks >> 16;
	vendor_wake_ticks3 = ticks >> 24;
}

static void suspend_reject(u8 error, u8 saved_ie)
{
	mb_suspend_status = GX_LPC_SUSPEND_STATUS_REJECTED;
	mb_suspend_ack = suspend_last_sequence;
	mb_suspend_error = error;
	mb_last_error = error;
	IE = saved_ie;
}

static void panel_apply_suspend(void)
{
	/* Stock standby: HH:MM, power LED, dimmest non-off brightness. */
	mb_aux = GX_LPC_AUX_POWER;
	mb_control = TM1650_CTRL_STANDBY;
	rtc_control |= GX_LPC_RTC_DISPLAY;
	panel_apply_clock();
}

/*
 * Intentional cold boot from live-8051 standby.  Bit 2 alone halted instead
 * of restarting when bit 1 was clear.  Assert both with one direct-SFR ORL:
 * two separate writes risk bit 1 stopping the 8051 before it can set bit 2.
 */
static void enter_destructive_poweroff(void)
{
	IE &= (u8)~INTERRUPTS_ENABLE;
	GX_SYS_CTL |= GX_SYS_LOW_POWER_ENABLE | GX_SYS_CK610_POWER_OFF;
	for (;;)
		;
}

/*
 * Soft standby: live dimmed clock.  Cold-boot on NEC IR power, TM1650 power
 * key, or RTC wake (countdown / absolute alarm).  Button/IR require a quiet
 * sample first so the press that entered standby does not wake.
 */
static void soft_standby_poll(void)
{
	u8 ir_hit;
	u8 key;
	u8 power_down;
	u8 rtc_hit;

	if (!soft_standby)
		return;

	rtc_hit = soft_wake_due ||
		  (soft_wake_rtc && !soft_wake_left && alarm_active);
	if (rtc_hit) {
		soft_wake_due = 0;
		vendor_suspend_reason = 2;
		enter_destructive_poweroff();
	}

	ir_hit = ir_power_hit;
	if (ir_hit)
		ir_power_hit = 0;
	key = tm1650_read_key();
	mb_last_key = key;
	power_down = (key == wake_key_code);
	if (!ir_hit && !power_down) {
		wake_btn_armed = 1;
		return;
	}
	if (!wake_btn_armed)
		return;

	vendor_suspend_reason = 1;
	enter_destructive_poweroff();
}

static u8 ir_code_is_power(u16 code)
{
	return code == IR_POWER_REMOTE1 ||
	       code == IR_POWER_REMOTE2 ||
	       code == IR_POWER_TELEFUNKEN ||
	       code == IR_POWER_BOARD ||
	       code == IR_POWER_ALT_STB;
}

static void ir_accept_frame(void)
{
	u16 code;
	u8 addr = ir_bytes[0];
	u8 cmd = ir_bytes[2];

	/* Prefer standard NEC; also accept if inverses are wrong (odd remotes). */
	if (ir_bytes[1] == (u8)~addr && ir_bytes[3] == (u8)~cmd)
		code = ((u16)addr << 8) | cmd;
	else
		code = ((u16)addr << 8) | cmd;

	mb_last_ir_lo = (u8)code;
	mb_last_ir_hi = (u8)(code >> 8);
	ir_last_was_power = ir_code_is_power(code);
	if (ir_last_was_power)
		ir_power_hit = 1;
}

static void ir_on_edge(u16 dt)
{
	u8 idx;

	switch (ir_state) {
	case IR_ST_IDLE:
		ir_state = IR_ST_LEAD;
		break;
	case IR_ST_LEAD:
		if (dt >= IR_LEAD_MIN && dt <= IR_LEAD_MAX) {
			ir_state = IR_ST_DATA;
			ir_bits = 0;
			ir_acc = 0;
		} else if (dt >= IR_REPEAT_MIN && dt <= IR_REPEAT_MAX) {
			if (ir_last_was_power)
				ir_power_hit = 1;
			ir_state = IR_ST_IDLE;
		} else {
			ir_state = IR_ST_LEAD;
		}
		break;
	case IR_ST_DATA:
		/* NEC bits arrive LSB-first within each byte. */
		ir_acc >>= 1;
		if (dt >= IR_BIT1_MIN)
			ir_acc |= 0x80;
		ir_bits++;
		if ((ir_bits & 7) == 0) {
			idx = (ir_bits >> 3) - 1;
			ir_bytes[idx] = ir_acc;
			ir_acc = 0;
		}
		if (ir_bits == 32) {
			ir_accept_frame();
			ir_state = IR_ST_IDLE;
		}
		break;
	default:
		ir_state = IR_ST_IDLE;
		break;
	}
}

static void wake_controller_wait_ticks(u8 saved_ie, u8 ticks)
{
	u8 elapsed = 0;
	u8 last_tick = rtc_subsecond;
	u8 tick;

	/*
	 * Bit 1 freezes a still-running CK610 (seen on hd2015 boot when armed
	 * at init, and on -S button when PREPARED waited after bit 1).  Stock
	 * STOPs first; open path waits here so U-Boot can jsr STOP, then arms
	 * bit 1 only against an already-stopped core before bit 2.
	 */
	IE = saved_ie;
	while (elapsed != ticks) {
		tick = rtc_subsecond;
		if (tick != last_tick) {
			last_tick = tick;
			elapsed++;
		}
	}
}

static void pmu_power_cut_gpio_init(void)
{
	/*
	 * The stock GX6702 gpio.xml declares powercut="12,0".  Vendor
	 * ConfigureLowPowerWake maps logical pin 12 to P1 bit 4: mode value 2
	 * sets SFR 0x9f bit 4 and clears SFR 0x9b bit 4.  The configured level
	 * is zero, so SFR 0x93 bit 5 remains clear.
	 */
	GX_P1_ENABLE |= GX6702_PMU_POWER_CUT_GPIO;
	GX_P1_MODE &= (u8)~GX6702_PMU_POWER_CUT_GPIO;
}

/*
 * Vendor ConfigureLowPowerWake (0x0bb9): clear bit 5 for powercut level 0,
 * set bit 1, configure the power-cut pin.  Stock does this at firmware init
 * while CK610 is still running; bit 2 comes much later after STOP.
 */
static void configure_low_power_wake(void)
{
	GX_SYS_CTL &= (u8)~GX_SYS_POWER_CUT_HIGH;
	GX_SYS_CTL |= GX_SYS_LOW_POWER_ENABLE;
	pmu_power_cut_gpio_init();
}

static void configure_live_8051_wake(void)
{
	/*
	 * Linux has already quiesced its main-domain devices and CK610 executes
	 * STOP from ISRAM.  Keep both LPC power-control bits clear so Timer1,
	 * TM1650 polling and NEC IR decoding continue to run indefinitely.
	 */
	GX_SYS_CTL &= (u8)~(GX_SYS_POWER_CUT_HIGH |
			       GX_SYS_LOW_POWER_ENABLE);
	pmu_power_cut_gpio_init();
}

static void retention_gpio_init(void)
{
	/*
	 * A shared-XDATA snapshot taken immediately after genuine stock
	 * standby contains mask 0xfffff7ff and data 0x00000800: GPIO 11 is
	 * the sole retained output and it is high.  Vendor gpio_write(11, 1)
	 * maps that pin to P1 bit 3, selects mode 0 by clearing both mode
	 * registers, then raises the output latch.
	 */
	GX_P1_ENABLE &= (u8)~GX6702_RETENTION_GPIO;
	GX_P1_MODE &= (u8)~GX6702_RETENTION_GPIO;
	P1 |= GX6702_RETENTION_GPIO;
}

static void wake_input_init(void)
{
	/*
	 * Vendor init at 0x00fb conditions logical GPIO 0 before it starts
	 * polling for a power event.  GPIO 0 is P0.0 in mode 0, driven high;
	 * external interrupt 0 is edge-triggered and Timer 0 measures the
	 * interval between IR demod falling edges for NEC decode.
	 */
	GX_P0_ENABLE &= (u8)~GX6702_WAKE_INPUT_GPIO;
	GX_P0_MODE &= (u8)~GX6702_WAKE_INPUT_GPIO;
	P0 |= GX6702_WAKE_INPUT_GPIO;
	ir_state = IR_ST_IDLE;
	ir_bits = 0;
	ir_acc = 0;
	ir_power_hit = 0;
	ir_last_was_power = 0;
	TCON |= EXT0_EDGE_TRIGGERED;
	GX_PWCM &= (u8)~0x08;
	TMOD &= (u8)~TIMER0_GATE_COUNTER_MASK;
	TMOD |= TIMER0_MODE_16BIT;
	TH0 = 0;
	TL0 = 0;
	TCON |= TIMER0_RUN;
	IE |= EXT0_INTERRUPT | TIMER0_INTERRUPT;
}

static void suspend_apply_mailbox(u8 force)
{
	u8 sequence = mb_suspend_sequence;
	u8 saved_ie;
	u32 wake_seconds;

	if (force) {
		suspend_last_sequence = sequence;
		mb_suspend_status = 0;
		mb_suspend_ack = sequence;
		mb_suspend_error = GX_LPC_ERROR_NONE;
		mb_suspend_reserved_status = 0;
		suspend_publish_u32(0, 0);
		return;
	}
	if (sequence == suspend_last_sequence)
		return;

	suspend_last_sequence = sequence;
	saved_ie = IE;
	IE &= (u8)~TIMER1_INTERRUPT;
	if (!(mb_suspend_control & GX_LPC_SUSPEND_REQUEST) ||
	    mb_suspend_guard0 != GX_LPC_SUSPEND_GUARD0 ||
	    mb_suspend_guard1 != GX_LPC_SUSPEND_GUARD1) {
		suspend_reject(GX_LPC_ERROR_SUSPEND_GUARD, saved_ie);
		return;
	}
	/*
	 * RTC wake is only defined for soft standby (NO_POWEROFF).  Destructive
	 * bit2-on-enter still rejects WAKE_ALARM.
	 */
	if ((mb_suspend_control & GX_LPC_SUSPEND_WAKE_ALARM) &&
	    !(mb_suspend_control & GX_LPC_SUSPEND_NO_POWEROFF)) {
		suspend_reject(GX_LPC_ERROR_SUSPEND_ALARM, saved_ie);
		return;
	}

	wake_seconds = (u32)mb_suspend_seconds0 |
		       ((u32)mb_suspend_seconds1 << 8) |
		       ((u32)mb_suspend_seconds2 << 16) |
		       ((u32)mb_suspend_seconds3 << 24);
	soft_wake_rtc = 0;
	soft_wake_left = 0;
	soft_wake_due = 0;
	if (mb_suspend_control & GX_LPC_SUSPEND_WAKE_ALARM) {
		if (wake_seconds) {
			if (wake_seconds < GX_LPC_SUSPEND_MIN_SECONDS) {
				suspend_reject(GX_LPC_ERROR_SUSPEND_SOON,
					       saved_ie);
				return;
			}
			soft_wake_rtc = 1;
			soft_wake_left = wake_seconds;
		} else if (alarm_armed) {
			soft_wake_rtc = 1;
			soft_wake_left = 0;
		} else {
			suspend_reject(GX_LPC_ERROR_SUSPEND_ALARM, saved_ie);
			return;
		}
	}

	/* Zero disables the vendor's awake-time automatic-suspend counter. */
	/* Exact little-endian values recovered from genuine stock standby. */
	vendor_gpio_mask[0] = 0xff;
	vendor_gpio_mask[1] = 0xf7;
	vendor_gpio_mask[2] = 0xff;
	vendor_gpio_mask[3] = 0xff;
	vendor_gpio_data[0] = 0x00;
	vendor_gpio_data[1] = 0x08;
	vendor_gpio_data[2] = 0x00;
	vendor_gpio_data[3] = 0x00;
	suspend_publish_u32(wake_seconds, 0);
	/* Vendor PollPowerKeyAndSuspend publishes 1 or 2 before bit 2. */
	vendor_suspend_reason = 1;
	mb_suspend_ack = sequence;
	mb_suspend_error = GX_LPC_ERROR_NONE;
	mb_last_error = GX_LPC_ERROR_NONE;
	panel_apply_suspend();

	/*
	 * PREPARED = request accepted.  CK610 STOPs immediately after publish;
	 * wait ~300ms for it.  KEEP_8051 leaves bit 1 clear, while legacy callers
	 * arm it only against an already-stopped CK610.
	 */
	mb_suspend_status = GX_LPC_SUSPEND_STATUS_PREPARED;
	wake_controller_wait_ticks(saved_ie, 30);
	if (mb_suspend_control & GX_LPC_SUSPEND_KEEP_8051)
		configure_live_8051_wake();
	else
		configure_low_power_wake();
	retention_gpio_init();

	if (mb_suspend_control & GX_LPC_SUSPEND_NO_POWEROFF) {
		/*
		 * CK610 is in STOP.  With KEEP_8051, both LPC power bits remain
		 * clear; return to main so Timer1 keeps the HH:MM display alive
		 * and soft_standby_poll() cold-boots on key or RTC wake.
		 */
		wake_key_code = mb_suspend_reserved0 ?
			mb_suspend_reserved0 : TM1650_KEY_POWER;
		soft_standby = 1;
		wake_btn_armed = 0;
		IE = saved_ie;
		return;
	}

	/* Vendor: DisableGlobalInterrupts then EnterDestructiveSuspend. */
	enter_destructive_poweroff();
}

void timer1_isr(void) __interrupt (3)
{
	TH1 = TIMER1_RELOAD_HIGH;
	TL1 = TIMER1_RELOAD_LOW;
	if (++rtc_subsecond != 100)
		return;

	rtc_subsecond = 0;
	if (++rtc_second == 60) {
		rtc_second = 0;
		if (++rtc_minute == 60) {
			rtc_minute = 0;
			if (++rtc_hour == 24) {
				rtc_hour = 0;
				rtc_days++;
			}
		}
	}
	if (alarm_armed && rtc_hour == alarm_hour &&
	    rtc_minute == alarm_minute && rtc_second == alarm_second) {
		alarm_armed = 0;
		alarm_active = 1;
		alarm_trigger_count++;
	}
	if (soft_standby && soft_wake_rtc && soft_wake_left) {
		soft_wake_left--;
		if (!soft_wake_left)
			soft_wake_due = 1;
	}
	rtc_refresh = 1;
	rtc_publish_snapshot();
}

void ext0_isr(void) __interrupt (0)
{
	u16 dt = ((u16)TH0 << 8) | TL0;

	TH0 = 0;
	TL0 = 0;
	ir_on_edge(dt);
}

void timer0_isr(void) __interrupt (1)
{
	/* Gap timeout: abandon a partial frame. */
	TH0 = 0;
	TL0 = 0;
	ir_state = IR_ST_IDLE;
	ir_bits = 0;
	ir_acc = 0;
}

static void rtc_init(void)
{
	rtc_subsecond = 0;
	rtc_second = 0;
	rtc_minute = 0;
	rtc_hour = 0;
	rtc_days = 0;
	rtc_refresh = 0;
	rtc_control = 0;
	rtc_time_valid = 0;
	rtc_last_set_sequence = 0;
	alarm_armed = 0;
	alarm_active = 0;
	alarm_hour = 0;
	alarm_minute = 0;
	alarm_second = 0;
	alarm_last_set_sequence = 0;
	alarm_trigger_count = 0;
	suspend_last_sequence = 0;
	mb_rtc_reserved_status = 0;
	rtc_apply_mailbox(1);
	alarm_apply_mailbox(1);
	suspend_apply_mailbox(1);

	/* Vendor 27 MHz Timer 1 setup: 16-bit mode, approximately 100 Hz. */
	GX_PWCM &= (u8)~0x10;
	TMOD &= (u8)~TIMER1_MODE_MASK;
	TMOD |= TIMER1_MODE_16BIT;
	TH1 = TIMER1_RELOAD_HIGH;
	TL1 = TIMER1_RELOAD_LOW;
	IE |= TIMER1_INTERRUPT;
	TCON |= TIMER1_RUN;
	wake_input_init();
	IE |= INTERRUPTS_ENABLE;
}

static void panel_apply_update(void)
{
	u8 length = mb_scroll_length;

	rtc_apply_mailbox(0);
	alarm_apply_mailbox(0);
	suspend_apply_mailbox(0);
	if (alarm_active) {
		scroll_active = 0;
		scroll_length = 0;
		scroll_position = 0;
		mb_scroll_position = 0;
		panel_apply_alarm();
		return;
	}
	if (rtc_control & GX_LPC_RTC_DISPLAY) {
		scroll_active = 0;
		scroll_length = 0;
		scroll_position = 0;
		mb_scroll_position = 0;
		panel_apply_clock();
		return;
	}

	if ((mb_scroll_flags & GX_LPC_SCROLL_ENABLE) && length > 4 &&
	    length <= GX_LPC_SCROLL_MAX) {
		scroll_active = 1;
		scroll_length = length;
		scroll_position = 0;
		mb_scroll_position = 0;
		panel_apply_scroll();
	} else {
		scroll_active = 0;
		scroll_length = 0;
		scroll_position = 0;
		mb_scroll_position = 0;
		panel_apply_static();
	}
}

static u8 scroll_wait(void)
{
	volatile unsigned int count;
	u8 slice;
	u8 period = mb_scroll_period;

	if (!period)
		period = GX_LPC_SCROLL_PERIOD;
	for (slice = 0; slice != period; slice++) {
		count = 0;
		do {
			count++;
			if (!(u8)count && mb_update)
				return 1;
		} while (count);
	}
	return 0;
}

static void scroll_advance(void)
{
	scroll_position++;
	if (scroll_position >= scroll_length + GX_LPC_SCROLL_GAP)
		scroll_position = 0;
	mb_scroll_position = scroll_position;
	panel_apply_scroll();
}

static void hardware_init(void)
{
	/* Required by the vendor firmware before touching GPIO SFRs. */
	GX_SYS_CTL = 1;

	/* Select GPIO/output mode for P1.5 and P1.6, then idle the bus high. */
	GX_P1_ENABLE &= (u8)~PANEL_PINS;
	GX_P1_MODE &= (u8)~PANEL_PINS;
	P1 |= PANEL_PINS;
}

void main(void)
{
	mb_status = GX_LPC_STATUS_BOOTING;
	mb_abi_major = GX_LPC_ABI_MAJOR;
	mb_abi_minor = GX_LPC_ABI_MINOR;
	mb_capabilities = GX_LPC_CAP_DISPLAY |
			  GX_LPC_CAP_BRIGHTNESS |
			  GX_LPC_CAP_AUX |
			  GX_LPC_CAP_SCROLL |
			  GX_LPC_CAP_RTC |
			  GX_LPC_CAP_ALARM |
			  GX_LPC_CAP_SUSPEND;
	mb_ack_count = 0;
	mb_last_error = 0;
	mb_last_key = 0;
	mb_last_ir_lo = 0;
	mb_last_ir_hi = 0;
	scroll_active = 0;
	scroll_length = 0;
	scroll_position = 0;

	hardware_init();
	rtc_init();
	mb_status = GX_LPC_STATUS_READY;

	for (;;) {
		soft_standby_poll();
		if (!soft_standby)
			mb_last_key = tm1650_read_key();
		if (mb_update & GX_LPC_MB_UPDATE) {
			panel_apply_update();
			mb_ack_count++;
			mb_update = 0;
		}
		if (rtc_refresh) {
			u8 saved_ie = IE;

			IE &= (u8)~TIMER1_INTERRUPT;
			rtc_refresh = 0;
			IE = saved_ie;
			if (soft_standby)
				panel_apply_suspend();
			else if (alarm_active)
				panel_apply_alarm();
			else if (rtc_control & GX_LPC_RTC_DISPLAY)
				panel_apply_clock();
		}
		if (scroll_active && !scroll_wait())
			scroll_advance();
	}
}
