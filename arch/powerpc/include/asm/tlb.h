/*
 *	TLB shootdown specifics for powerpc
 *
 * Copyright (C) 2002 Anton Blanchard, IBM Corp.
 * Copyright (C) 2002 Paul Mackerras, IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#ifndef _ASM_POWERPC_TLB_H
#define _ASM_POWERPC_TLB_H
#ifdef __KERNEL__

#ifndef __powerpc64__
#include <asm/pgtable.h>
#endif
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#ifndef __powerpc64__
#include <asm/page.h>
#include <asm/mmu.h>
#endif

#include <linux/pagemap.h>

#define __tlb_remove_tlb_entry	__tlb_remove_tlb_entry

#define tlb_flush tlb_flush
extern void tlb_flush(struct mmu_gather *tlb);
/*
 * book3s:
 * Hash does not use the linux page-tables, so we can avoid
 * the TLB invalidate for page-table freeing, Radix otoh does use the
 * page-tables and needs the TLBI.
 *
 * nohash:
 * We still do TLB invalidate in the __pte_free_tlb routine before we
 * add the page table pages to mmu gather table batch.
 */
#define tlb_needs_table_invalidate()	radix_enabled()

/* Get the generic bits... */
#include <asm-generic/tlb.h>

extern void flush_hash_entry(struct mm_struct *mm, pte_t *ptep,
			     unsigned long address);

static inline void __tlb_remove_tlb_entry(struct mmu_gather *tlb, pte_t *ptep,
					  unsigned long address)
{
#ifdef CONFIG_PPC_BOOK3S_32
	if (pte_val(*ptep) & _PAGE_HASHPTE)
		flush_hash_entry(tlb->mm, ptep, address);
#endif
}

#ifdef CONFIG_SMP
static inline int mm_is_core_local(struct mm_struct *mm)
{
	return cpumask_subset(mm_cpumask(mm),
			      topology_sibling_cpumask(smp_processor_id()));
}

#ifdef CONFIG_PPC_BOOK3S_64
static inline int mm_is_thread_local(struct mm_struct *mm)
{
	if (atomic_read(&mm->context.active_cpus) > 1)
		return false;
	return cpumask_test_cpu(smp_processor_id(), mm_cpumask(mm));
}
#else /* CONFIG_PPC_BOOK3S_64 */
static inline int mm_is_thread_local(struct mm_struct *mm)
{
	return cpumask_equal(mm_cpumask(mm),
			      cpumask_of(smp_processor_id()));
}
#endif /* !CONFIG_PPC_BOOK3S_64 */

#else /* CONFIG_SMP */
static inline int mm_is_core_local(struct mm_struct *mm)
{
	return 1;
}

static inline int mm_is_thread_local(struct mm_struct *mm)
{
	return 1;
}
#endif

#endif /* __KERNEL__ */
#endif /* __ASM_POWERPC_TLB_H */
