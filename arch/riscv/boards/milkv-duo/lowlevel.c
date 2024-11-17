// SPDX-License-Identifier: GPL-2.0-only

#include <common.h>
#include <debug_ll.h>
#include <asm/barebox-riscv.h>

#define DRAM_BASE	0x80000000
#define RTOS_SIZE	0x000c0000

ENTRY_FUNCTION(start_milkv_duo, a0, a1, a2)
{
	unsigned long memsize = SZ_64M - RTOS_SIZE;

	debug_ll_init();
	putc_ll('>');

	barebox_riscv_supervisor_entry(DRAM_BASE, memsize, a0, (void *)a1);
}
