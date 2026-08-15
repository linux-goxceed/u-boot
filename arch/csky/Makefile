# SPDX-License-Identifier: GPL-2.0+

ARCH_FLAGS = -EL -mcpu=ck610

PLATFORM_CPPFLAGS += $(ARCH_FLAGS)
CFLAGS_EFI += $(ARCH_FLAGS)

head-y := arch/csky/cpu/start.o

libs-y += arch/csky/cpu/
libs-y += arch/csky/lib/
