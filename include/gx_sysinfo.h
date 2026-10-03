/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __GX_SYSINFO_H__
#define __GX_SYSINFO_H__

/*
 * Print "Board: NationalChip GX%.4s (%s %.6s)" from the reversed
 * chip-name register. 6701/6702/6703 are Gemini; 6705/6706 are Cygnus.
 * Falls back when that register is not printable.
 */
void gx_sysinfo_print_board(void);

#endif /* __GX_SYSINFO_H__ */
