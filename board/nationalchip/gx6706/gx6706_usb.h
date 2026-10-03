/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __GX6706_USB_H__
#define __GX6706_USB_H__

/*
 * Cygnus USB clock, pad, and PHY trim, shared by generic-ehci and the
 * two OHCI companions. EHCI is at 0xA0904000. OHCI is at 0xA0900000 and
 * 0xA0901000. There is no Gemini PHY bank.
 */
int gx6706_usb_enable(void);
void gx6706_timer_check(void);

#endif /* __GX6706_USB_H__ */
