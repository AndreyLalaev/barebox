#ifndef __RISCV_MMU_H
#define __RISCV_MMU_H

#define PAGE_V		BIT(0)
#define PAGE_R		BIT(1)
#define PAGE_W		BIT(2)
#define PAGE_X		BIT(3)
#define PAGE_U		BIT(4)
#define PAGE_G		BIT(5)
#define PAGE_A		BIT(6)
#define PAGE_D		BIT(7)
#define PAGE_RSW	GENMASK(9, 8)

#define TABLE_ENTRY_CNT	1024

void mmu_early_enable(unsigned long membase, unsigned long memsize);

#endif /* __RISCV_MMU_H */
