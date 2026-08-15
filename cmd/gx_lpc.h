/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef GX_LPC_H
#define GX_LPC_H

#include <linux/types.h>
#include <linux/bitops.h>

#define GX_LPC_SHARED		0xA4D00000UL
#define GX_LPC_STATUS_READY	0xa5
#define GX_LPC_ABI_MAJOR	1
#define GX_LPC_ABI_MINOR	7
#define GX_LPC_CAP_DISPLAY	BIT(0)
#define GX_LPC_CAP_BRIGHTNESS	BIT(1)
#define GX_LPC_CAP_AUX		BIT(2)
#define GX_LPC_CAP_SCROLL	BIT(3)
#define GX_LPC_CAP_RTC		BIT(4)
#define GX_LPC_CAP_ALARM	BIT(5)
#define GX_LPC_CAP_SUSPEND	BIT(6)
#define GX_LPC_AUX_MAX		0x0f
#define GX_LPC_AUX_CLOCK_COLON	BIT(1)
#define GX_LPC_AUX_STATUS_GREEN	BIT(2)
#define GX_LPC_AUX_POWER	BIT(3)
#define GX_LPC_SCROLL_MAX	32
#define GX_LPC_SCROLL_ENABLE	BIT(0)
#define GX_LPC_SCROLL_PERIOD	4
#define GX_LPC_RTC_DISPLAY	BIT(0)
#define GX_LPC_RTC_TIME_VALID	BIT(1)
#define GX_LPC_RTC_STATUS_RUNNING BIT(0)
#define GX_LPC_RTC_STATUS_DISPLAY BIT(1)
#define GX_LPC_RTC_STATUS_TIME_VALID BIT(2)
#define GX_LPC_ALARM_ARMED	BIT(0)
#define GX_LPC_ALARM_STATUS_ARMED BIT(0)
#define GX_LPC_ALARM_STATUS_ACTIVE BIT(1)
#define GX_LPC_SUSPEND_REQUEST	BIT(0)
#define GX_LPC_SUSPEND_WAKE_ALARM BIT(1)
#define GX_LPC_SUSPEND_NO_POWEROFF BIT(2)
#define GX_LPC_SUSPEND_KEEP_8051 BIT(3)
#define GX_LPC_SUSPEND_GUARD0	0x47
#define GX_LPC_SUSPEND_GUARD1	0x58
#define GX_LPC_SUSPEND_STATUS_PREPARED BIT(0)
#define GX_LPC_SUSPEND_STATUS_REJECTED BIT(1)
#define GX_LPC_SUSPEND_MIN_SECONDS 5
#define GX_LPC_TM1650_KEY_POWER	0x4f
#define GX_TM1650_BRIGHT_MAX	8

extern const u8 gxopen_fw[];
extern const u8 gxopen_fw_end[];
extern const u8 gx6702_ck610_stop_blob[];
extern const u8 gx6702_ck610_stop_blob_end[];
void gx6702_ck610_enter_isram(void);

extern u8 gx_lpc_wake_key;
extern unsigned int gx_lpc_aux;
extern unsigned int gx_lpc_brightness;
extern unsigned int gx_lpc_scroll_length;

bool gx_lpc8051_open_ready(u8 required_caps);
int gx_lpc_ensure_open(const char *text);
void gx_lpc8051_publish(void);
void gx_lpc8051_write_text(const char *text);
void gx_lpc8051_set_brightness(unsigned int brightness);
void gx_lpc8051_set_aux(unsigned int aux);
unsigned int gx_lpc_get_aux(void);
unsigned int gx_lpc_get_brightness(void);
unsigned int gx_lpc_get_scroll_length(void);

int gx_lpc8051_parse_time(const char *text, u8 *hour, u8 *minute, u8 *second);
int gx_lpc8051_print_rtc(void);
int gx_lpc8051_set_rtc(u8 hour, u8 minute, u8 second);
int gx_lpc8051_set_rtc_display(bool enable);
int gx_lpc8051_set_alarm(u8 hour, u8 minute, u8 second);
int gx_lpc8051_cancel_alarm(void);

int gx_lpc8051_suspend_soft(u32 wake_seconds, bool arm_alarm,
			    u8 alarm_hour, u8 alarm_minute, u8 alarm_second);
int gx_lpc8051_suspend_button(void);
int gx_lpc8051_suspend_after(u32 delay_seconds);
int gx_lpc_ck610_stop_only(void);
int gx_lpc8051_suspend_bit1_only(void);
int gx_lpc8051_probe_keys(void);
int gx_lpc8051_dump_shared(void);

int gx_lpc8051_start(const u8 *fw, ulong fw_size, const char *source,
		     const char *text);

#endif /* GX_LPC_H */
