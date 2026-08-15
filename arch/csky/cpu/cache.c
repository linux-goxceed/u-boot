// SPDX-License-Identifier: GPL-2.0+
/*
 * CK610 cachev1 maintenance via CR17 (CFR).
 *
 * Stage-1 leaves I/D caches enabled. Empty stubs here left EHCI QH/qTD
 * writes in D-cache while the host controller DMA'd from DRAM, causing
 * "EHCI timed out on TD". Range ops are not used: CK610 is flushed with
 * whole-cache CR17 ops (same recipe as gxtest/stage2load.c).
 */

#include <cpu_func.h>

#define INS_CACHE	(1u << 0)
#define DATA_CACHE	(1u << 1)
#define CACHE_INV	(1u << 4)
#define CACHE_CLR	(1u << 5)

static void ck610_cache_op(unsigned int op)
{
	asm volatile("mtcr %0, cr17" : : "r"(op) : "memory");
	asm volatile("sync" ::: "memory");
	asm volatile("idly4" ::: "memory");
}

void flush_dcache_range(unsigned long start, unsigned long end)
{
	(void)start;
	(void)end;
	ck610_cache_op(DATA_CACHE | CACHE_CLR);
}

void invalidate_dcache_range(unsigned long start, unsigned long end)
{
	(void)start;
	(void)end;
	ck610_cache_op(DATA_CACHE | CACHE_CLR | CACHE_INV);
}

void flush_cache(unsigned long start, unsigned long size)
{
	(void)start;
	(void)size;
	ck610_cache_op(INS_CACHE | DATA_CACHE | CACHE_CLR | CACHE_INV);
}

void icache_enable(void)
{
}

void icache_disable(void)
{
}

void dcache_enable(void)
{
}

void dcache_disable(void)
{
}

int icache_status(void)
{
	return 0;
}

int dcache_status(void)
{
	return 0;
}
