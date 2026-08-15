/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __ASM_CSKY_IO_H_
#define __ASM_CSKY_IO_H_

#include <asm/types.h>

static inline void sync(void)
{
	asm volatile("sync" : : : "memory");
}

#define readb(addr)		(*(volatile unsigned char *)(addr))
#define readw(addr)		(*(volatile unsigned short *)(addr))
#define readl(addr)		(*(volatile unsigned int *)(addr))
#define writeb(val, addr)	(*(volatile unsigned char *)(addr) = (val))
#define writew(val, addr)	(*(volatile unsigned short *)(addr) = (val))
#define writel(val, addr)	(*(volatile unsigned int *)(addr) = (val))

#define out_le32(a, v)		writel((v), (a))
#define out_le16(a, v)		writew((v), (a))
#define in_le32(a)		readl(a)
#define in_le16(a)		readw(a)

/*
 * Vendor stage-1 MMU: CPU VA bit31 set for DRAM/IO mirrors.
 * DMA (EHCI) needs the physical bus address (bit31 clear), e.g.
 * 0x93CExxxx -> 0x13CExxxx (DRAM @ 0x10000000), 0xA0904000 -> 0x00904000.
 */
static inline phys_addr_t virt_to_phys(void *vaddr)
{
	return (phys_addr_t)((unsigned long)vaddr & 0x1fffffffUL);
}

#define virt_to_phys virt_to_phys

#include <asm-generic/io.h>

#endif /* __ASM_CSKY_IO_H_ */
