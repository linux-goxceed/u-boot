/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __ASM_CSKY_SYSTEM_H_
#define __ASM_CSKY_SYSTEM_H_

#define local_irq_enable()		asm volatile("psrset ie\n")
#define local_irq_disable()		asm volatile("psrclr ie\n")

#define local_save_flags(x)		asm volatile("mfcr %0, psr" : "=r"(x))
#define local_irq_restore(x)		asm volatile("mtcr %0, psr" :: "r"(x))

#define local_irq_save(x) \
	do { local_save_flags(x); local_irq_disable(); } while (0)

static inline int irqs_disabled(void)
{
	unsigned long psr;

	local_save_flags(psr);
	return !(psr & 0x80);
}

#endif /* __ASM_CSKY_SYSTEM_H_ */
