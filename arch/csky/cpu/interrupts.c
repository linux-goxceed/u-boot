// SPDX-License-Identifier: GPL-2.0+

#include <irq_func.h>

int disable_interrupts(void)
{
	asm volatile("psrclr ie\n");
	return 0;
}

void enable_interrupts(void)
{
	asm volatile("psrset ie\n");
}

int interrupt_init(void)
{
	disable_interrupts();
	return 0;
}
