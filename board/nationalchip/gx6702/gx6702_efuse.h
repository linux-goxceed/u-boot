/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * GX6702 eFuse helpers (read-only).
 *
 * Byte addresses 0..0x7ff are also exposed via the stock fuse command as
 * bank 0 / word == byte address (see gx6702_fuse.c).
 */

#ifndef GX6702_EFUSE_H
#define GX6702_EFUSE_H

#include <linux/types.h>

/**
 * gx6702_efuse_dac_gain() - packed GFX_DAC_GAIN from eFuse words 0x126..0x129
 *
 * Per-channel blank (byte == 0) uses vendor defaults 0x1E/0x1E/0x1E/0x20.
 * If any byte read fails, returns the live-golden fallback 0x1b1b1b1b.
 */
u32 gx6702_efuse_dac_gain(void);

#endif /* GX6702_EFUSE_H */
