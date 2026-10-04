/* SPDX-License-Identifier: MIT */
/*
 * Open display/RTC firmware for the GX6702 standby 8051.
 *
 * Soft standby: CK610 STOP with optional legacy SFR bit1.  ABI 1.6 can leave
 * bit1 clear so the 8051 and its dim HH:MM display remain clocked.  Wake and
 * cold-boot use the TM1650 power key (0x4f), decoded NEC IR power codes,
 * an RTC alarm, or a HDMI CEC wake frame while cecmode is 1 or 2.
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
__sfr __at (0xab) GX_CEC_ISTAT;
__sfr __at (0xac) GX_CEC_ICTL;
__sfr __at (0xe9) GX_CEC_CLK;

#if defined(GX6706_LPC)
/*
 * gxlowpower-gx6706.fw gpio.xml: panel clk,data = 0,1 (P0.0/P0.1) and
 * powercut = 8,0 (P1.0, level 0).  The vendor image arms EXT0 on logical
 * GPIO 2 (P0.2), leaving P0.0/P0.1 for the panel bus.
 */
#define PANEL_CLK			0x01
#define PANEL_DAT			0x02
#define GX_PMU_POWER_CUT_GPIO		0x01
#define GX_WAKE_INPUT_GPIO		0x04
#define GX_LPC_KEY_POWER_EXTRA		0x47
#else
#define PANEL_CLK			0x20
#define PANEL_DAT			0x40
#define GX_PMU_POWER_CUT_GPIO		0x10
#define GX6702_WAKE_INPUT_GPIO		0x01
#define GX6702_RETENTION_GPIO		0x08
#endif
#define PANEL_PINS	(PANEL_CLK | PANEL_DAT)

#define TM1650_CONTROL_ADDR	0x48
/* HD2015 answers 0x49.  FD650B answers 0x4F.  Polling the other one corrupts it. */
#define TM1650_KEY_ADDR		0x49
#define TM1650_KEY_ADDR_FD650	0x4f
#define TM1650_DIGIT0_ADDR	0x68
#define TM1650_DOT		0x80
/* Display on + brightness step 1.  Stock standby dims to the lowest on level. */
#define TM1650_CTRL_STANDBY	0x11
#define TM1650_KEY_PRESSED	0x40
/*
 * Pressed power codes from the two panels.  Bit 6 is the press flag.
 * Released values are 0x0f and 0x37 and must not wake, or the quiet-sample
 * arm never happens.  Those two press codes always wake.  A programmed
 * override is accepted as well.
 */
#define TM1650_KEY_POWER	0x4f
#define TM1650_KEY_POWER_ALT	0x77
#ifndef GX6706_LPC
#define GX6702_IR_GPIO		GX6702_WAKE_INPUT_GPIO
#endif

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
#define EXT1_INTERRUPT		0x04
#define EXT1_EDGE_TRIGGERED	0x04
#define TIMER1_INTERRUPT	0x08
#define CEC_PIN			0x20
#define CEC_OP_IMAGE_VIEW_ON	0x04
#define CEC_OP_TEXT_VIEW_ON	0x0d
#define CEC_OP_STANDBY		0x36
#define CEC_OP_ROUTING_CHANGE	0x80
#define CEC_OP_ACTIVE_SOURCE	0x82
#define CEC_OP_SET_STREAM_PATH	0x86
#define CEC_OP_GIVE_POWER	0x8f
#define CEC_OP_REPORT_POWER	0x90
#define CEC_POWER_STANDBY	0x01
#define CEC_POWER_ON		0x00
/*
 * CEC timing is in microseconds and is converted to Timer0 ticks at run
 * time.  Timer0 counts at the 8051 clock / 12: 2.0 MHz when the shared
 * clock word says 24 MHz (measured start bit 7371 ticks = 3.69 ms), 2.25 MHz
 * for 27 MHz.  Sample point is 1.05 ms after each falling edge.
 */
/*
 * A start bit joined late still reads far longer than any data low (1.5 ms
 * at most), so accept it down to 1.8 ms; the key poll can hide the first 1.9 ms.
 */
#define CEC_US_START_MIN	1800U
#define CEC_US_START_MAX	4300U
#define CEC_US_START_HIGH_MAX	2000U
#define CEC_US_SAMPLE		1050U
#define CEC_US_ACK		1500U
/* Shortest legal bit cell is 2.05 ms; ignore any edge before this. */
#define CEC_US_CELL_MIN		1900U
#define CEC_US_CELL_WAIT	1700U
#define CEC_US_IDLE		6000U
#define CEC_US_WAIT_START	25000U
#define CEC_US_SIGNAL_FREE	8000U
#define CEC_US_TX_START_LOW	3700U
#define CEC_US_TX_START_HIGH	800U
#define CEC_US_TX_BIT0_LOW	1500U
#define CEC_US_TX_BIT0_HIGH	900U
#define CEC_US_TX_BIT1_LOW	600U
#define CEC_US_TX_BIT1_HIGH	1800U
#define CEC_T_START_MIN		cec_us(CEC_US_START_MIN)
#define CEC_T_START_MAX		cec_us(CEC_US_START_MAX)
#define CEC_T_START_HIGH_MAX	cec_us(CEC_US_START_HIGH_MAX)
#define CEC_T_SAMPLE		cec_us(CEC_US_SAMPLE)
#define CEC_T_ACK		cec_us(CEC_US_ACK)
#define CEC_T_CELL_MIN		cec_us(CEC_US_CELL_MIN)
#define CEC_T_CELL_WAIT		cec_us(CEC_US_CELL_WAIT)
#define CEC_T_IDLE		cec_us(CEC_US_IDLE)
#define CEC_T_WAIT_START	cec_us(CEC_US_WAIT_START)
#define CEC_T_SIGNAL_FREE	cec_us(CEC_US_SIGNAL_FREE)
#define CEC_T_TX_START_LOW	cec_us(CEC_US_TX_START_LOW)
#define CEC_T_TX_START_HIGH	cec_us(CEC_US_TX_START_HIGH)
#define CEC_T_TX_BIT0_LOW	cec_us(CEC_US_TX_BIT0_LOW)
#define CEC_T_TX_BIT0_HIGH	cec_us(CEC_US_TX_BIT0_HIGH)
#define CEC_T_TX_BIT1_LOW	cec_us(CEC_US_TX_BIT1_LOW)
#define CEC_T_TX_BIT1_HIGH	cec_us(CEC_US_TX_BIT1_HIGH)
#define INTERRUPTS_ENABLE	0x80
/*
 * 100 Hz tick.  Timer1 counts at the 8051 clock / 12, so the reload follows the
 * shared clock word: 65536 - 22488 at 27 MHz, 65536 - 19988 at 24 MHz.  Both
 * leave 12 ticks for interrupt latency.
 */
#define TIMER1_RELOAD_LOW	0x28
#define TIMER1_RELOAD_HIGH	0xa8
#define TIMER1_RELOAD_LOW_24	0xec
#define TIMER1_RELOAD_HIGH_24	0xb1

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
__xdata __at (0x0074) volatile u8 vendor_clock0;
__xdata __at (0x0075) volatile u8 vendor_clock1;
__xdata __at (0x0076) volatile u8 vendor_clock2;
__xdata __at (0x0077) volatile u8 vendor_clock3;
__xdata __at (0x0090) volatile u8 vendor_cecmode0;
__xdata __at (0x0091) volatile u8 vendor_cecmode1;
__xdata __at (0x0092) volatile u8 vendor_cecmode2;
__xdata __at (0x0093) volatile u8 vendor_cecmode3;

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
/* CK610 writes opcode and sequence as one aligned word.  Ack is firmware-owned. */
__xdata __at (0x0168) volatile u8 mb_cec_opcode;
__xdata __at (0x0169) volatile u8 mb_cec_sequence;
__xdata __at (0x016a) volatile u8 mb_cec_ack;

/* RAM-only wake config.  Zero panel key keeps the 0x4f default. */
__xdata __at (0x016c) volatile u8 mb_wake_panel;
__xdata __at (0x016d) volatile u8 mb_wake_sequence;
__xdata __at (0x016e) volatile u8 mb_wake_ack;
__xdata __at (0x016f) volatile u8 mb_wake_ir_count;
__xdata __at (0x0170) volatile u8 mb_wake_ir[8];

/* ABI 1.10.  CK610 writes the address before soft standby. */
__xdata __at (0x0178) volatile u8 mb_cec_pa_valid;
__xdata __at (0x0179) volatile u8 mb_cec_pa_hi;
__xdata __at (0x017a) volatile u8 mb_cec_pa_lo;
__xdata __at (0x017b) volatile u8 mb_cec_la;
/* Peripheral snapshot for a later register map.  Not a wake source. */
__xdata __at (0x017c) volatile u8 mb_cec_snap_seq;
__xdata __at (0x017d) volatile u8 mb_cec_snap_istat;
__xdata __at (0x017e) volatile u8 mb_cec_snap[48];
/* Last GPIO-decoded frame.  A changed sequence is also the ucsim inject. */
__xdata __at (0x01ae) volatile u8 mb_cec_rx_seq;
__xdata __at (0x01af) volatile u8 mb_cec_rx_len;
__xdata __at (0x01b0) volatile u8 mb_cec_rx[16];
/* Absolute so standby survives no CRT clear and ucsim can arm it. */
__xdata __at (0x01c0) volatile u8 soft_standby;
__xdata __at (0x01c1) volatile u8 cec_rx_armed;
/* CK610 bumps the sequence to send Image View On and Active Source. */
__xdata __at (0x01c2) volatile u8 mb_cec_announce_seq;
__xdata __at (0x01c3) volatile u8 mb_cec_announce_ack;
/* Falling edges seen on P0.5 while awake.  CK610 prints a change. */
__xdata __at (0x01c4) volatile u8 mb_cec_edges;
__xdata __at (0x01c5) volatile u8 cec_tx_len;
__xdata __at (0x01c6) volatile u8 cec_tx_buf[8];
__xdata __at (0x01ce) volatile u8 cec_tx_last;
/* Ticks of the last low pulse measured as a candidate start bit. */
__xdata __at (0x01d0) volatile u8 mb_cec_pulse_lo;
__xdata __at (0x01d1) volatile u8 mb_cec_pulse_hi;
__xdata __at (0x01d2) volatile u8 mb_cec_fail;
__xdata __at (0x01d3) volatile u8 mb_cec_pulse_seq;
/* Sampled value of the first 16 bit cells of the last frame. */
__xdata __at (0x01d4) volatile u8 cec_low_log[16];

/* Bit-banged transmit result: count, ACK-slot-low mask (bit n = byte n), length. */
__xdata __at (0x01f0) volatile u8 mb_tx_seq;
__xdata __at (0x01f1) volatile u8 mb_tx_acks;
__xdata __at (0x01f2) volatile u8 mb_tx_len;

/* ABI 1.11 edge recorder.  CK610 bumps the request, 8051 fills the buffer. */
__xdata __at (0x01e4) volatile u8 mb_edge_req;
__xdata __at (0x01e8) volatile u8 mb_edge_ack;
__xdata __at (0x01e9) volatile u8 mb_edge_count;
/* bit0: no frame before the timeout, bit1: buffer full. */
__xdata __at (0x01ea) volatile u8 mb_edge_flags;
/* Timer0 ticks per millisecond, little endian (2000 at 24 MHz). */
__xdata __at (0x01ec) volatile u8 mb_cec_rate_lo;
__xdata __at (0x01ed) volatile u8 mb_cec_rate_hi;
/* Duration of each level in units of 32 ticks; entry 0 is the start low. */
#define EDGE_MAX		96
__xdata __at (0x00a0) volatile u8 mb_edges[EDGE_MAX];

__xdata __at (0x8001) volatile u8 cec_reg_8001;
__xdata __at (0x8004) volatile u8 cec_reg_cmd;
__xdata __at (0x8006) volatile u8 cec_reg_fmt;
__xdata __at (0x8007) volatile u8 cec_reg_opcode;
__xdata __at (0x8026) volatile u8 cec_reg_8026;
__xdata __at (0x8027) volatile u8 cec_reg_8027;
__xdata __at (0x8028) volatile u8 cec_reg_start;

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
static u8 wake_btn_armed;
static u8 wake_key_code;
/* 0x49 on HD2015, 0x4F on FD650B.  Chosen once so the poll never sends both. */
static u8 key_cmd = TM1650_KEY_ADDR;
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
static volatile u8 cec_done_flag;
static u8 cec_ready;
static u8 cec_seq_seen;
static u8 wake_cfg_seen;
static u8 cec_rx_seen;
static u8 cec_announce_seen;
static u8 edge_seen;
static u8 timer1_reload_hi = TIMER1_RELOAD_HIGH;
static u8 timer1_reload_lo = TIMER1_RELOAD_LOW;
static u8 key_div;
static __bit cec_line_low;

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

#if defined(GX6706_LPC)
#define panel_or(mask)		(P0 |= (mask))
#define panel_and(mask)		(P0 &= (u8)~(mask))
#define panel_dat_is_high()	(P0 & PANEL_DAT)
#define PANEL_PORT_ENABLE	GX_P0_ENABLE
#define PANEL_PORT_MODE		GX_P0_MODE
#else
#define panel_or(mask)		(P1 |= (mask))
#define panel_and(mask)		(P1 &= (u8)~(mask))
#define panel_dat_is_high()	(P1 & PANEL_DAT)
#define PANEL_PORT_ENABLE	GX_P1_ENABLE
#define PANEL_PORT_MODE		GX_P1_MODE
#endif

static void clk_high(void)
{
	panel_or(PANEL_CLK);
}

static void clk_low(void)
{
	panel_and(PANEL_CLK);
}

static void dat_high(void)
{
	panel_or(PANEL_DAT);
}

static void dat_low(void)
{
	panel_and(PANEL_DAT);
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

/* Read one key-scan byte.  DAT is briefly an input.  cmd is 0x49 or 0x4F. */
static u8 tm1650_read_cmd(u8 cmd)
{
	u8 value = 0;
	u8 bit;

	tm1650_start();
	tm1650_write_byte(cmd);
	dat_high();
	/* Mode-1 input on DAT while CLK stays an output. */
	PANEL_PORT_MODE |= PANEL_DAT;
	bus_delay();
	for (bit = 0; bit != 8; bit++) {
		value <<= 1;
		clk_high();
		bus_delay();
		if (panel_dat_is_high())
			value |= 1;
		clk_low();
		bus_delay();
	}
	PANEL_PORT_MODE &= (u8)~PANEL_DAT;
	tm1650_stop();
	return value;
}

/*
 * FD650B drives a byte for command 0x4F.  0xff is a floating line, which is
 * what an HD2015 does with that command.  One probe, then the poll uses only
 * the winner: 0x49 on the FD650 latches a mode byte (sleep / dim) and is not
 * a key read.
 */
static void panel_key_select(void)
{
	u8 n;

	key_cmd = TM1650_KEY_ADDR;
	for (n = 0; n != 3; n++) {
		if (tm1650_read_cmd(TM1650_KEY_ADDR_FD650) != 0xff) {
			key_cmd = TM1650_KEY_ADDR_FD650;
			return;
		}
	}
}

static u8 tm1650_read_key(void)
{
	return tm1650_read_cmd(key_cmd);
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
	/*
	 * This image never runs again after the CK610 cold-boots, but it stays
	 * in the LPC.  Say so, so U-Boot reloads it instead of trusting a
	 * stale ready status.
	 */
	mb_status = GX_LPC_STATUS_WOKEN;
	IE &= (u8)~INTERRUPTS_ENABLE;
	GX_SYS_CTL |= GX_SYS_LOW_POWER_ENABLE | GX_SYS_CK610_POWER_OFF;
	for (;;)
		;
}

/*
 * Soft standby: live dimmed clock.  Cold-boot on NEC IR power, TM1650 power
 * key, or RTC wake (countdown / absolute alarm).  Button/IR require a quiet
 * sample first so the press that entered standby does not wake.
 *
 * Shared cecmode does not cold-boot by itself.  Mode 1 posts Image View On
 * on the LPC engine, then cold-boots, when the panel or IR power key leaves
 * standby.  A received Set Stream Path, Active Source, or directed view-on
 * frame cold-boots without posting Image View On: the TV is already on.
 */
/*
 * Vendor LPC CEC engine (gxlowpower.fw CODE:148a / CODE:14b5 / CODE:14ce).
 * cecmode is the 32-bit word at shared 0x90.  Mode 1 or 2 muxes P0.5 and can
 * post System Standby.  Mode 1 posts Image View On before a box-power cold boot.
 * A set completion flag means the engine already finished, so that wake skips
 * a second Image View On, matching CODE:1728.
 */
static u8 cec_mode(void)
{
	if (vendor_cecmode1 || vendor_cecmode2 || vendor_cecmode3)
		return 0xff;
	return vendor_cecmode0;
}

static u8 cec_clock_is_24mhz(void)
{
	return vendor_clock0 == 0x00 && vendor_clock1 == 0x36 &&
	       vendor_clock2 == 0x6e && vendor_clock3 == 0x01;
}

/* Timer0 ticks in us microseconds: 2.0 ticks/us at 24 MHz, 2.25 at 27 MHz. */
static u16 cec_us(u16 us)
{
	u16 ticks = us << 1;

	if (!cec_clock_is_24mhz())
		ticks += us >> 2;
	return ticks;
}

static void cec_rates_init(void)
{
	u16 tpm = cec_us(1000);

	mb_cec_rate_lo = (u8)tpm;
	mb_cec_rate_hi = (u8)(tpm >> 8);
}

static void cec_pin_engine(void)
{
	GX_P0_ENABLE &= (u8)~CEC_PIN;
	GX_P0_MODE &= (u8)~CEC_PIN;
	GX_P0_ENABLE |= CEC_PIN;
}

static void cec_pin_gpio(void)
{
	/* Quasi-bidirectional input: a written one releases the open-drain line. */
	GX_P0_ENABLE &= (u8)~CEC_PIN;
	GX_P0_MODE &= (u8)~CEC_PIN;
	P0 |= CEC_PIN;
}

static void cec_engine_init(void)
{
	if (cec_ready)
		return;
	GX_CEC_CLK |= 0x18;
	if (cec_clock_is_24mhz())
		GX_CEC_CLK |= 0x02;
	IE |= EXT1_INTERRUPT;
	GX_CEC_ICTL = 0x7f;
	cec_reg_8001 = 0;
	TCON |= EXT1_EDGE_TRIGGERED;
	cec_pin_engine();
	cec_reg_8027 = 0;
	cec_ready = 1;
}

static void cec_post(u8 opcode)
{
	cec_engine_init();
	cec_pin_engine();
	cec_reg_cmd = 0x02;
	cec_reg_fmt = 0x10;
	cec_reg_opcode = opcode;
	cec_reg_start = 0x03;
}

static void cec_delay(u16 outer)
{
	u16 inner_reload = cec_clock_is_24mhz() ? 0x015f : 0x018b;

	while (outer) {
		u16 inner = inner_reload;

		while (inner)
			inner--;
		outer--;
	}
}

static void cec_post_standby(void)
{
	u8 mode = cec_mode();

	if (mode != 1 && mode != 2)
		return;
	/*
	 * Box is entering standby.  Tell the TV, then forget any completion
	 * flag so the later power-key path still posts Image View On.
	 */
	cec_done_flag = 0;
	cec_post(CEC_OP_STANDBY);
	cec_delay(0x03e8);
	cec_done_flag = 0;
}

static void cec_view_on_then_poweroff(void)
{
	if (cec_mode() == 1) {
		cec_done_flag = 0;
		cec_post(CEC_OP_IMAGE_VIEW_ON);
		cec_delay(0x03e8);
	}
	enter_destructive_poweroff();
}

static void cec_service(void)
{
	u8 seq = mb_cec_sequence;

	/*
	 * Do not switch P0.5 to GPIO while the CK610 is awake.  The HDMI CEC
	 * wire is the LPC engine's alternate function.  GPIO mode releases it,
	 * and gxcec poll then sees no remote traffic.  Stay on the engine and
	 * publish register changes instead.
	 */
	if (seq == cec_seq_seen)
		return;
	cec_seq_seen = seq;
	if (!mb_cec_opcode) {
		mb_cec_ack = seq;
		return;
	}
	cec_post(mb_cec_opcode);
	mb_cec_ack = seq;
	/* Same wait as the vendor power-key path, so the frame leaves the pin. */
	cec_delay(0x03e8);
	if (!cec_rx_armed)
		cec_pin_gpio();
}

static u16 cec_now(void)
{
	u8 hi;
	u8 lo;

	/* Reread when TL0 carried into TH0 between the two reads. */
	do {
		hi = TH0;
		lo = TL0;
	} while (hi != TH0);
	return ((u16)hi << 8) | lo;
}

static u8 cec_pin_high(void)
{
	return (P0 & CEC_PIN) != 0;
}

/*
 * Ticks the pin stays at level (1 = high), capped at limit.  A result below
 * limit means the pin changed; a result of limit means it did not.  Timer0
 * must be free running (EXT0 and Timer0 interrupts masked).
 */
static u16 cec_hold(u8 level, u16 limit)
{
	u16 start = cec_now();
	u16 t;

	while (cec_pin_high() == level) {
		t = cec_now() - start;
		if (t >= limit)
			return limit;
	}
	return (u16)(cec_now() - start);
}

static void cec_wait_until(u16 start, u16 ticks)
{
	while ((u16)(cec_now() - start) < ticks)
		;
}

/* 1 once the bus has been high for CEC_US_IDLE.  Bounded by the longest frame. */
static u8 cec_wait_idle(void)
{
	u8 n;

	for (n = 0; n != 250; n++) {
		if (cec_hold(1, CEC_T_IDLE) >= CEC_T_IDLE)
			return 1;
		if (!cec_pin_high())
			cec_hold(0, CEC_T_START_MAX);
	}
	return 0;
}

/*
 * Logical addresses we answer to: the LPC header 0x10 (1), the default tuner
 * address 3 and whatever U-Boot claimed.
 */
static u8 cec_is_mine(u8 dest)
{
	if (dest == 0x0f)
		return 0;
	return dest == 1 || dest == 3 ||
	       (mb_cec_la < 0x0f && dest == mb_cec_la);
}

static u8 cec_frame_wakes(void)
{
	u8 len = mb_cec_rx_len;
	u8 dest;
	u8 op;

	if (cec_mode() != 1 && cec_mode() != 2)
		return 0;
	if (len < 2)
		return 0;
	dest = mb_cec_rx[0] & 0x0f;
	op = mb_cec_rx[1];
	if (op == CEC_OP_STANDBY)
		return 0;
	if (op == CEC_OP_IMAGE_VIEW_ON || op == CEC_OP_TEXT_VIEW_ON)
		return cec_is_mine(dest);
	/* Routing Change: 0f 80 <old PA> <new PA>.  The TV sends it on a source switch. */
	if (op == CEC_OP_ROUTING_CHANGE) {
		if (!mb_cec_pa_valid || len < 6)
			return 0;
		return mb_cec_rx[4] == mb_cec_pa_hi && mb_cec_rx[5] == mb_cec_pa_lo;
	}
	if (op != CEC_OP_SET_STREAM_PATH && op != CEC_OP_ACTIVE_SOURCE)
		return 0;
	if (!mb_cec_pa_valid || len < 4)
		return 0;
	return mb_cec_rx[2] == mb_cec_pa_hi && mb_cec_rx[3] == mb_cec_pa_lo;
}

static void cec_cold_boot(void)
{
	vendor_suspend_reason = 3;
	enter_destructive_poweroff();
}

static void cec_note_frame(u8 len)
{
	mb_cec_rx_len = len;
	mb_cec_rx_seq++;
	if (!mb_cec_rx_seq)
		mb_cec_rx_seq = 1;
}

static void cec_pulse_note(void)
{
	mb_cec_pulse_seq++;
	if (!mb_cec_pulse_seq)
		mb_cec_pulse_seq = 1;
}

/*
 * Decode one frame; the pin is low on entry.  Each bit cell is timed from
 * its falling edge: sample at 1.05 ms (low is 0, high is 1), ignore every
 * edge until 1.9 ms, then take the next falling edge as the next cell.  Bytes
 * are MSB first, slot 8 is EOM and slot 9 is the ACK, which is driven low
 * for frames addressed to us.  Timer1 keeps running; EXT0 and Timer0 are
 * masked because their handlers zero Timer0.  Returns 1 when a byte was stored.
 */
static u8 cec_rx_frame(void)
{
	u8 saved;
	u8 tries;
	u8 n;
	u8 eom;
	u8 byte;
	u8 got;
	u8 level;
	u16 w;
	u16 cs;
	u16 t_sample;
	u16 t_ack;
	u16 t_cell_min;
	u16 t_cell_wait;

	cec_rates_init();
	t_sample = CEC_T_SAMPLE;
	t_ack = CEC_T_ACK;
	t_cell_min = CEC_T_CELL_MIN;
	t_cell_wait = CEC_T_CELL_WAIT;
	saved = IE;
	IE &= (u8)~(EXT0_INTERRUPT | TIMER0_INTERRUPT | EXT1_INTERRUPT);
	for (tries = 0; tries != 3; tries++) {
		w = cec_hold(0, CEC_T_START_MAX);
		mb_cec_pulse_lo = (u8)w;
		mb_cec_pulse_hi = (u8)(w >> 8);
		if (w >= CEC_T_START_MIN && w < CEC_T_START_MAX)
			break;
		/* Joined mid-frame, or the line is stuck low: wait for the next frame. */
		if (w >= CEC_T_START_MAX || !cec_wait_idle() ||
		    cec_hold(1, CEC_T_WAIT_START) >= CEC_T_WAIT_START) {
			IE = saved;
			return 0;
		}
	}
	if (tries == 3) {
		IE = saved;
		return 0;
	}
	mb_cec_fail = 0;
	cec_tx_len = 0;
	if (cec_hold(1, CEC_T_START_HIGH_MAX) >= CEC_T_START_HIGH_MAX) {
		mb_cec_fail = 4;
		cec_pulse_note();
		IE = saved;
		return 0;
	}
	n = 0;
	eom = 0;
	byte = 0;
	got = 0;
	for (;;) {
		cs = cec_now();
		if (n == 9) {
			if (got && cec_is_mine(mb_cec_rx[0] & 0x0f)) {
				P0 &= (u8)~CEC_PIN;
				cec_wait_until(cs, t_ack);
				P0 |= CEC_PIN;
			}
			n = 0;
			byte = 0;
			if (eom || got == 16)
				break;
		} else {
			cec_wait_until(cs, t_sample);
			level = cec_pin_high();
			if (cec_tx_len < 16) {
				cec_low_log[cec_tx_len] = level;
				cec_tx_len++;
			}
			if (n < 8) {
				byte = (u8)((byte << 1) | level);
				n++;
				if (n == 8) {
					mb_cec_rx[got] = byte;
					got++;
				}
			} else {
				eom = level;
				n = 9;
			}
		}
		cec_wait_until(cs, t_cell_min);
		if (cec_pin_high() &&
		    cec_hold(1, t_cell_wait) >= t_cell_wait) {
			mb_cec_fail = 5;
			break;
		}
	}
	IE = saved;
	cec_pulse_note();
	if (got) {
		cec_note_frame(got);
		return 1;
	}
	if (!mb_cec_fail)
		mb_cec_fail = 6;
	return 0;
}

/* Watch the bus for one RTC tick and decode a frame that starts in it. */
static u8 cec_watch(void)
{
	u8 tick = rtc_subsecond;
	u16 spins = 0;

	/* The spin bound keeps a stopped Timer1 from hanging the main loop. */
	while (rtc_subsecond == tick) {
		if (!cec_pin_high())
			return cec_rx_frame();
		if (++spins == 0)
			break;
	}
	return 0;
}

static void cec_reply_service(void);

static void cec_rx_service(void)
{
	if (!soft_standby || !cec_rx_armed)
		return;
	if (mb_cec_rx_seq == cec_rx_seen)
		cec_watch();
	if (mb_cec_rx_seq != cec_rx_seen) {
		cec_rx_seen = mb_cec_rx_seq;
		if (cec_frame_wakes())
			cec_cold_boot();
		cec_reply_service();
	}
}

static void cec_wait_ticks(u16 ticks)
{
	u16 start = cec_now();

	while ((u16)(cec_now() - start) < ticks)
		;
}

static void cec_tx_bit(u8 one)
{
	P0 &= (u8)~CEC_PIN;
	cec_wait_ticks(one ? CEC_T_TX_BIT1_LOW : CEC_T_TX_BIT0_LOW);
	P0 |= CEC_PIN;
	cec_wait_ticks(one ? CEC_T_TX_BIT1_HIGH : CEC_T_TX_BIT0_HIGH);
}

/* Returns the level of the ACK slot at 1.05 ms: low means a follower answered. */
static u8 cec_tx_byte(u8 value)
{
	u8 i;
	u8 level;
	u16 start;

	for (i = 0; i != 8; i++) {
		cec_tx_bit((value & 0x80) != 0);
		value <<= 1;
	}
	cec_tx_bit(cec_tx_last);
	/* Initiator sends a 1 and releases.  A follower holds the line low. */
	start = cec_now();
	P0 &= (u8)~CEC_PIN;
	cec_wait_ticks(CEC_T_TX_BIT1_LOW);
	P0 |= CEC_PIN;
	cec_wait_until(start, CEC_T_SAMPLE);
	level = cec_pin_high();
	cec_wait_until(start, CEC_T_TX_BIT1_LOW + CEC_T_TX_BIT1_HIGH);
	return level;
}

/* P0.5 GPIO.  The CEC engine has no operand bytes, so Active Source cannot use it. */
static void cec_tx_bytes(void)
{
	u8 saved;
	u8 n;
	u8 acks;

	if (!cec_tx_len || cec_tx_len > 8)
		return;
	saved = IE;
	IE &= (u8)~(EXT0_INTERRUPT | TIMER0_INTERRUPT | EXT1_INTERRUPT |
		    TIMER1_INTERRUPT);
	cec_rates_init();
	cec_pin_gpio();
	cec_wait_ticks(CEC_T_SIGNAL_FREE);
	cec_wait_ticks(CEC_T_SIGNAL_FREE);
	P0 &= (u8)~CEC_PIN;
	cec_wait_ticks(CEC_T_TX_START_LOW);
	P0 |= CEC_PIN;
	cec_wait_ticks(CEC_T_TX_START_HIGH);
	acks = 0;
	for (n = 0; n != cec_tx_len; n++) {
		cec_tx_last = (u8)(n + 1 == cec_tx_len);
		if (!cec_tx_byte(cec_tx_buf[n]))
			acks |= (u8)(1u << n);
	}
	IE = saved;
	mb_tx_acks = acks;
	mb_tx_len = cec_tx_len;
	mb_tx_seq++;
}

/*
 * The TV polls the box with Give Device Power Status and retries until it
 * hears Report Power Status.  Answer as the address it used.  A box in
 * soft standby reports standby.  Awake means the CK610 is running and
 * driving video, so it reports on: a TV keeps re-polling a device that
 * says "in transition".
 */
static void cec_reply_service(void)
{
	u8 init;
	u8 dest;

	if (mb_cec_rx_len != 2 || mb_cec_rx[1] != CEC_OP_GIVE_POWER)
		return;
	init = mb_cec_rx[0] >> 4;
	dest = mb_cec_rx[0] & 0x0f;
	if (init == 0x0f || !cec_is_mine(dest))
		return;
	cec_tx_buf[0] = (u8)((dest << 4) | init);
	cec_tx_buf[1] = CEC_OP_REPORT_POWER;
	cec_tx_buf[2] = soft_standby ? CEC_POWER_STANDBY : CEC_POWER_ON;
	cec_tx_len = 3;
	cec_tx_bytes();
}

static void cec_send_active_source(void)
{
	cec_tx_buf[0] = 0x1f;
	cec_tx_buf[1] = CEC_OP_ACTIVE_SOURCE;
	cec_tx_buf[2] = mb_cec_pa_hi;
	cec_tx_buf[3] = mb_cec_pa_lo;
	cec_tx_len = 4;
	cec_tx_bytes();
}

static void cec_announce_service(void)
{
	if (mb_cec_announce_seq == cec_announce_seen)
		return;
	cec_announce_seen = mb_cec_announce_seq;
	if (cec_mode() != 1 && cec_mode() != 2)
		return;
	/* Header 0x10.  This is the transmit path the TV actually acknowledges. */
	cec_post(CEC_OP_IMAGE_VIEW_ON);
	cec_delay(0x03e8);
	if (mb_cec_pa_valid)
		cec_send_active_source();
	cec_pin_gpio();
	mb_cec_announce_ack = cec_announce_seen;
}

static void cec_note_edge(void)
{
	if (cec_pin_high()) {
		cec_line_low = 0;
		return;
	}
	if (cec_line_low)
		return;
	cec_line_low = 1;
	mb_cec_edges++;
}

/* Returns 1 while the bus is being watched, so the main loop can slow its key poll. */
static u8 cec_awake_listen(void)
{
	if (soft_standby)
		return 0;
	if (vendor_cecmode1 || vendor_cecmode2 || vendor_cecmode3)
		return 0;
	if (vendor_cecmode0 != 1 && vendor_cecmode0 != 2)
		return 0;
	cec_pin_gpio();
	cec_note_edge();
	if (cec_watch())
		cec_reply_service();
	return 1;
}

static void cec_arm_listen(void)
{
	if (cec_mode() == 1 || cec_mode() == 2) {
		cec_engine_init();
		cec_pin_gpio();
		cec_rx_seen = mb_cec_rx_seq;
		cec_rx_armed = 1;
	} else {
		cec_rx_armed = 0;
	}
}

static void cec_capture_regs(u8 stat)
{
	u8 __xdata *regs = (__xdata u8 *)0x8000;
	u8 i;

	if (!(stat & (u8)~0x02))
		return;
	mb_cec_snap_istat = stat;
	for (i = 0; i != 48; i++)
		mb_cec_snap[i] = regs[i];
	mb_cec_snap_seq++;
}

/*
 * Record how long the pin stays low and high through the next frame, so the
 * CK610 can print the raw waveform.  Entry 0 is the start-bit low.  The
 * capture starts on the first falling edge after the bus has been idle and
 * ends at the next idle period or when the buffer is full.
 */
static void cec_edge_service(void)
{
	u8 seq = mb_edge_req;
	u8 saved;
	u8 n;
	u8 waits;
	u8 level;
	u16 w;

	if (seq == edge_seen)
		return;
	edge_seen = seq;
	cec_rates_init();
	mb_edge_count = 0;
	mb_edge_flags = 0;
	saved = IE;
	IE &= (u8)~(EXT0_INTERRUPT | TIMER0_INTERRUPT | EXT1_INTERRUPT);
	cec_pin_gpio();
	for (waits = 0; waits != 150; waits++) {
		if (cec_hold(1, CEC_T_IDLE) >= CEC_T_IDLE) {
			if (cec_hold(1, CEC_T_WAIT_START) < CEC_T_WAIT_START)
				break;
		} else if (!cec_pin_high()) {
			cec_hold(0, CEC_T_START_MAX);
		}
	}
	if (waits == 150) {
		mb_edge_flags = 1;
		IE = saved;
		mb_edge_ack = seq;
		return;
	}
	level = 0;
	for (n = 0; n != EDGE_MAX; n++) {
		w = cec_hold(level, CEC_T_IDLE);
		mb_edges[n] = (w >> 5) > 255 ? 255 : (u8)(w >> 5);
		if (w >= CEC_T_IDLE) {
			n++;
			break;
		}
		level ^= 1;
	}
	if (n == EDGE_MAX)
		mb_edge_flags = 2;
	mb_edge_count = n;
	IE = saved;
	mb_edge_ack = seq;
}

static void wake_config_service(void)
{
	if (mb_wake_sequence == wake_cfg_seen)
		return;
	wake_cfg_seen = mb_wake_sequence;
	mb_wake_ack = mb_wake_sequence;
}

static void soft_standby_poll(void)
{
	u8 ir_hit;
	u8 key;
	u8 power_down;
	u8 rtc_hit;

	if (!soft_standby)
		return;

	cec_rx_service();

	rtc_hit = soft_wake_due ||
		  (soft_wake_rtc && !soft_wake_left && alarm_active);
	if (rtc_hit) {
		soft_wake_due = 0;
		vendor_suspend_reason = 2;
		cec_view_on_then_poweroff();
	}

	ir_hit = ir_power_hit;
	if (ir_hit)
		ir_power_hit = 0;
	/*
	 * While the CEC pin is watched, the TM1650 read (about 4 ms) is kept
	 * rare so it does not swallow a 3.7 ms start bit.
	 */
	if (cec_rx_armed && !ir_hit) {
		if (++key_div < 5)
			return;
	}
	key_div = 0;
	key = tm1650_read_key();
	mb_last_key = key;
	/* Stock presses always count.  A programmed override is extra. */
	power_down = (key == TM1650_KEY_POWER || key == TM1650_KEY_POWER_ALT);
#if defined(GX6706_LPC)
	/* FD650 board xml lists KEY_PW as 71 (0x47), already with the press bit. */
	if (key == GX_LPC_KEY_POWER_EXTRA)
		power_down = 1;
#endif
	if (wake_key_code && key == wake_key_code)
		power_down = 1;
	if (!ir_hit && !power_down) {
		wake_btn_armed = 1;
		return;
	}
	if (!wake_btn_armed)
		return;

	vendor_suspend_reason = 1;
	cec_view_on_then_poweroff();
}

static u8 ir_code_is_power(u16 code)
{
	u8 n, i;
	u16 extra;

	if (code == IR_POWER_REMOTE1 ||
	    code == IR_POWER_REMOTE2 ||
	    code == IR_POWER_TELEFUNKEN ||
	    code == IR_POWER_BOARD ||
	    code == IR_POWER_ALT_STB)
		return 1;
	n = mb_wake_ir_count;
	if (n > 4)
		n = 4;
	for (i = 0; i < n; i++) {
		extra = mb_wake_ir[i * 2] | ((u16)mb_wake_ir[i * 2 + 1] << 8);
		if (extra && extra == code)
			return 1;
	}
	return 0;
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
	GX_P1_ENABLE |= GX_PMU_POWER_CUT_GPIO;
	GX_P1_MODE &= (u8)~GX_PMU_POWER_CUT_GPIO;
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
#if !defined(GX6706_LPC)
	/*
	 * A shared-XDATA snapshot taken immediately after genuine stock
	 * standby contains mask 0xfffff7ff and data 0x00000800: GPIO 11 is
	 * the sole retained output and it is high.  Vendor gpio_write(11, 1)
	 * maps that pin to P1 bit 3, selects mode 0 by clearing both mode
	 * registers, then raises the output latch.  The GX6706 gpio.xml has
	 * no retention pin, so that board leaves P1.3 alone.
	 */
	GX_P1_ENABLE &= (u8)~GX6702_RETENTION_GPIO;
	GX_P1_MODE &= (u8)~GX6702_RETENTION_GPIO;
	P1 |= GX6702_RETENTION_GPIO;
#endif
}

static void wake_input_init(void)
{
	/*
	 * Vendor init at 0x00fb conditions logical GPIO 0 before it starts
	 * polling for a power event.  GPIO 0 is P0.0 in mode 0, driven high;
	 * external interrupt 0 is edge-triggered and Timer 0 measures the
	 * interval between IR demod falling edges for NEC decode.
	 */
#if defined(GX6706_LPC)
	/* Vendor init passes logical GPIO 2 (P0.2) before arming EXT0. */
	GX_P0_ENABLE &= (u8)~GX_WAKE_INPUT_GPIO;
	GX_P0_MODE &= (u8)~GX_WAKE_INPUT_GPIO;
	P0 |= GX_WAKE_INPUT_GPIO;
#else
	GX_P0_ENABLE &= (u8)~GX6702_WAKE_INPUT_GPIO;
	GX_P0_MODE &= (u8)~GX6702_WAKE_INPUT_GPIO;
	P0 |= GX6702_WAKE_INPUT_GPIO;
#endif
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
	cec_post_standby();

	if (mb_suspend_control & GX_LPC_SUSPEND_NO_POWEROFF) {
		/*
		 * CK610 is in STOP.  With KEEP_8051, both LPC power bits remain
		 * clear; return to main so Timer1 keeps the HH:MM display alive
		 * and soft_standby_poll() cold-boots on key or RTC wake.
		 */
		wake_key_code = mb_suspend_reserved0;
		if (!wake_key_code)
			wake_key_code = mb_wake_panel;
		if (!wake_key_code)
			wake_key_code = TM1650_KEY_POWER;
		soft_standby = 1;
		wake_btn_armed = 0;
		scroll_active = 0;
		cec_arm_listen();
		IE = saved_ie;
		return;
	}

	/* Vendor: DisableGlobalInterrupts then EnterDestructiveSuspend. */
	enter_destructive_poweroff();
}

void ext1_isr(void) __interrupt (2)
{
	u8 stat = GX_CEC_ISTAT;

	cec_capture_regs(stat);
	if (stat & 0x02) {
		if (cec_reg_8027) {
			cec_done_flag = 1;
			cec_reg_8027 = 0;
		}
		cec_reg_8026 = 0;
	}
	GX_CEC_ISTAT = 0x7f;
}

void timer1_isr(void) __interrupt (3)
{
	TH1 = timer1_reload_hi;
	TL1 = timer1_reload_lo;
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
	if (cec_clock_is_24mhz()) {
		timer1_reload_hi = TIMER1_RELOAD_HIGH_24;
		timer1_reload_lo = TIMER1_RELOAD_LOW_24;
	}
	TH1 = timer1_reload_hi;
	TL1 = timer1_reload_lo;
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

	/* Idle the panel bus high in GPIO output mode. */
	PANEL_PORT_ENABLE &= (u8)~PANEL_PINS;
	PANEL_PORT_MODE &= (u8)~PANEL_PINS;
	panel_or(PANEL_PINS);
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
	/* Absolute XDATA is not cleared by the CRT.  A stale byte must not
	 * become the only accepted wake code.
	 */
	mb_wake_panel = 0;
	mb_wake_sequence = 0;
	mb_wake_ack = 0;
	mb_wake_ir_count = 0;
	mb_cec_pa_valid = 0;
	mb_cec_pa_hi = 0;
	mb_cec_pa_lo = 0;
	mb_cec_la = 0xff;
	mb_cec_snap_seq = 0;
	mb_cec_snap_istat = 0;
	mb_cec_rx_seq = 0;
	mb_cec_rx_len = 0;
	soft_standby = 0;
	cec_rx_armed = 0;
	mb_cec_announce_seq = 0;
	mb_cec_announce_ack = 0;
	mb_cec_edges = 0;
	mb_cec_pulse_lo = 0;
	mb_cec_pulse_hi = 0;
	mb_cec_fail = 0;
	mb_cec_pulse_seq = 0;
	mb_tx_seq = 0;
	mb_tx_acks = 0;
	mb_tx_len = 0;
	edge_seen = mb_edge_req;
	mb_edge_ack = edge_seen;
	mb_edge_count = 0;
	mb_edge_flags = 0;
	cec_rates_init();
	mb_suspend_reserved0 = 0;
	scroll_active = 0;
	scroll_length = 0;
	scroll_position = 0;

	hardware_init();
	panel_key_select();
	rtc_init();
	/*
	 * Do not post System Standby here.  This image starts while the main
	 * CPU is awake.  Standby opcode 0x36 is sent only from suspend entry;
	 * posting it at boot collides with a following Image View On.
	 */
	mb_status = GX_LPC_STATUS_READY;

	for (;;) {
		u8 cec_watching;

		soft_standby_poll();
		cec_service();
		cec_announce_service();
		cec_edge_service();
		cec_watching = cec_awake_listen();
		wake_config_service();
		if (!soft_standby && (!cec_watching || ++key_div >= 5)) {
			key_div = 0;
			mb_last_key = tm1650_read_key();
		}
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
