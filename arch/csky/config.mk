# SPDX-License-Identifier: GPL-2.0+

PLATFORM_CPPFLAGS += -D__CSKY__
PLATFORM_RELFLAGS += -ffunction-sections -fdata-sections
LDFLAGS_FINAL += --gc-sections

# csky-linux-gcc (ABIV1) - use gnu99 for 4.5.x, gnu11 ok on 6.3.x
KBUILD_CFLAGS := $(filter-out -std=gnu11,$(KBUILD_CFLAGS))
KBUILD_CFLAGS += $(if $(findstring 4.5.,$(shell $(CC) -dumpversion 2>/dev/null)),-std=gnu99,-std=gnu11)
KBUILD_CFLAGS := $(filter-out -Wno-unused-but-set-variable,$(KBUILD_CFLAGS))
