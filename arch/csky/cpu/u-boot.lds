/* SPDX-License-Identifier: GPL-2.0+ */

#include <config.h>

OUTPUT_FORMAT("elf32-csky-little", "elf32-csky-little", "elf32-csky-little")
OUTPUT_ARCH(csky)
ENTRY(_start)

SECTIONS
{
	. = CONFIG_TEXT_BASE;

	.text : {
		arch/csky/cpu/start.o (.text.start)
		arch/csky/cpu/start.o (.text)
		arch/csky/cpu/start.o (.text.*)
		*(.text*)
		*(.rodata*)
		. = ALIGN(8);
		*(.dtb.init.rodata)
	}

	. = ALIGN(4);
	_etext = .;

	. = ALIGN(4);
	__u_boot_list : {
		KEEP(*(SORT(__u_boot_list*)));
	}

	. = ALIGN(4);
	__image_copy_end = .;
	__init_end = .;

	. = ALIGN(4);
	_data = .;
	.data : {
		*(.data*)
	}

	_edata = .;

	. = ALIGN(4);
	__bss_start = .;
	.bss (NOLOAD) : {
		*(.bss*)
		*(COMMON)
	}
	__bss_end = .;

	_end = .;
}
