/*
 * KVMGT - the implementation of Intel mediated pass-through framework for KVM
 *
 * Copyright(c) 2014-2016 Intel Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Kevin Tian <kevin.tian@intel.com>
 *    Jike Song <jike.song@intel.com>
 *    Xiaoguang Chen <xiaoguang.chen@intel.com>
 */

#include <linux/init.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/kthread.h>
#include <linux/sched/mm.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/eventfd.h>
#include <linux/uuid.h>
#include <linux/mdev.h>
#include <linux/debugfs.h>

#include <linux/nospec.h>

#include <drm/drm_edid.h>

#include "i915_drv.h"
#include "intel_gvt.h"
#include "gvt.h"

#define MODULE_IMPORT_NS(ns)	MODULE_INFO(import_ns, __stringify(ns))

MODULE_IMPORT_NS(DMA_BUF);
MODULE_IMPORT_NS(I915_GVT);

/* helper macros copied from vfio-pci */
#define VFIO_PCI_OFFSET_SHIFT   40
#define VFIO_PCI_OFFSET_TO_INDEX(off)   (off >> VFIO_PCI_OFFSET_SHIFT)
#define VFIO_PCI_INDEX_TO_OFFSET(index) ((u64)(index) << VFIO_PCI_OFFSET_SHIFT)
#define VFIO_PCI_OFFSET_MASK    (((u64)(1) << VFIO_PCI_OFFSET_SHIFT) - 1)

#define EDID_BLOB_OFFSET (PAGE_SIZE/2)

#define OPREGION_SIGNATURE "IntelGraphicsMem"

struct vfio_region;
struct intel_vgpu_regops {
	size_t (*rw)(struct intel_vgpu *vgpu, char *buf,
			size_t count, loff_t *ppos, bool iswrite);
	void (*release)(struct intel_vgpu *vgpu,
			struct vfio_region *region);
};

struct vfio_region {
	u32				type;
	u32				subtype;
	size_t				size;
	u32				flags;
	const struct intel_vgpu_regops	*ops;
	void				*data;
};

struct vfio_edid_region {
	struct vfio_region_gfx_edid vfio_edid_regs;
	void *edid_blob;
};

struct kvmgt_pgfn {
	gfn_t gfn;
	struct hlist_node hnode;
};

struct gvt_dma {
	struct intel_vgpu *vgpu;
	struct rb_node gfn_node;
	struct rb_node dma_addr_node;
	gfn_t gfn;
	dma_addr_t dma_addr;
	unsigned long size;
	struct kref ref;
};

static void kvmgt_page_track_write(struct kvm_vcpu *vcpu, gpa_t gpa,
		const u8 *val, int len,
		struct kvm_page_track_notifier_node *node);
static void kvmgt_page_track_flush_slot(struct kvm *kvm,
		struct kvm_memory_slot *slot,
		struct kvm_page_track_notifier_node *node);

static ssize_t available_instances_show(struct kobject *kobj,
					struct device *dev,
					char *buf)
{
	unsigned int num = 0;
	struct intel_vgpu_type *type;
	struct intel_gvt *gvt = kdev_to_i915(dev)->gvt;
	int id = mdev_type_kobj_to_group_id(kobj);

	if (id < 0 || id >= NR_MAX_INTEL_VGPU_TYPES)
		return 0;

	type = &gvt->types[id];
	if (!type)
		num = 0;
	else
		num = type->avail_instance;

	return sprintf(buf, "%u\n", num);
}

static ssize_t device_api_show(struct kobject *kobj,
			       struct device *dev,
			       char *buf)
{
	return sprintf(buf, "%s\n", VFIO_DEVICE_API_PCI_STRING);
}

static ssize_t description_show(struct kobject *kobj,
				struct device *dev,
				char *buf)
{
	struct intel_vgpu_type *type;
	struct intel_gvt *gvt = kdev_to_i915(dev)->gvt;
	int id = mdev_type_kobj_to_group_id(kobj);

	if (id < 0 || id >= NR_MAX_INTEL_VGPU_TYPES)
		return 0;

	type = &gvt->types[id];
	if (!type)
		return 0;

	return sprintf(buf, "low_gm_size: %dMB\nhigh_gm_size: %dMB\n"
		       "fence: %d\nresolution: %s\n"
		       "weight: %d\n",
		       BYTES_TO_MB(type->low_gm_size),
		       BYTES_TO_MB(type->high_gm_size),
		       type->fence, vgpu_edid_str(type->resolution),
		       type->weight);
}

static ssize_t name_show(struct kobject *kobj,
			 struct device *dev,
			 char *buf)
{
	struct intel_vgpu_type *type;
	struct intel_gvt *gvt = kdev_to_i915(dev)->gvt;
	int id = mdev_type_kobj_to_group_id(kobj);

	if (id < 0 || id >= NR_MAX_INTEL_VGPU_TYPES)
		return 0;

	type = &gvt->types[id];
	if (!type)
		return 0;

	return sprintf(buf, "%s\n", type->name);
}

static MDEV_TYPE_ATTR_RO(available_instances);
static MDEV_TYPE_ATTR_RO(device_api);
static MDEV_TYPE_ATTR_RO(description);
static MDEV_TYPE_ATTR_RO(name);

static struct attribute *gvt_type_attrs[] = {
	&mdev_type_attr_available_instances.attr,
	&mdev_type_attr_device_api.attr,
	&mdev_type_attr_description.attr,
	&mdev_type_attr_name.attr,
	NULL,
};

static struct attribute_group *gvt_vgpu_type_groups[] = {
	[0 ... NR_MAX_INTEL_VGPU_TYPES - 1] = NULL,
};

int intel_gvt_init_vgpu_type_groups(struct intel_gvt *gvt)
{
	int i, j;
	struct intel_vgpu_type *type;
	struct attribute_group *group;

	for (i = 0; i < gvt->num_types; i++) {
		type = &gvt->types[i];

		group = kzalloc(sizeof(struct attribute_group), GFP_KERNEL);
		if (!group)
			goto unwind;

		group->name = type->name;
		group->attrs = gvt_type_attrs;
		gvt_vgpu_type_groups[i] = group;
	}

	return 0;

unwind:
	for (j = 0; j < i; j++) {
		group = gvt_vgpu_type_groups[j];
		kfree(group);
	}

	return -ENOMEM;
}

void intel_gvt_cleanup_vgpu_type_groups(struct intel_gvt *gvt)
{
	int i;
	struct attribute_group *group;

	for (i = 0; i < gvt->num_types; i++) {
		group = gvt_vgpu_type_groups[i];
		gvt_vgpu_type_groups[i] = NULL;
		kfree(group);
	}
}

static void intel_vgpu_release_work(struct work_struct *work);

static void gvt_unpin_guest_page(struct intel_vgpu *vgpu, unsigned long gfn,
		unsigned long size)
{
	struct drm_i915_private *i915 = vgpu->gvt->gt->i915;
	int total_pages;
	int npage;
	int ret;

	total_pages = roundup(size, PAGE_SIZE) / PAGE_SIZE;

	for (npage = 0; npage < total_pages; npage++) {
		unsigned long cur_gfn = gfn + npage;

		ret = vfio_group_unpin_pages(vgpu->vfio_group, &cur_gfn, 1);
		drm_WARN_ON(&i915->drm, ret != 1);
	}
}

/* Pin a normal or compound guest page for dma. */
static int gvt_pin_guest_page(struct intel_vgpu *vgpu, unsigned long gfn,
		unsigned long size, struct page **page)
{
	unsigned long base_pfn = 0;
	int total_pages;
	int npage;
	int ret;

	total_pages = roundup(size, PAGE_SIZE) / PAGE_SIZE;
	/*
	 * We pin the pages one-by-one to avoid allocating a big arrary
	 * on stack to hold pfns.
	 */
	for (npage = 0; npage < total_pages; npage++) {
		unsigned long cur_gfn = gfn + npage;
		unsigned long pfn;

		ret = vfio_group_pin_pages(vgpu->vfio_group, &cur_gfn, 1,
					   IOMMU_READ | IOMMU_WRITE, &pfn);
		if (ret != 1) {
			gvt_vgpu_err("vfio_pin_pages failed for gfn 0x%lx, ret %d\n",
				     cur_gfn, ret);
			goto err;
		}

		if (!pfn_valid(pfn)) {
			gvt_vgpu_err("pfn 0x%lx is not mem backed\n", pfn);
			npage++;
			ret = -EFAULT;
			goto err;
		}

		if (npage == 0)
			base_pfn = pfn;
		else if (base_pfn + npage != pfn) {
			gvt_vgpu_err("The pages are not continuous\n");
			ret = -EINVAL;
			npage++;
			goto err;
		}
	}

	*page = pfn_to_page(base_pfn);
	return 0;
err:
	gvt_unpin_guest_page(vgpu, gfn, npage * PAGE_SIZE);
	return ret;
}

static int gvt_dma_map_page(struct intel_vgpu *vgpu, unsigned long gfn,
		dma_addr_t *dma_addr, unsigned long size)
{
	struct device *dev = vgpu->gvt->gt->i915->drm.dev;
	struct page *page = NULL;
	int ret;

	ret = gvt_pin_guest_page(vgpu, gfn, size, &page);
	if (ret)
		return ret;

	/* Setup DMA mapping. */
	*dma_addr = dma_map_page(dev, page, 0, size, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, *dma_addr)) {
		gvt_vgpu_err("DMA mapping failed for pfn 0x%lx, ret %d\n",
			     page_to_pfn(page), ret);
		gvt_unpin_guest_page(vgpu, gfn, size);
		return -ENOMEM;
	}

	return 0;
}

static void gvt_dma_unmap_page(struct intel_vgpu *vgpu, unsigned long gfn,
		dma_addr_t dma_addr, unsigned long size)
{
	struct device *dev = vgpu->gvt->gt->i915->drm.dev;

	dma_unmap_page(dev, dma_addr, size, DMA_BIDIRECTIONAL);
	gvt_unpin_guest_page(vgpu, gfn, size);
}

static struct gvt_dma *__gvt_cache_find_dma_addr(struct intel_vgpu *vgpu,
		dma_addr_t dma_addr)
{
	struct rb_node *node = vgpu->dma_addr_cache.rb_node;
	struct gvt_dma *itr;

	while (node) {
		itr = rb_entry(node, struct gvt_dma, dma_addr_node);

		if (dma_addr < itr->dma_addr)
			node = node->rb_left;
		else if (dma_addr > itr->dma_addr)
			node = node->rb_right;
		else
			return itr;
	}
	return NULL;
}

static struct gvt_dma *__gvt_cache_find_gfn(struct intel_vgpu *vgpu, gfn_t gfn)
{
	struct rb_node *node = vgpu->gfn_cache.rb_node;
	struct gvt_dma *itr;

	while (node) {
		itr = rb_entry(node, struct gvt_dma, gfn_node);

		if (gfn < itr->gfn)
			node = node->rb_left;
		else if (gfn > itr->gfn)
			node = node->rb_right;
		else
			return itr;
	}
	return NULL;
}

static int __gvt_cache_add(struct intel_vgpu *vgpu, gfn_t gfn,
		dma_addr_t dma_addr, unsigned long size)
{
	struct gvt_dma *new, *itr;
	struct rb_node **link, *parent = NULL;

	new = kzalloc(sizeof(struct gvt_dma), GFP_KERNEL);
	if (!new)
		return -ENOMEM;

	new->vgpu = vgpu;
	new->gfn = gfn;
	new->dma_addr = dma_addr;
	new->size = size;
	kref_init(&new->ref);

	/* gfn_cache maps gfn to struct gvt_dma. */
	link = &vgpu->gfn_cache.rb_node;
	while (*link) {
		parent = *link;
		itr = rb_entry(parent, struct gvt_dma, gfn_node);

		if (gfn < itr->gfn)
			link = &parent->rb_left;
		else
			link = &parent->rb_right;
	}
	rb_link_node(&new->gfn_node, parent, link);
	rb_insert_color(&new->gfn_node, &vgpu->gfn_cache);

	/* dma_addr_cache maps dma addr to struct gvt_dma. */
	parent = NULL;
	link = &vgpu->dma_addr_cache.rb_node;
	while (*link) {
		parent = *link;
		itr = rb_entry(parent, struct gvt_dma, dma_addr_node);

		if (dma_addr < itr->dma_addr)
			link = &parent->rb_left;
		else
			link = &parent->rb_right;
	}
	rb_link_node(&new->dma_addr_node, parent, link);
	rb_insert_color(&new->dma_addr_node, &vgpu->dma_addr_cache);

	vgpu->nr_cache_entries++;
	return 0;
}

static void __gvt_cache_remove_entry(struct intel_vgpu *vgpu,
				struct gvt_dma *entry)
{
	rb_erase(&entry->gfn_node, &vgpu->gfn_cache);
	rb_erase(&entry->dma_addr_node, &vgpu->dma_addr_cache);
	kfree(entry);
	vgpu->nr_cache_entries--;
}

static void gvt_cache_destroy(struct intel_vgpu *vgpu)
{
	struct gvt_dma *dma;
	struct rb_node *node = NULL;

	for (;;) {
		mutex_lock(&vgpu->cache_lock);
		node = rb_first(&vgpu->gfn_cache);
		if (!node) {
			mutex_unlock(&vgpu->cache_lock);
			break;
		}
		dma = rb_entry(node, struct gvt_dma, gfn_node);
		gvt_dma_unmap_page(vgpu, dma->gfn, dma->dma_addr, dma->size);
		__gvt_cache_remove_entry(vgpu, dma);
		mutex_unlock(&vgpu->cache_lock);
	}
}

static void gvt_cache_init(struct intel_vgpu *vgpu)
{
	vgpu->gfn_cache = RB_ROOT;
	vgpu->dma_addr_cache = RB_ROOT;
	vgpu->nr_cache_entries = 0;
	mutex_init(&vgpu->cache_lock);
}

static void kvmgt_protect_table_init(struct intel_vgpu *info)
{
	hash_init(info->ptable);
}

static void kvmgt_protect_table_destroy(struct intel_vgpu *info)
{
	struct kvmgt_pgfn *p;
	struct hlist_node *tmp;
	int i;

	hash_for_each_safe(info->ptable, i, tmp, p, hnode) {
		hash_del(&p->hnode);
		kfree(p);
	}
}

static struct kvmgt_pgfn *
__kvmgt_protect_table_find(struct intel_vgpu *info, gfn_t gfn)
{
	struct kvmgt_pgfn *p, *res = NULL;

	hash_for_each_possible(info->ptable, p, hnode, gfn) {
		if (gfn == p->gfn) {
			res = p;
			break;
		}
	}

	return res;
}

static bool kvmgt_gfn_is_write_protected(struct intel_vgpu *info, gfn_t gfn)
{
	struct kvmgt_pgfn *p;

	p = __kvmgt_protect_table_find(info, gfn);
	return !!p;
}

static void kvmgt_protect_table_add(struct intel_vgpu *info, gfn_t gfn)
{
	struct kvmgt_pgfn *p;

	if (kvmgt_gfn_is_write_protected(info, gfn))
		return;

	p = kzalloc(sizeof(struct kvmgt_pgfn), GFP_ATOMIC);
	if (WARN(!p, "gfn: 0x%llx\n", gfn))
		return;

	p->gfn = gfn;
	hash_add(info->ptable, &p->hnode, gfn);
}

static void kvmgt_protect_table_del(struct intel_vgpu *info, gfn_t gfn)
{
	struct kvmgt_pgfn *p;

	p = __kvmgt_protect_table_find(info, gfn);
	if (p) {
		hash_del(&p->hnode);
		kfree(p);
	}
}

static size_t intel_vgpu_reg_rw_opregion(struct intel_vgpu *vgpu, char *buf,
		size_t count, loff_t *ppos, bool iswrite)
{
	unsigned int i = VFIO_PCI_OFFSET_TO_INDEX(*ppos) -
			VFIO_PCI_NUM_REGIONS;
	void *base = vgpu->region[i].data;
	loff_t pos = *ppos & VFIO_PCI_OFFSET_MASK;


	if (pos >= vgpu->region[i].size || iswrite) {
		gvt_vgpu_err("invalid op or offset for Intel vgpu OpRegion\n");
		return -EINVAL;
	}
	count = min(count, (size_t)(vgpu->region[i].size - pos));
	memcpy(buf, base + pos, count);

	return count;
}

static void intel_vgpu_reg_release_opregion(struct intel_vgpu *vgpu,
		struct vfio_region *region)
{
}

static const struct intel_vgpu_regops intel_vgpu_regops_opregion = {
	.rw = intel_vgpu_reg_rw_opregion,
	.release = intel_vgpu_reg_release_opregion,
};

static int handle_edid_regs(struct intel_vgpu *vgpu,
			struct vfio_edid_region *region, char *buf,
			size_t count, u16 offset, bool is_write)
{
	struct vfio_region_gfx_edid *regs = &region->vfio_edid_regs;
	unsigned int data;

	if (offset + count > sizeof(*regs))
		return -EINVAL;

	if (count != 4)
		return -EINVAL;

	if (is_write) {
		data = *((unsigned int *)buf);
		switch (offset) {
		case offsetof(struct vfio_region_gfx_edid, link_state):
			if (data == VFIO_DEVICE_GFX_LINK_STATE_UP) {
				if (!drm_edid_block_valid(
					(u8 *)region->edid_blob,
					0,
					true,
					NULL)) {
					gvt_vgpu_err("invalid EDID blob\n");
					return -EINVAL;
				}
				intel_vgpu_emulate_hotplug(vgpu, true);
			} else if (data == VFIO_DEVICE_GFX_LINK_STATE_DOWN)
				intel_vgpu_emulate_hotplug(vgpu, false);
			else {
				gvt_vgpu_err("invalid EDID link state %d\n",
					regs->link_state);
				return -EINVAL;
			}
			regs->link_state = data;
			break;
		case offsetof(struct vfio_region_gfx_edid, edid_size):
			if (data > regs->edid_max_size) {
				gvt_vgpu_err("EDID size is bigger than %d!\n",
					regs->edid_max_size);
				return -EINVAL;
			}
			regs->edid_size = data;
			break;
		default:
			/* read-only regs */
			gvt_vgpu_err("write read-only EDID region at offset %d\n",
				offset);
			return -EPERM;
		}
	} else {
		memcpy(buf, (char *)regs + offset, count);
	}

	return count;
}

static int handle_edid_blob(struct vfio_edid_region *region, char *buf,
			size_t count, u16 offset, bool is_write)
{
	if (offset + count > region->vfio_edid_regs.edid_size)
		return -EINVAL;

	if (is_write)
		memcpy(region->edid_blob + offset, buf, count);
	else
		memcpy(buf, region->edid_blob + offset, count);

	return count;
}

static size_t intel_vgpu_reg_rw_edid(struct intel_vgpu *vgpu, char *buf,
		size_t count, loff_t *ppos, bool iswrite)
{
	int ret;
	unsigned int i = VFIO_PCI_OFFSET_TO_INDEX(*ppos) -
			VFIO_PCI_NUM_REGIONS;
	struct vfio_edid_region *region = vgpu->region[i].data;
	loff_t pos = *ppos & VFIO_PCI_OFFSET_MASK;

	if (pos < region->vfio_edid_regs.edid_offset) {
		ret = handle_edid_regs(vgpu, region, buf, count, pos, iswrite);
	} else {
		pos -= EDID_BLOB_OFFSET;
		ret = handle_edid_blob(region, buf, count, pos, iswrite);
	}

	if (ret < 0)
		gvt_vgpu_err("failed to access EDID region\n");

	return ret;
}

static void intel_vgpu_reg_release_edid(struct intel_vgpu *vgpu,
					struct vfio_region *region)
{
	kfree(region->data);
}

static const struct intel_vgpu_regops intel_vgpu_regops_edid = {
	.rw = intel_vgpu_reg_rw_edid,
	.release = intel_vgpu_reg_release_edid,
};

static int intel_vgpu_register_reg(struct intel_vgpu *vgpu,
		unsigned int type, unsigned int subtype,
		const struct intel_vgpu_regops *ops,
		size_t size, u32 flags, void *data)
{
	struct vfio_region *region;

	region = krealloc(vgpu->region,
			(vgpu->num_regions + 1) * sizeof(*region),
			GFP_KERNEL);
	if (!region)
		return -ENOMEM;

	vgpu->region = region;
	vgpu->region[vgpu->num_regions].type = type;
	vgpu->region[vgpu->num_regions].subtype = subtype;
	vgpu->region[vgpu->num_regions].ops = ops;
	vgpu->region[vgpu->num_regions].size = size;
	vgpu->region[vgpu->num_regions].flags = flags;
	vgpu->region[vgpu->num_regions].data = data;
	vgpu->num_regions++;
	return 0;
}

int intel_gvt_set_opregion(struct intel_vgpu *vgpu)
{
	void *base;
	int ret;

	/* Each vgpu has its own opregion, although VFIO would create another
	 * one later. This one is used to expose opregion to VFIO. And the
	 * other one created by VFIO later, is used by guest actually.
	 */
	base = vgpu_opregion(vgpu)->va;
	if (!base)
		return -ENOMEM;

	if (memcmp(base, OPREGION_SIGNATURE, 16)) {
		memunmap(base);
		return -EINVAL;
	}

	ret = intel_vgpu_register_reg(vgpu,
			PCI_VENDOR_ID_INTEL | VFIO_REGION_TYPE_PCI_VENDOR_TYPE,
			VFIO_REGION_SUBTYPE_INTEL_IGD_OPREGION,
			&intel_vgpu_regops_opregion, OPREGION_SIZE,
			VFIO_REGION_INFO_FLAG_READ, base);

	return ret;
}

int intel_gvt_set_edid(struct intel_vgpu *vgpu, int port_num)
{
	struct intel_vgpu_port *port = intel_vgpu_port(vgpu, port_num);
	struct vfio_edid_region *base;
	int ret;

	base = kzalloc(sizeof(*base), GFP_KERNEL);
	if (!base)
		return -ENOMEM;

	/* TODO: Add multi-port and EDID extension block support */
	base->vfio_edid_regs.edid_offset = EDID_BLOB_OFFSET;
	base->vfio_edid_regs.edid_max_size = EDID_SIZE;
	base->vfio_edid_regs.edid_size = EDID_SIZE;
	base->vfio_edid_regs.max_xres = vgpu_edid_xres(port->id);
	base->vfio_edid_regs.max_yres = vgpu_edid_yres(port->id);
	base->edid_blob = port->edid->edid_block;

	ret = intel_vgpu_register_reg(vgpu,
			VFIO_REGION_TYPE_GFX,
			VFIO_REGION_SUBTYPE_GFX_EDID,
			&intel_vgpu_regops_edid, EDID_SIZE,
			VFIO_REGION_INFO_FLAG_READ |
			VFIO_REGION_INFO_FLAG_WRITE |
			VFIO_REGION_INFO_FLAG_CAPS, base);

	return ret;
}

static int intel_vgpu_create(struct kobject *kobj, struct mdev_device *mdev)
{
	struct device *pdev = mdev_parent_dev(mdev);
	struct intel_gvt *gvt = kdev_to_i915(pdev)->gvt;
	struct intel_vgpu_type *type;
	struct intel_vgpu *vgpu;
	int id;

	id = mdev_type_kobj_to_group_id(kobj);
	if (id < 0 || id >= NR_MAX_INTEL_VGPU_TYPES)
		return -EINVAL;

	type = &gvt->types[id];
	if (!type)
		return -EINVAL;

	vgpu = intel_gvt_create_vgpu(gvt, type);
	if (IS_ERR(vgpu)) {
		gvt_err("failed to create intel vgpu: %ld\n", PTR_ERR(vgpu));
		return PTR_ERR(vgpu);
	}

	INIT_WORK(&vgpu->release_work, intel_vgpu_release_work);

	vgpu->mdev = mdev;
	mdev_set_drvdata(mdev, vgpu);

	gvt_dbg_core("intel_vgpu_create succeeded for mdev: %s\n",
		     dev_name(mdev_dev(mdev)));
	return 0;
}

static int intel_vgpu_remove(struct mdev_device *mdev)
{
	struct intel_vgpu *vgpu = mdev_get_drvdata(mdev);

	if (vgpu->attached)
		return -EBUSY;

	intel_gvt_destroy_vgpu(vgpu);
	return 0;
}

static int intel_vgpu_iommu_notifier(struct notifier_block *nb,
				     unsigned long action, void *data)
{
	struct intel_vgpu *vgpu =
		container_of(nb, struct intel_vgpu, iommu_notifier);

	if (action == VFIO_IOMMU_NOTIFY_DMA_UNMAP) {
		struct vfio_iommu_type1_dma_unmap *unmap = data;
		struct gvt_dma *entry;
		unsigned long iov_pfn, end_iov_pfn;

		iov_pfn = unmap->iova >> PAGE_SHIFT;
		end_iov_pfn = iov_pfn + unmap->size / PAGE_SIZE;

		mutex_lock(&vgpu->cache_lock);
		for (; iov_pfn < end_iov_pfn; iov_pfn++) {
			entry = __gvt_cache_find_gfn(vgpu, iov_pfn);
			if (!entry)
				continue;

			gvt_dma_unmap_page(vgpu, entry->gfn, entry->dma_addr,
					   entry->size);
			__gvt_cache_remove_entry(vgpu, entry);
		}
		mutex_unlock(&vgpu->cache_lock);
	}

	return NOTIFY_OK;
}

static int intel_vgpu_group_notifier(struct notifier_block *nb,
				     unsigned long action, void *data)
{
	struct intel_vgpu *vgpu =
		container_of(nb, struct intel_vgpu, group_notifier);

	/* the only action we care about */
	if (action == VFIO_GROUP_NOTIFY_SET_KVM) {
		vgpu->kvm = data;

		if (!data)
			schedule_work(&vgpu->release_work);
	}

	return NOTIFY_OK;
}

static bool __kvmgt_vgpu_exist(struct intel_vgpu *vgpu)
{
	struct intel_vgpu *itr;
	int id;
	bool ret = false;

	mutex_lock(&vgpu->gvt->lock);
	for_each_active_vgpu(vgpu->gvt, itr, id) {
		if (!itr->attached)
			continue;

		if (vgpu->kvm == itr->kvm) {
			ret = true;
			goto out;
		}
	}
out:
	mutex_unlock(&vgpu->gvt->lock);
	return ret;
}

static int intel_vgpu_open(struct mdev_device *mdev)
{
	struct intel_vgpu *vgpu = mdev_get_drvdata(mdev);
	unsigned long events;
	int ret;
	struct vfio_group *vfio_group;

	vgpu->iommu_notifier.notifier_call = intel_vgpu_iommu_notifier;
	vgpu->group_notifier.notifier_call = intel_vgpu_group_notifier;

	events = VFIO_IOMMU_NOTIFY_DMA_UNMAP;
	ret = vfio_register_notifier(mdev_dev(mdev), VFIO_IOMMU_NOTIFY, &events,
				&vgpu->iommu_notifier);
	if (ret != 0) {
		gvt_vgpu_err("vfio_register_notifier for iommu failed: %d\n",
			ret);
		goto out;
	}

	events = VFIO_GROUP_NOTIFY_SET_KVM;
	ret = vfio_register_notifier(mdev_dev(mdev), VFIO_GROUP_NOTIFY, &events,
				&vgpu->group_notifier);
	if (ret != 0) {
		gvt_vgpu_err("vfio_register_notifier for group failed: %d\n",
			ret);
		goto undo_iommu;
	}

	vfio_group = vfio_group_get_external_user_from_dev(mdev_dev(mdev));
	if (IS_ERR_OR_NULL(vfio_group)) {
		ret = !vfio_group ? -EFAULT : PTR_ERR(vfio_group);
		gvt_vgpu_err("vfio_group_get_external_user_from_dev failed\n");
		goto undo_register;
	}
	vgpu->vfio_group = vfio_group;

	/* Take a module reference as mdev core doesn't take
	 * a reference for vendor driver.
	 */
	if (!try_module_get(THIS_MODULE)) {
		ret = -ENODEV;
		goto undo_group;
	}

	ret = -EEXIST;
	if (vgpu->attached)
		goto undo_group;

	ret = -ESRCH;
	if (!vgpu->kvm || vgpu->kvm->mm != current->mm) {
		gvt_vgpu_err("KVM is required to use Intel vGPU\n");
		goto undo_group;
	}

	ret = -EEXIST;
	if (__kvmgt_vgpu_exist(vgpu))
		goto undo_group;

	vgpu->attached = true;
	kvm_get_kvm(vgpu->kvm);

	kvmgt_protect_table_init(vgpu);
	gvt_cache_init(vgpu);

	vgpu->track_node.track_write = kvmgt_page_track_write;
	vgpu->track_node.track_flush_slot = kvmgt_page_track_flush_slot;
	kvm_page_track_register_notifier(vgpu->kvm, &vgpu->track_node);

	debugfs_create_ulong(KVMGT_DEBUGFS_FILENAME, 0444, vgpu->debugfs,
			     &vgpu->nr_cache_entries);

	intel_gvt_activate_vgpu(vgpu);

	atomic_set(&vgpu->released, 0);
	return 0;

undo_group:
	vfio_group_put_external_user(vgpu->vfio_group);
	vgpu->vfio_group = NULL;

undo_register:
	vfio_unregister_notifier(mdev_dev(mdev), VFIO_GROUP_NOTIFY,
					&vgpu->group_notifier);

undo_iommu:
	vfio_unregister_notifier(mdev_dev(mdev), VFIO_IOMMU_NOTIFY,
					&vgpu->iommu_notifier);
out:
	return ret;
}

static void intel_vgpu_release_msi_eventfd_ctx(struct intel_vgpu *vgpu)
{
	struct eventfd_ctx *trigger;

	trigger = vgpu->msi_trigger;
	if (trigger) {
		eventfd_ctx_put(trigger);
		vgpu->msi_trigger = NULL;
	}
}

static void __intel_vgpu_release(struct intel_vgpu *vgpu)
{
	struct drm_i915_private *i915 = vgpu->gvt->gt->i915;
	int ret;

	if (!vgpu->attached)
		return;

	if (atomic_cmpxchg(&vgpu->released, 0, 1))
		return;

	intel_gvt_release_vgpu(vgpu);

	ret = vfio_unregister_notifier(mdev_dev(vgpu->mdev), VFIO_IOMMU_NOTIFY,
					&vgpu->iommu_notifier);
	drm_WARN(&i915->drm, ret,
		 "vfio_unregister_notifier for iommu failed: %d\n", ret);

	ret = vfio_unregister_notifier(mdev_dev(vgpu->mdev), VFIO_GROUP_NOTIFY,
					&vgpu->group_notifier);
	drm_WARN(&i915->drm, ret,
		 "vfio_unregister_notifier for group failed: %d\n", ret);

	/* dereference module reference taken at open */
	module_put(THIS_MODULE);

	debugfs_remove(debugfs_lookup(KVMGT_DEBUGFS_FILENAME, vgpu->debugfs));

	kvm_page_track_unregister_notifier(vgpu->kvm, &vgpu->track_node);
	kvm_put_kvm(vgpu->kvm);
	kvmgt_protect_table_destroy(vgpu);
	gvt_cache_destroy(vgpu);

	intel_vgpu_release_msi_eventfd_ctx(vgpu);
	vfio_group_put_external_user(vgpu->vfio_group);

	vgpu->kvm = NULL;
	vgpu->attached = false;
}

static void intel_vgpu_release(struct mdev_device *mdev)
{
	struct intel_vgpu *vgpu = mdev_get_drvdata(mdev);

	__intel_vgpu_release(vgpu);
}

static void intel_vgpu_release_work(struct work_struct *work)
{
	struct intel_vgpu *vgpu =
		container_of(work, struct intel_vgpu, release_work);

	__intel_vgpu_release(vgpu);
}

static u64 intel_vgpu_get_bar_addr(struct intel_vgpu *vgpu, int bar)
{
	u32 start_lo, start_hi;
	u32 mem_type;

	start_lo = (*(u32 *)(vgpu->cfg_space.virtual_cfg_space + bar)) &
			PCI_BASE_ADDRESS_MEM_MASK;
	mem_type = (*(u32 *)(vgpu->cfg_space.virtual_cfg_space + bar)) &
			PCI_BASE_ADDRESS_MEM_TYPE_MASK;

	switch (mem_type) {
	case PCI_BASE_ADDRESS_MEM_TYPE_64:
		start_hi = (*(u32 *)(vgpu->cfg_space.virtual_cfg_space
						+ bar + 4));
		break;
	case PCI_BASE_ADDRESS_MEM_TYPE_32:
	case PCI_BASE_ADDRESS_MEM_TYPE_1M:
		/* 1M mem BAR treated as 32-bit BAR */
	default:
		/* mem unknown type treated as 32-bit BAR */
		start_hi = 0;
		break;
	}

	return ((u64)start_hi << 32) | start_lo;
}

static int intel_vgpu_bar_rw(struct intel_vgpu *vgpu, int bar, u64 off,
			     void *buf, unsigned int count, bool is_write)
{
	u64 bar_start = intel_vgpu_get_bar_addr(vgpu, bar);
	int ret;

	if (is_write)
		ret = intel_vgpu_emulate_mmio_write(vgpu,
					bar_start + off, buf, count);
	else
		ret = intel_vgpu_emulate_mmio_read(vgpu,
					bar_start + off, buf, count);
	return ret;
}

static inline bool intel_vgpu_in_aperture(struct intel_vgpu *vgpu, u64 off)
{
	return off >= vgpu_aperture_offset(vgpu) &&
	       off < vgpu_aperture_offset(vgpu) + vgpu_aperture_sz(vgpu);
}

static int intel_vgpu_aperture_rw(struct intel_vgpu *vgpu, u64 off,
		void *buf, unsigned long count, bool is_write)
{
	void __iomem *aperture_va;

	if (!intel_vgpu_in_aperture(vgpu, off) ||
	    !intel_vgpu_in_aperture(vgpu, off + count)) {
		gvt_vgpu_err("Invalid aperture offset %llu\n", off);
		return -EINVAL;
	}

	aperture_va = io_mapping_map_wc(&vgpu->gvt->gt->ggtt->iomap,
					ALIGN_DOWN(off, PAGE_SIZE),
					count + offset_in_page(off));
	if (!aperture_va)
		return -EIO;

	if (is_write)
		memcpy_toio(aperture_va + offset_in_page(off), buf, count);
	else
		memcpy_fromio(buf, aperture_va + offset_in_page(off), count);

	io_mapping_unmap(aperture_va);

	return 0;
}

static ssize_t intel_vgpu_rw(struct intel_vgpu *vgpu, char *buf,
			size_t count, loff_t *ppos, bool is_write)
{
	unsigned int index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);
	u64 pos = *ppos & VFIO_PCI_OFFSET_MASK;
	int ret = -EINVAL;


	if (index >= VFIO_PCI_NUM_REGIONS + vgpu->num_regions) {
		gvt_vgpu_err("invalid index: %u\n", index);
		return -EINVAL;
	}

	switch (index) {
	case VFIO_PCI_CONFIG_REGION_INDEX:
		if (is_write)
			ret = intel_vgpu_emulate_cfg_write(vgpu, pos,
						buf, count);
		else
			ret = intel_vgpu_emulate_cfg_read(vgpu, pos,
						buf, count);
		break;
	case VFIO_PCI_BAR0_REGION_INDEX:
		ret = intel_vgpu_bar_rw(vgpu, PCI_BASE_ADDRESS_0, pos,
					buf, count, is_write);
		break;
	case VFIO_PCI_BAR2_REGION_INDEX:
		ret = intel_vgpu_aperture_rw(vgpu, pos, buf, count, is_write);
		break;
	case VFIO_PCI_BAR1_REGION_INDEX:
	case VFIO_PCI_BAR3_REGION_INDEX:
	case VFIO_PCI_BAR4_REGION_INDEX:
	case VFIO_PCI_BAR5_REGION_INDEX:
	case VFIO_PCI_VGA_REGION_INDEX:
	case VFIO_PCI_ROM_REGION_INDEX:
		break;
	default:
		if (index >= VFIO_PCI_NUM_REGIONS + vgpu->num_regions)
			return -EINVAL;

		index -= VFIO_PCI_NUM_REGIONS;
		return vgpu->region[index].ops->rw(vgpu, buf, count,
				ppos, is_write);
	}

	return ret == 0 ? count : ret;
}

static bool gtt_entry(struct intel_vgpu *vgpu, loff_t *ppos)
{
	unsigned int index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);
	struct intel_gvt *gvt = vgpu->gvt;
	int offset;

	/* Only allow MMIO GGTT entry access */
	if (index != PCI_BASE_ADDRESS_0)
		return false;

	offset = (u64)(*ppos & VFIO_PCI_OFFSET_MASK) -
		intel_vgpu_get_bar_gpa(vgpu, PCI_BASE_ADDRESS_0);

	return (offset >= gvt->device_info.gtt_start_offset &&
		offset < gvt->device_info.gtt_start_offset + gvt_ggtt_sz(gvt)) ?
			true : false;
}

static ssize_t intel_vgpu_read(struct mdev_device *mdev, char __user *buf,
			size_t count, loff_t *ppos)
{
	struct intel_vgpu *vgpu = mdev_get_drvdata(mdev);
	unsigned int done = 0;
	int ret;

	while (count) {
		size_t filled;

		/* Only support GGTT entry 8 bytes read */
		if (count >= 8 && !(*ppos % 8) &&
			gtt_entry(vgpu, ppos)) {
			u64 val;

			ret = intel_vgpu_rw(vgpu, (char *)&val, sizeof(val),
					ppos, false);
			if (ret <= 0)
				goto read_err;

			if (copy_to_user(buf, &val, sizeof(val)))
				goto read_err;

			filled = 8;
		} else if (count >= 4 && !(*ppos % 4)) {
			u32 val;

			ret = intel_vgpu_rw(vgpu, (char *)&val, sizeof(val),
					ppos, false);
			if (ret <= 0)
				goto read_err;

			if (copy_to_user(buf, &val, sizeof(val)))
				goto read_err;

			filled = 4;
		} else if (count >= 2 && !(*ppos % 2)) {
			u16 val;

			ret = intel_vgpu_rw(vgpu, (char *)&val, sizeof(val),
					ppos, false);
			if (ret <= 0)
				goto read_err;

			if (copy_to_user(buf, &val, sizeof(val)))
				goto read_err;

			filled = 2;
		} else {
			u8 val;

			ret = intel_vgpu_rw(vgpu, &val, sizeof(val), ppos,
					false);
			if (ret <= 0)
				goto read_err;

			if (copy_to_user(buf, &val, sizeof(val)))
				goto read_err;

			filled = 1;
		}

		count -= filled;
		done += filled;
		*ppos += filled;
		buf += filled;
	}

	return done;

read_err:
	return -EFAULT;
}

static ssize_t intel_vgpu_write(struct mdev_device *mdev,
				const char __user *buf,
				size_t count, loff_t *ppos)
{
	struct intel_vgpu *vgpu = mdev_get_drvdata(mdev);
	unsigned int done = 0;
	int ret;

	while (count) {
		size_t filled;

		/* Only support GGTT entry 8 bytes write */
		if (count >= 8 && !(*ppos % 8) &&
			gtt_entry(vgpu, ppos)) {
			u64 val;

			if (copy_from_user(&val, buf, sizeof(val)))
				goto write_err;

			ret = intel_vgpu_rw(vgpu, (char *)&val, sizeof(val),
					ppos, true);
			if (ret <= 0)
				goto write_err;

			filled = 8;
		} else if (count >= 4 && !(*ppos % 4)) {
			u32 val;

			if (copy_from_user(&val, buf, sizeof(val)))
				goto write_err;

			ret = intel_vgpu_rw(vgpu, (char *)&val, sizeof(val),
					ppos, true);
			if (ret <= 0)
				goto write_err;

			filled = 4;
		} else if (count >= 2 && !(*ppos % 2)) {
			u16 val;

			if (copy_from_user(&val, buf, sizeof(val)))
				goto write_err;

			ret = intel_vgpu_rw(vgpu, (char *)&val,
					sizeof(val), ppos, true);
			if (ret <= 0)
				goto write_err;

			filled = 2;
		} else {
			u8 val;

			if (copy_from_user(&val, buf, sizeof(val)))
				goto write_err;

			ret = intel_vgpu_rw(vgpu, &val, sizeof(val),
					ppos, true);
			if (ret <= 0)
				goto write_err;

			filled = 1;
		}

		count -= filled;
		done += filled;
		*ppos += filled;
		buf += filled;
	}

	return done;
write_err:
	return -EFAULT;
}

static int intel_vgpu_mmap(struct mdev_device *mdev, struct vm_area_struct *vma)
{
	unsigned int index;
	u64 virtaddr;
	unsigned long req_size, pgoff, req_start;
	pgprot_t pg_prot;
	struct intel_vgpu *vgpu = mdev_get_drvdata(mdev);

	index = vma->vm_pgoff >> (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT);
	if (index >= VFIO_PCI_ROM_REGION_INDEX)
		return -EINVAL;

	if (vma->vm_end < vma->vm_start)
		return -EINVAL;
	if ((vma->vm_flags & VM_SHARED) == 0)
		return -EINVAL;
	if (index != VFIO_PCI_BAR2_REGION_INDEX)
		return -EINVAL;

	pg_prot = vma->vm_page_prot;
	virtaddr = vma->vm_start;
	req_size = vma->vm_end - vma->vm_start;
	pgoff = vma->vm_pgoff &
		((1U << (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT)) - 1);
	req_start = pgoff << PAGE_SHIFT;

	if (!intel_vgpu_in_aperture(vgpu, req_start))
		return -EINVAL;
	if (req_start + req_size >
	    vgpu_aperture_offset(vgpu) + vgpu_aperture_sz(vgpu))
		return -EINVAL;

	pgoff = (gvt_aperture_pa_base(vgpu->gvt) >> PAGE_SHIFT) + pgoff;

	return remap_pfn_range(vma, virtaddr, pgoff, req_size, pg_prot);
}

static int intel_vgpu_get_irq_count(struct intel_vgpu *vgpu, int type)
{
	if (type == VFIO_PCI_INTX_IRQ_INDEX || type == VFIO_PCI_MSI_IRQ_INDEX)
		return 1;

	return 0;
}

static int intel_vgpu_set_intx_mask(struct intel_vgpu *vgpu,
			unsigned int index, unsigned int start,
			unsigned int count, u32 flags,
			void *data)
{
	return 0;
}

static int intel_vgpu_set_intx_unmask(struct intel_vgpu *vgpu,
			unsigned int index, unsigned int start,
			unsigned int count, u32 flags, void *data)
{
	return 0;
}

static int intel_vgpu_set_intx_trigger(struct intel_vgpu *vgpu,
		unsigned int index, unsigned int start, unsigned int count,
		u32 flags, void *data)
{
	return 0;
}

static int intel_vgpu_set_msi_trigger(struct intel_vgpu *vgpu,
		unsigned int index, unsigned int start, unsigned int count,
		u32 flags, void *data)
{
	struct eventfd_ctx *trigger;

	if (flags & VFIO_IRQ_SET_DATA_EVENTFD) {
		int fd = *(int *)data;

		trigger = eventfd_ctx_fdget(fd);
		if (IS_ERR(trigger)) {
			gvt_vgpu_err("eventfd_ctx_fdget failed\n");
			return PTR_ERR(trigger);
		}
		vgpu->msi_trigger = trigger;
	} else if ((flags & VFIO_IRQ_SET_DATA_NONE) && !count)
		intel_vgpu_release_msi_eventfd_ctx(vgpu);

	return 0;
}

static int intel_vgpu_set_irqs(struct intel_vgpu *vgpu, u32 flags,
		unsigned int index, unsigned int start, unsigned int count,
		void *data)
{
	int (*func)(struct intel_vgpu *vgpu, unsigned int index,
			unsigned int start, unsigned int count, u32 flags,
			void *data) = NULL;

	switch (index) {
	case VFIO_PCI_INTX_IRQ_INDEX:
		switch (flags & VFIO_IRQ_SET_ACTION_TYPE_MASK) {
		case VFIO_IRQ_SET_ACTION_MASK:
			func = intel_vgpu_set_intx_mask;
			break;
		case VFIO_IRQ_SET_ACTION_UNMASK:
			func = intel_vgpu_set_intx_unmask;
			break;
		case VFIO_IRQ_SET_ACTION_TRIGGER:
			func = intel_vgpu_set_intx_trigger;
			break;
		}
		break;
	case VFIO_PCI_MSI_IRQ_INDEX:
		switch (flags & VFIO_IRQ_SET_ACTION_TYPE_MASK) {
		case VFIO_IRQ_SET_ACTION_MASK:
		case VFIO_IRQ_SET_ACTION_UNMASK:
			/* XXX Need masking support exported */
			break;
		case VFIO_IRQ_SET_ACTION_TRIGGER:
			func = intel_vgpu_set_msi_trigger;
			break;
		}
		break;
	}

	if (!func)
		return -ENOTTY;

	return func(vgpu, index, start, count, flags, data);
}

static long intel_vgpu_ioctl(struct mdev_device *mdev, unsigned int cmd,
			     unsigned long arg)
{
	struct intel_vgpu *vgpu = mdev_get_drvdata(mdev);
	unsigned long minsz;

	gvt_dbg_core("vgpu%d ioctl, cmd: %d\n", vgpu->id, cmd);

	if (cmd == VFIO_DEVICE_GET_INFO) {
		struct vfio_device_info info;

		minsz = offsetofend(struct vfio_device_info, num_irqs);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if (info.argsz < minsz)
			return -EINVAL;

		info.flags = VFIO_DEVICE_FLAGS_PCI;
		info.flags |= VFIO_DEVICE_FLAGS_RESET;
		info.num_regions = VFIO_PCI_NUM_REGIONS +
				vgpu->num_regions;
		info.num_irqs = VFIO_PCI_NUM_IRQS;

		return copy_to_user((void __user *)arg, &info, minsz) ?
			-EFAULT : 0;

	} else if (cmd == VFIO_DEVICE_GET_REGION_INFO) {
		struct vfio_region_info info;
		struct vfio_info_cap caps = { .buf = NULL, .size = 0 };
		unsigned int i;
		int ret;
		struct vfio_region_info_cap_sparse_mmap *sparse = NULL;
		int nr_areas = 1;
		int cap_type_id;

		minsz = offsetofend(struct vfio_region_info, offset);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if (info.argsz < minsz)
			return -EINVAL;

		switch (info.index) {
		case VFIO_PCI_CONFIG_REGION_INDEX:
			info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
			info.size = vgpu->gvt->device_info.cfg_space_size;
			info.flags = VFIO_REGION_INFO_FLAG_READ |
				     VFIO_REGION_INFO_FLAG_WRITE;
			break;
		case VFIO_PCI_BAR0_REGION_INDEX:
			info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
			info.size = vgpu->cfg_space.bar[info.index].size;
			if (!info.size) {
				info.flags = 0;
				break;
			}

			info.flags = VFIO_REGION_INFO_FLAG_READ |
				     VFIO_REGION_INFO_FLAG_WRITE;
			break;
		case VFIO_PCI_BAR1_REGION_INDEX:
			info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
			info.size = 0;
			info.flags = 0;
			break;
		case VFIO_PCI_BAR2_REGION_INDEX:
			info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
			info.flags = VFIO_REGION_INFO_FLAG_CAPS |
					VFIO_REGION_INFO_FLAG_MMAP |
					VFIO_REGION_INFO_FLAG_READ |
					VFIO_REGION_INFO_FLAG_WRITE;
			info.size = gvt_aperture_sz(vgpu->gvt);

			sparse = kzalloc(struct_size(sparse, areas, nr_areas),
					 GFP_KERNEL);
			if (!sparse)
				return -ENOMEM;

			sparse->header.id = VFIO_REGION_INFO_CAP_SPARSE_MMAP;
			sparse->header.version = 1;
			sparse->nr_areas = nr_areas;
			cap_type_id = VFIO_REGION_INFO_CAP_SPARSE_MMAP;
			sparse->areas[0].offset =
					PAGE_ALIGN(vgpu_aperture_offset(vgpu));
			sparse->areas[0].size = vgpu_aperture_sz(vgpu);
			break;

		case VFIO_PCI_BAR3_REGION_INDEX ... VFIO_PCI_BAR5_REGION_INDEX:
			info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
			info.size = 0;
			info.flags = 0;

			gvt_dbg_core("get region info bar:%d\n", info.index);
			break;

		case VFIO_PCI_ROM_REGION_INDEX:
		case VFIO_PCI_VGA_REGION_INDEX:
			info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
			info.size = 0;
			info.flags = 0;

			gvt_dbg_core("get region info index:%d\n", info.index);
			break;
		default:
			{
				struct vfio_region_info_cap_type cap_type = {
					.header.id = VFIO_REGION_INFO_CAP_TYPE,
					.header.version = 1 };

				if (info.index >= VFIO_PCI_NUM_REGIONS +
						vgpu->num_regions)
					return -EINVAL;
				info.index =
					array_index_nospec(info.index,
							VFIO_PCI_NUM_REGIONS +
							vgpu->num_regions);

				i = info.index - VFIO_PCI_NUM_REGIONS;

				info.offset =
					VFIO_PCI_INDEX_TO_OFFSET(info.index);
				info.size = vgpu->region[i].size;
				info.flags = vgpu->region[i].flags;

				cap_type.type = vgpu->region[i].type;
				cap_type.subtype = vgpu->region[i].subtype;

				ret = vfio_info_add_capability(&caps,
							&cap_type.header,
							sizeof(cap_type));
				if (ret)
					return ret;
			}
		}

		if ((info.flags & VFIO_REGION_INFO_FLAG_CAPS) && sparse) {
			switch (cap_type_id) {
			case VFIO_REGION_INFO_CAP_SPARSE_MMAP:
				ret = vfio_info_add_capability(&caps,
					&sparse->header,
					struct_size(sparse, areas,
						    sparse->nr_areas));
				if (ret) {
					kfree(sparse);
					return ret;
				}
				break;
			default:
				kfree(sparse);
				return -EINVAL;
			}
		}

		if (caps.size) {
			info.flags |= VFIO_REGION_INFO_FLAG_CAPS;
			if (info.argsz < sizeof(info) + caps.size) {
				info.argsz = sizeof(info) + caps.size;
				info.cap_offset = 0;
			} else {
				vfio_info_cap_shift(&caps, sizeof(info));
				if (copy_to_user((void __user *)arg +
						  sizeof(info), caps.buf,
						  caps.size)) {
					kfree(caps.buf);
					kfree(sparse);
					return -EFAULT;
				}
				info.cap_offset = sizeof(info);
			}

			kfree(caps.buf);
		}

		kfree(sparse);
		return copy_to_user((void __user *)arg, &info, minsz) ?
			-EFAULT : 0;
	} else if (cmd == VFIO_DEVICE_GET_IRQ_INFO) {
		struct vfio_irq_info info;

		minsz = offsetofend(struct vfio_irq_info, count);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if (info.argsz < minsz || info.index >= VFIO_PCI_NUM_IRQS)
			return -EINVAL;

		switch (info.index) {
		case VFIO_PCI_INTX_IRQ_INDEX:
		case VFIO_PCI_MSI_IRQ_INDEX:
			break;
		default:
			return -EINVAL;
		}

		info.flags = VFIO_IRQ_INFO_EVENTFD;

		info.count = intel_vgpu_get_irq_count(vgpu, info.index);

		if (info.index == VFIO_PCI_INTX_IRQ_INDEX)
			info.flags |= (VFIO_IRQ_INFO_MASKABLE |
				       VFIO_IRQ_INFO_AUTOMASKED);
		else
			info.flags |= VFIO_IRQ_INFO_NORESIZE;

		return copy_to_user((void __user *)arg, &info, minsz) ?
			-EFAULT : 0;
	} else if (cmd == VFIO_DEVICE_SET_IRQS) {
		struct vfio_irq_set hdr;
		u8 *data = NULL;
		int ret = 0;
		size_t data_size = 0;

		minsz = offsetofend(struct vfio_irq_set, count);

		if (copy_from_user(&hdr, (void __user *)arg, minsz))
			return -EFAULT;

		if (!(hdr.flags & VFIO_IRQ_SET_DATA_NONE)) {
			int max = intel_vgpu_get_irq_count(vgpu, hdr.index);

			ret = vfio_set_irqs_validate_and_prepare(&hdr, max,
						VFIO_PCI_NUM_IRQS, &data_size);
			if (ret) {
				gvt_vgpu_err("intel:vfio_set_irqs_validate_and_prepare failed\n");
				return -EINVAL;
			}
			if (data_size) {
				data = memdup_user((void __user *)(arg + minsz),
						   data_size);
				if (IS_ERR(data))
					return PTR_ERR(data);
			}
		}

		ret = intel_vgpu_set_irqs(vgpu, hdr.flags, hdr.index,
					hdr.start, hdr.count, data);
		kfree(data);

		return ret;
	} else if (cmd == VFIO_DEVICE_RESET) {
		intel_gvt_reset_vgpu(vgpu);
		return 0;
	} else if (cmd == VFIO_DEVICE_QUERY_GFX_PLANE) {
		struct vfio_device_gfx_plane_info dmabuf;
		int ret = 0;

		minsz = offsetofend(struct vfio_device_gfx_plane_info,
				    dmabuf_id);
		if (copy_from_user(&dmabuf, (void __user *)arg, minsz))
			return -EFAULT;
		if (dmabuf.argsz < minsz)
			return -EINVAL;

		ret = intel_vgpu_query_plane(vgpu, &dmabuf);
		if (ret != 0)
			return ret;

		return copy_to_user((void __user *)arg, &dmabuf, minsz) ?
								-EFAULT : 0;
	} else if (cmd == VFIO_DEVICE_GET_GFX_DMABUF) {
		__u32 dmabuf_id;

		if (get_user(dmabuf_id, (__u32 __user *)arg))
			return -EFAULT;
		return intel_vgpu_get_dmabuf(vgpu, dmabuf_id);
	}

	return -ENOTTY;
}

static ssize_t
vgpu_id_show(struct device *dev, struct device_attribute *attr,
	     char *buf)
{
	struct mdev_device *mdev = mdev_from_dev(dev);

	if (mdev) {
		struct intel_vgpu *vgpu = (struct intel_vgpu *)
			mdev_get_drvdata(mdev);
		return sprintf(buf, "%d\n", vgpu->id);
	}
	return sprintf(buf, "\n");
}

static DEVICE_ATTR_RO(vgpu_id);

static struct attribute *intel_vgpu_attrs[] = {
	&dev_attr_vgpu_id.attr,
	NULL
};

static const struct attribute_group intel_vgpu_group = {
	.name = "intel_vgpu",
	.attrs = intel_vgpu_attrs,
};

static const struct attribute_group *intel_vgpu_groups[] = {
	&intel_vgpu_group,
	NULL,
};

const struct mdev_parent_ops intel_vgpu_mdev_ops = {
	.mdev_attr_groups       = intel_vgpu_groups,
	.supported_type_groups	= gvt_vgpu_type_groups,
	.create			= intel_vgpu_create,
	.remove			= intel_vgpu_remove,

	.open			= intel_vgpu_open,
	.release		= intel_vgpu_release,

	.read			= intel_vgpu_read,
	.write			= intel_vgpu_write,
	.mmap			= intel_vgpu_mmap,
	.ioctl			= intel_vgpu_ioctl,
};

int intel_gvt_page_track_add(struct intel_vgpu *info, u64 gfn)
{
	struct kvm *kvm = info->kvm;
	struct kvm_memory_slot *slot;
	int idx;

	if (!info->attached)
		return -ESRCH;

	idx = srcu_read_lock(&kvm->srcu);
	slot = gfn_to_memslot(kvm, gfn);
	if (!slot) {
		srcu_read_unlock(&kvm->srcu, idx);
		return -EINVAL;
	}

	write_lock(&kvm->mmu_lock);

	if (kvmgt_gfn_is_write_protected(info, gfn))
		goto out;

	kvm_slot_page_track_add_page(kvm, slot, gfn, KVM_PAGE_TRACK_WRITE);
	kvmgt_protect_table_add(info, gfn);

out:
	write_unlock(&kvm->mmu_lock);
	srcu_read_unlock(&kvm->srcu, idx);
	return 0;
}

int intel_gvt_page_track_remove(struct intel_vgpu *info, u64 gfn)
{
	struct kvm *kvm = info->kvm;
	struct kvm_memory_slot *slot;
	int idx;

	if (!info->attached)
		return 0;

	idx = srcu_read_lock(&kvm->srcu);
	slot = gfn_to_memslot(kvm, gfn);
	if (!slot) {
		srcu_read_unlock(&kvm->srcu, idx);
		return -EINVAL;
	}

	write_lock(&kvm->mmu_lock);

	if (!kvmgt_gfn_is_write_protected(info, gfn))
		goto out;

	kvm_slot_page_track_remove_page(kvm, slot, gfn, KVM_PAGE_TRACK_WRITE);
	kvmgt_protect_table_del(info, gfn);

out:
	write_unlock(&kvm->mmu_lock);
	srcu_read_unlock(&kvm->srcu, idx);
	return 0;
}

static void kvmgt_page_track_write(struct kvm_vcpu *vcpu, gpa_t gpa,
		const u8 *val, int len,
		struct kvm_page_track_notifier_node *node)
{
	struct intel_vgpu *info =
		container_of(node, struct intel_vgpu, track_node);

	if (kvmgt_gfn_is_write_protected(info, gpa_to_gfn(gpa)))
		intel_vgpu_page_track_handler(info, gpa,
						     (void *)val, len);
}

static void kvmgt_page_track_flush_slot(struct kvm *kvm,
		struct kvm_memory_slot *slot,
		struct kvm_page_track_notifier_node *node)
{
	int i;
	gfn_t gfn;
	struct intel_vgpu *info =
		container_of(node, struct intel_vgpu, track_node);

	write_lock(&kvm->mmu_lock);
	for (i = 0; i < slot->npages; i++) {
		gfn = slot->base_gfn + i;
		if (kvmgt_gfn_is_write_protected(info, gfn)) {
			kvm_slot_page_track_remove_page(kvm, slot, gfn,
						KVM_PAGE_TRACK_WRITE);
			kvmgt_protect_table_del(info, gfn);
		}
	}
	write_unlock(&kvm->mmu_lock);
}

void intel_vgpu_detach_regions(struct intel_vgpu *vgpu)
{
	int i;

	if (!vgpu->region)
		return;

	for (i = 0; i < vgpu->num_regions; i++)
		if (vgpu->region[i].ops->release)
			vgpu->region[i].ops->release(vgpu,
					&vgpu->region[i]);
	vgpu->num_regions = 0;
	kfree(vgpu->region);
	vgpu->region = NULL;
}

int intel_gvt_dma_map_guest_page(struct intel_vgpu *vgpu, unsigned long gfn,
		unsigned long size, dma_addr_t *dma_addr)
{
	struct gvt_dma *entry;
	int ret;

	if (!vgpu->attached)
		return -EINVAL;

	mutex_lock(&vgpu->cache_lock);

	entry = __gvt_cache_find_gfn(vgpu, gfn);
	if (!entry) {
		ret = gvt_dma_map_page(vgpu, gfn, dma_addr, size);
		if (ret)
			goto err_unlock;

		ret = __gvt_cache_add(vgpu, gfn, *dma_addr, size);
		if (ret)
			goto err_unmap;
	} else if (entry->size != size) {
		/* the same gfn with different size: unmap and re-map */
		gvt_dma_unmap_page(vgpu, gfn, entry->dma_addr, entry->size);
		__gvt_cache_remove_entry(vgpu, entry);

		ret = gvt_dma_map_page(vgpu, gfn, dma_addr, size);
		if (ret)
			goto err_unlock;

		ret = __gvt_cache_add(vgpu, gfn, *dma_addr, size);
		if (ret)
			goto err_unmap;
	} else {
		kref_get(&entry->ref);
		*dma_addr = entry->dma_addr;
	}

	mutex_unlock(&vgpu->cache_lock);
	return 0;

err_unmap:
	gvt_dma_unmap_page(vgpu, gfn, *dma_addr, size);
err_unlock:
	mutex_unlock(&vgpu->cache_lock);
	return ret;
}

int intel_gvt_dma_pin_guest_page(struct intel_vgpu *vgpu, dma_addr_t dma_addr)
{
	struct gvt_dma *entry;
	int ret = 0;

	if (!vgpu->attached)
		return -ENODEV;

	mutex_lock(&vgpu->cache_lock);
	entry = __gvt_cache_find_dma_addr(vgpu, dma_addr);
	if (entry)
		kref_get(&entry->ref);
	else
		ret = -ENOMEM;
	mutex_unlock(&vgpu->cache_lock);

	return ret;
}

static void __gvt_dma_release(struct kref *ref)
{
	struct gvt_dma *entry = container_of(ref, typeof(*entry), ref);

	gvt_dma_unmap_page(entry->vgpu, entry->gfn, entry->dma_addr,
			   entry->size);
	__gvt_cache_remove_entry(entry->vgpu, entry);
}

void intel_gvt_dma_unmap_guest_page(struct intel_vgpu *vgpu,
		dma_addr_t dma_addr)
{
	struct gvt_dma *entry;

	if (!vgpu->attached)
		return;

	mutex_lock(&vgpu->cache_lock);
	entry = __gvt_cache_find_dma_addr(vgpu, dma_addr);
	if (entry)
		kref_put(&entry->ref, __gvt_dma_release);
	mutex_unlock(&vgpu->cache_lock);
}

static int __init kvmgt_init(void)
{
	return intel_gvt_set_ops(&intel_gvt_vgpu_ops);
}

static void __exit kvmgt_exit(void)
{
	intel_gvt_clear_ops(&intel_gvt_vgpu_ops);
}

module_init(kvmgt_init);
module_exit(kvmgt_exit);

MODULE_LICENSE("GPL and additional rights");
MODULE_AUTHOR("Intel Corporation");
