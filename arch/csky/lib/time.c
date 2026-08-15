// SPDX-License-Identifier: GPL-2.0+

#include <init.h>
#include <time.h>
#include <asm/global_data.h>

DECLARE_GLOBAL_DATA_PTR;

/*
 * When CONFIG_TIMER is enabled a real DM timer driver (e.g. GX6605S_TIMER)
 * provides get_ticks()/get_timer() via lib/time.c, so the software fallback
 * below must be compiled out to avoid duplicate symbols.
 */
#if !CONFIG_IS_ENABLED(TIMER)

static ulong timestamp;

int timer_init(void)
{
	timestamp = 0;
	return 0;
}

ulong get_timer(ulong base)
{
	return timestamp - base;
}

unsigned long long get_ticks(void)
{
	return get_timer(0);
}

ulong get_tbclk(void)
{
	return CONFIG_SYS_HZ;
}

unsigned long timer_read_counter(void)
{
	return get_timer(0);
}

void __weak timer_set_base(ulong t)
{
	timestamp = t;
}

#endif /* !TIMER */
