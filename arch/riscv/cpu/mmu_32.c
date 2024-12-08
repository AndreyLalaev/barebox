#define pr_fmt(fmt)	"mmu: " fmt

#include <common.h>
#include <asm/csr.h>
#include <asm/barebox-riscv.h>
#include <linux/align.h>

#include "mmu_32.h"

static void __mmu_enable_sv32(void)
{
	asm volatile ("sfence.vma");
	csr_read_set(CSR_SATP, SATP_MODE);
}

static void set_ttb_ppn(unsigned long ppn)
{
	csr_read_set(CSR_SATP, ppn >> PAGE_SHIFT);
}

static unsigned long get_ttb_ppn(void)
{
	return (csr_read(CSR_SATP) & SATP_PPN) * PAGE_SIZE;
}

static void map_page(u32* ttb, u32 addr)
{
	u32 offset = ALIGN_DOWN(addr / PAGE_SIZE, TABLE_ENTRY_CNT);
	u32 *ttb_2_base = ttb + TABLE_ENTRY_CNT + offset;
	u32 vpn_1 = (addr >> 22) & 0x3ff;
	u32 vpn_0 = (addr >> 12) & 0x3ff;
	u32 new_entry = ((((u32) ttb_2_base) / PAGE_SIZE) << 10) | PAGE_V;
	u32 flags = PAGE_V | PAGE_R | PAGE_W | PAGE_X;

	ttb[vpn_1] = new_entry;
	ttb_2_base[vpn_0] = ((addr / PAGE_SIZE) << 10) | flags;
}

static void create_flat_mapping(void)
{
	u32 *ttb = (u32 *) get_ttb_ppn();
	u32 end = 0xffffffff / PAGE_SIZE;

	for (u32 begin = 0; begin < end; begin++) {
		map_page(ttb, begin * PAGE_SIZE);
	}
}

void mmu_early_enable(unsigned long membase, unsigned long memsize)
{
	unsigned long ppn = riscv_mem_ttb(membase, membase + memsize);
	pr_info("membase = 0x%lx; memsize = 0x%lx; ppn = 0x%lx\n",
		membase, memsize, ppn);

	set_ttb_ppn(ppn);
	create_flat_mapping();
	__mmu_enable_sv32();

	pr_info("initialized\n");
}
