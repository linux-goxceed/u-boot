/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __GX6702_USB_H__
#define __GX6702_USB_H__

/*
 * Stage-1 pinmux + A030A pad window (GxLoader 6B61C / USB prologue).
 * Needed for GPIO pads after BootROM→UART upload (skips full GxLoader).
 */
int gx6702_pad_init(void);

/*
 * Enable GX6702 USB host padmux + PHY before EHCI probe.
 * Recovered from gemini-6702H5-sflash-24M.boot @ 0x93CE6620.
 */
int gx6702_usb_enable(void);

#endif /* __GX6702_USB_H__ */
