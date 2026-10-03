/* SPDX-License-Identifier: MIT */
#ifndef GX6702_LPC_MAILBOX_H
#define GX6702_LPC_MAILBOX_H

/*
 * 8051 XDATA offsets.  The CK610 sees the same bytes at 0xa4d00000 + offset.
 * Bytes 0x0100..0x0107 are command data written by U-Boot using aligned words;
 * bytes 0x0108..0x010f are firmware-owned status.
 *
 * LXDVB501 eCos LPC setup at 0x90348d5c stores cecmode (strtol, base 10) as a
 * 32-bit word at offset 0x90 of the 0x98-byte shared struct, CK610
 * 0xA4D00090.  The matching store is mov r7,r6; addu r7,r14; st.w r2,(r7,0)
 * at file 0x349024, and the r6 immediate 0x90 is the literal at file
 * 0x348fc0.  Neighbouring words are timesummer at 0x88 and curtime at 0x8c.
 * The vendor 8051 adds 0x90 to the struct base in IRAM 0x4F/0x50.  Mode 1 or 2
 * connects P0.5 to the LPC CEC engine and posts System Standby (0x36) when
 * that image starts.  Mode 1 posts Image View On (0x04) before a power-key
 * cold boot.  ABI 1.8 also accepts an explicit post at mailbox 0x0168.
 * ABI 1.10 stores the HDMI physical address at mailbox 0x0178 so a received
 * Set Stream Path or Active Source can cold-boot while the CK610 is stopped.
 */
#define GX_LPC_MB_UPDATE	0x01

#define GX_LPC_STATUS_BOOTING	0x42
#define GX_LPC_STATUS_READY	0xa5

#define GX_LPC_ABI_MAJOR	1
#define GX_LPC_ABI_MINOR	10

#define GX_LPC_CAP_DISPLAY	0x01
#define GX_LPC_CAP_BRIGHTNESS	0x02
#define GX_LPC_CAP_AUX		0x04
#define GX_LPC_CAP_SCROLL	0x08
#define GX_LPC_CAP_RTC		0x10
#define GX_LPC_CAP_ALARM	0x20
#define GX_LPC_CAP_SUSPEND	0x40

#define GX_LPC_SCROLL_ENABLE	0x01
#define GX_LPC_SCROLL_MAX	32
#define GX_LPC_SCROLL_GAP	4
#define GX_LPC_SCROLL_PERIOD	4

#define GX_LPC_RTC_DISPLAY	0x01
#define GX_LPC_RTC_TIME_VALID	0x02

#define GX_LPC_RTC_STATUS_RUNNING	0x01
#define GX_LPC_RTC_STATUS_DISPLAY	0x02
#define GX_LPC_RTC_STATUS_TIME_VALID	0x04

#define GX_LPC_ALARM_ARMED	0x01
#define GX_LPC_ALARM_STATUS_ARMED	0x01
#define GX_LPC_ALARM_STATUS_ACTIVE	0x02

/* Destructive CK610 standby; physical button/IR/RTC wake causes a cold boot. */
#define GX_LPC_SUSPEND_REQUEST		0x01
/*
 * Soft-standby RTC wake with NO_POWEROFF: mb_suspend_seconds > 0 counts down
 * to cold-boot, or seconds == 0 uses an already-armed mailbox alarm time.
 */
#define GX_LPC_SUSPEND_WAKE_ALARM	0x02
/* Soft standby request; bit2 is used only on intentional wake. */
#define GX_LPC_SUSPEND_NO_POWEROFF	0x04
/* Keep the 8051 clocked: do not assert SYS_CTL bit1 while CK610 is in STOP. */
#define GX_LPC_SUSPEND_KEEP_8051	0x08
#define GX_LPC_SUSPEND_GUARD0		0x47
#define GX_LPC_SUSPEND_GUARD1		0x58
#define GX_LPC_SUSPEND_STATUS_PREPARED	0x01
#define GX_LPC_SUSPEND_STATUS_REJECTED	0x02
#define GX_LPC_SUSPEND_MIN_SECONDS	5

#define GX_LPC_ERROR_NONE	0x00
#define GX_LPC_ERROR_RTC_TIME	0x01
#define GX_LPC_ERROR_SUSPEND_GUARD	0x02
#define GX_LPC_ERROR_SUSPEND_RTC	0x03
#define GX_LPC_ERROR_SUSPEND_ALARM	0x04
#define GX_LPC_ERROR_SUSPEND_SOON	0x05

#define GX_LPC_AUX0		0x01
#define GX_LPC_AUX1		0x02
#define GX_LPC_AUX2		0x04
#define GX_LPC_AUX3		0x08
#define GX_LPC_AUX_MASK		0x0f

/* LXDVB501 front-panel aliases established by a one-at-a-time live probe. */
#define GX_LPC_AUX_UNUSED	GX_LPC_AUX0
#define GX_LPC_AUX_CLOCK_COLON	GX_LPC_AUX1
#if defined(GX6706_LPC)
/* UUVision panel: power and status lamps are swapped versus the GX6702. */
#define GX_LPC_AUX_STATUS_GREEN	GX_LPC_AUX3
#define GX_LPC_AUX_POWER	GX_LPC_AUX2
#else
#define GX_LPC_AUX_STATUS_GREEN	GX_LPC_AUX2
#define GX_LPC_AUX_POWER	GX_LPC_AUX3
#endif

#endif
