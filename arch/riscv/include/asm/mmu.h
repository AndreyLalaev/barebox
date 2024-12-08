/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __ASM_MMU_H
#define __ASM_MMU_H

#define MAP_ARCH_DEFAULT MAP_UNCACHED

void mmu_early_enable(unsigned long membase, unsigned long memsize);

#endif /* __ASM_MMU_H */
