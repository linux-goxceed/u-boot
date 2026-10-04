/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __GX_ETH_H__
#define __GX_ETH_H__

/*
 * GX6702 RMII pad setup clears bits 6 and 7 and sets bit 8 of
 * 0xA030A1B0 before the Synopsys GMAC at 0xA0A00000 is used.
 */
void gx_eth_enable(void);

#endif /* __GX_ETH_H__ */
