/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MEMREMAP_H_
#define _LINUX_MEMREMAP_H_
#include <linux/range.h>
#include <linux/ioport.h>
#include <linux/percpu-refcount.h>
#include <linux/mm_types.h>  /* needed in RHEL8 for dev_pagemap & vm_fault_t */

struct resource;
struct device;

/**
 * struct vmem_altmap - pre-allocated storage for vmemmap_populate
 * @base_pfn: base of the entire dev_pagemap mapping
 * @reserve: pages mapped, but reserved for driver use (relative to @base)
 * @free: free pages set aside in the mapping for memmap storage
 * @align: pages reserved to meet allocation alignments
 * @alloc: track pages consumed, private to vmemmap_populate()
 */
struct vmem_altmap {
	const unsigned long base_pfn;
	const unsigned long reserve;
	unsigned long free;
	unsigned long align;
	unsigned long alloc;
	RH_KABI_EXTEND(const unsigned long end_pfn)
};

/*
 * Specialize ZONE_DEVICE memory into multiple types each having differents
 * usage.
 *
 * MEMORY_DEVICE_PRIVATE:
 * Device memory that is not directly addressable by the CPU: CPU can neither
 * read nor write private memory. In this case, we do still have struct pages
 * backing the device memory. Doing so simplifies the implementation, but it is
 * important to remember that there are certain points at which the struct page
 * must be treated as an opaque object, rather than a "normal" struct page.
 *
 * A more complete discussion of unaddressable memory may be found in
 * include/linux/hmm.h and Documentation/vm/hmm.rst.
 *
 * MEMORY_DEVICE_PUBLIC:
 * Device memory that is cache coherent from device and CPU point of view. This
 * is use on platform that have an advance system bus (like CAPI or CCIX). A
 * driver can hotplug the device memory using ZONE_DEVICE and with that memory
 * type. Any page of a process can be migrated to such memory. However no one
 * should be allow to pin such memory so that it can always be evicted.
 *
 * MEMORY_DEVICE_FS_DAX:
 * Host memory that has similar access semantics as System RAM i.e. DMA
 * coherent and supports page pinning. In support of coordinating page
 * pinning vs other operations MEMORY_DEVICE_FS_DAX arranges for a
 * wakeup event whenever a page is unpinned and becomes idle. This
 * wakeup is used to coordinate physical address space management (ex:
 * fs truncate/hole punch) vs pinned pages (ex: device dma).
 *
 * MEMORY_DEVICE_GENERIC:
 * Host memory that has similar access semantics as System RAM i.e. DMA
 * coherent and supports page pinning. This is for example used by DAX devices
 * that expose memory using a character device.
 *
 * MEMORY_DEVICE_PCI_P2PDMA:
 * Device memory residing in a PCI BAR intended for use with Peer-to-Peer
 * transactions.
 */
enum memory_type {
	/* 0 is reserved to catch uninitialized type fields */
	MEMORY_DEVICE_PRIVATE = 1,
	MEMORY_DEVICE_PUBLIC,
	MEMORY_DEVICE_FS_DAX,
	MEMORY_DEVICE_PCI_P2PDMA,
#ifndef __GENKSYMS__
	MEMORY_DEVICE_GENERIC,
#endif /* __GENKSYMS__ */
};

typedef int (*dev_page_fault_t)(struct vm_area_struct *vma,
				unsigned long addr,
				const struct page *page,
				unsigned int flags,
				pmd_t *pmdp);
typedef void (*dev_page_free_t)(struct page *page, void *data);
struct dev_pagemap_ops {
	/*
	 * Called once the page refcount reaches 0.  The reference count will be
	 * reset to one by the core code after the method is called to prepare
	 * for handing out the page again.
	 */
	void (*page_free)(struct page *page);

	/*
	 * Transition the refcount in struct dev_pagemap to the dead state.
	 */
	void (*kill)(struct dev_pagemap *pgmap);

	/*
	 * Wait for refcount in struct dev_pagemap to be idle and reap it.
	 */
	void (*cleanup)(struct dev_pagemap *pgmap);

	/*
	 * Used for private (un-addressable) device memory only.  Must migrate
	 * the page back to a CPU accessible page.
	 */
	vm_fault_t (*migrate_to_ram)(struct vm_fault *vmf);
};

#define PGMAP_ALTMAP_VALID	(1 << 0)

/**
 * struct dev_pagemap - metadata for ZONE_DEVICE mappings
 * @altmap: pre-allocated/reserved memory for vmemmap allocations
 * @ref: reference count that pins the devm_memremap_pages() mapping
 * @internal_ref: internal reference if @ref is not provided by the caller
 * @done: completion for @internal_ref
 * @type: memory type: see MEMORY_* in memory_hotplug.h
 * @flags: PGMAP_* flags to specify defailed behavior
 * @vmemmap_shift: structural definition of how the vmemmap page metadata
 *      is populated, specifically the metadata page order.
 *	A zero value (default) uses base pages as the vmemmap metadata
 *	representation. A bigger value will set up compound struct pages
 *	of the requested order value.
 * @ops: method table
 * @owner: an opaque pointer identifying the entity that manages this
 *	instance.  Used by various helpers to make sure that no
 *	foreign ZONE_DEVICE memory is accessed.
 * @nr_range: number of ranges to be mapped
 * @range: range to be mapped when nr_range == 1
 * @ranges: array of ranges to be mapped when nr_range > 1
 */
struct dev_pagemap {
	RH_KABI_DEPRECATE(dev_page_fault_t, page_fault)
	RH_KABI_DEPRECATE(dev_page_free_t, page_free)
	struct vmem_altmap altmap;
	RH_KABI_DEPRECATE(bool, altmap_valid)
	RH_KABI_DEPRECATE(struct resource, res)
	struct percpu_ref *ref;
	RH_KABI_DEPRECATE(struct device *, dev)
	RH_KABI_DEPRECATE(void *, data)
	enum memory_type type;
	RH_KABI_DEPRECATE(u64, pci_p2pdma_bus_offset)
	RH_KABI_EXTEND(const struct dev_pagemap_ops *ops)
	RH_KABI_EXTEND(unsigned int flags)
	RH_KABI_EXTEND(struct percpu_ref internal_ref)
	RH_KABI_EXTEND(struct completion done)
	RH_KABI_EXTEND(void *owner)
	RH_KABI_EXTEND(int nr_range)
	RH_KABI_BROKEN_INSERT(unsigned long vmemmap_shift)
	RH_KABI_BROKEN_INSERT_BLOCK(
	union {
		struct range range;
		struct range ranges[0];
	};
	) /* RH_KABI_BROKEN_INSERT_BLOCK */
};

static inline struct vmem_altmap *pgmap_altmap(struct dev_pagemap *pgmap)
{
	if (pgmap->flags & PGMAP_ALTMAP_VALID)
		return &pgmap->altmap;
	return NULL;
}

static inline unsigned long pgmap_vmemmap_nr(struct dev_pagemap *pgmap)
{
	return 1 << pgmap->vmemmap_shift;
}

#ifdef CONFIG_ZONE_DEVICE
void *memremap_pages(struct dev_pagemap *pgmap, int nid);
void memunmap_pages(struct dev_pagemap *pgmap);
void *devm_memremap_pages(struct device *dev, struct dev_pagemap *pgmap);
void devm_memunmap_pages(struct device *dev, struct dev_pagemap *pgmap);
struct dev_pagemap *get_dev_pagemap(unsigned long pfn,
		struct dev_pagemap *pgmap);
bool pgmap_pfn_valid(struct dev_pagemap *pgmap, unsigned long pfn);

unsigned long vmem_altmap_offset(struct vmem_altmap *altmap);
void vmem_altmap_free(struct vmem_altmap *altmap, unsigned long nr_pfns);
unsigned long memremap_compat_align(void);
#else
static inline void *devm_memremap_pages(struct device *dev,
		struct dev_pagemap *pgmap)
{
	/*
	 * Fail attempts to call devm_memremap_pages() without
	 * ZONE_DEVICE support enabled, this requires callers to fall
	 * back to plain devm_memremap() based on config
	 */
	WARN_ON_ONCE(1);
	return ERR_PTR(-ENXIO);
}

static inline void devm_memunmap_pages(struct device *dev,
		struct dev_pagemap *pgmap)
{
}

static inline struct dev_pagemap *get_dev_pagemap(unsigned long pfn,
		struct dev_pagemap *pgmap)
{
	return NULL;
}

static inline bool pgmap_pfn_valid(struct dev_pagemap *pgmap, unsigned long pfn)
{
	return false;
}

static inline unsigned long vmem_altmap_offset(struct vmem_altmap *altmap)
{
	return 0;
}

static inline void vmem_altmap_free(struct vmem_altmap *altmap,
		unsigned long nr_pfns)
{
}

/* when memremap_pages() is disabled all archs can remap a single page */
static inline unsigned long memremap_compat_align(void)
{
	return PAGE_SIZE;
}
#endif /* CONFIG_ZONE_DEVICE */

static inline void put_dev_pagemap(struct dev_pagemap *pgmap)
{
	if (pgmap)
		percpu_ref_put(pgmap->ref);
}

#endif /* _LINUX_MEMREMAP_H_ */
