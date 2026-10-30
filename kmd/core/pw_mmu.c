// SPDX-License-Identifier: MIT
/*
 * pw_mmu.c - GPU page tables and address-space management.
 *
 * Page tables: AArch64 VMSAv8-64 stage-1, 4 KiB granule, 4 KiB pages and 2 MiB blocks, written
 * from the Arm ARM descriptor format (NOT derived from Linux io-pgtable-arm.c, which is GPL-only).
 * Each table is one 4 KiB pw_os_mem with a cached kernel mapping; the GPU walker is not coherent
 * with the CPU on RK3588, so every descriptor store is cleaned to the PoC with pw_os_mem_flush()
 * before anything that could make the GPU walk it (a newly allocated table is zeroed and cleaned
 * before its table descriptor is published).
 *
 * AS management, lock regions, MMU fault decode: derived from Linux panthor_mmu.c (dual
 * GPL-2.0/MIT, used under MIT; Copyright 2019 Linaro Ltd, 2023 Collabora ltd.).
 */

#include "pw_mmu.h"

#define PW_AS_TIMEOUT_US	100000	/* mmu.c:546 */

static inline unsigned int level_shift(unsigned int lvl)
{
	return 12 + 9 * (3 - lvl);
}

static inline unsigned int level_index(uint64_t va, unsigned int lvl)
{
	return (unsigned int)(va >> level_shift(lvl)) & (PW_PT_ENTRIES - 1);
}

/* Lower attributes [11:2] and upper attributes [63:50] are at the same place in block and
 * page descriptors, so they survive a block split unchanged. */
#define PW_PTE_ATTRS_ALL	(PW_GENMASK64(11, 2) | PW_GENMASK64(63, 50))

/* ------------------------------------------------------------------------------------------ */
/* attribute helpers                                                                          */
/* ------------------------------------------------------------------------------------------ */

/*
 * Descriptor attributes for a mapping. Chosen to match what panthor's ARM_64_LPAE_S1 tables
 * contain for the same DRM_PANTHOR_VM_BIND_OP_MAP_* flags (flags_to_prot(), mmu.c:1074):
 * AF always; nG (stage-1, no effect on Mali); AP[1]=1 (EL0 accessible - Mali does not use the
 * privilege distinction); AP[2]=1 for read-only; XN for no-exec. Cached mappings use the WB
 * MAIR index and inner-shareable, which mair_to_memattr() maps to Mali's "internal shareable"
 * (SH_MIDGARD_INNER) on a non-coherent system; uncached ones use NC + outer-shareable.
 */
uint64_t pw_mmu_pte_attrs(uint32_t flags, bool coherent)
{
	uint64_t pte = PW_PTE_AF | PW_PTE_NG | PW_PTE_AP_UNPRIV;

	(void)coherent;
	if (flags & PW_VM_MAP_READONLY)
		pte |= PW_PTE_AP_RDONLY;
	if (flags & PW_VM_MAP_NOEXEC)
		pte |= PW_PTE_XN;
	if (flags & PW_VM_MAP_DEVICE)
		pte |= PW_PTE_ATTRINDX(PW_MAIR_IDX_DEV) | PW_PTE_SH_OS;
	else if (flags & PW_VM_MAP_UNCACHED)
		pte |= PW_PTE_ATTRINDX(PW_MAIR_IDX_NC) | PW_PTE_SH_OS;
	else
		pte |= PW_PTE_ATTRINDX(PW_MAIR_IDX_WB) | PW_PTE_SH_IS;
	return pte;
}

/* mmu.c:2178-2217 */
uint64_t pw_mmu_mair_to_memattr(uint64_t mair, bool coherent)
{
	uint64_t memattr = 0;

	for (unsigned int i = 0; i < 8; i++) {
		uint8_t in_attr = (uint8_t)(mair >> (8 * i)), out_attr;
		uint8_t outer = in_attr >> 4, inner = in_attr & 0xf;

		/* Caching needs both inner and outer write-back; anything else, including
		 * Device memory, becomes non-cacheable. */
		if (!(outer & 3) || !(outer & 4) || !(inner & 4)) {
			out_attr = AS_MEMATTR_AARCH64_INNER_OUTER_NC |
				   AS_MEMATTR_AARCH64_SH_MIDGARD_INNER |
				   AS_MEMATTR_AARCH64_INNER_ALLOC_EXPL(false, false);
		} else {
			out_attr = AS_MEMATTR_AARCH64_INNER_OUTER_WB |
				   AS_MEMATTR_AARCH64_INNER_ALLOC_EXPL(inner & 1, inner & 2);
			/* Non-coherent: inner/outer shareable WB is not allowed by the Mali spec,
			 * so SH_IS must mean Mali-internal shareable. */
			out_attr |= coherent ? AS_MEMATTR_AARCH64_SH_CPU_INNER :
					       AS_MEMATTR_AARCH64_SH_MIDGARD_INNER;
		}
		memattr |= (uint64_t)out_attr << (8 * i);
	}
	return memattr;
}

/*
 * mmu.c:570-597: the lock region is a naturally aligned power-of-two block, encoded as
 * log2(size) - 1 in the low bits of the (aligned) start address, minimum 32 KiB.
 */
uint64_t pw_mmu_pack_region_range(uint64_t *region_start, uint64_t *size)
{
	uint64_t region_end;
	unsigned int width;

	if (!*size)
		return 0;
	region_end = *region_start + *size;
	width = PW_MAX(pw_fls64(*region_start ^ (region_end - 1)), 15u) - 1;
	*region_start &= PW_GENMASK64(63, width);
	*size = 1ull << (width + 1);
	return width | *region_start;
}

/* mmu.c:1758-1774 */
const char *pw_mmu_access_type_name(uint32_t fault_status)
{
	switch (fault_status & AS_FAULTSTATUS_ACCESS_TYPE_MASK) {
	case AS_FAULTSTATUS_ACCESS_TYPE_ATOMIC:
		return "ATOMIC";
	case AS_FAULTSTATUS_ACCESS_TYPE_READ:
		return "READ";
	case AS_FAULTSTATUS_ACCESS_TYPE_WRITE:
		return "WRITE";
	default:
		return "EXECUTE";
	}
}

/* ------------------------------------------------------------------------------------------ */
/* translation tables                                                                         */
/* ------------------------------------------------------------------------------------------ */

static void pt_flush(struct pw_vm *vm, struct pw_pt *pt, unsigned int first, unsigned int count)
{
	if (count)
		pw_os_mem_flush(vm->dev->os, pt->mem, (size_t)first * 8, (size_t)count * 8);
}

static struct pw_pt *pt_alloc(struct pw_vm *vm, unsigned int level)
{
	struct pw_os *os = vm->dev->os;
	struct pw_pt *pt;

	pt = pw_os_zalloc(os, sizeof(*pt));
	if (!pt)
		return NULL;
	if (pw_os_mem_alloc(os, PW_SZ_4K, PW_MEM_KMAP, &pt->mem))
		goto err_free;
	pt->ptes = pw_os_mem_kva(pt->mem);
	pt->pa = pw_os_mem_page_pa(pt->mem, 0);
	if (!pt->ptes || (pt->pa & (PW_SZ_4K - 1)) || (pt->pa & ~PW_PTE_OA_MASK) ||
	    (vm->dev->mmu.pa_bits && vm->dev->mmu.pa_bits < 64 &&
	     (pt->pa >> vm->dev->mmu.pa_bits))) {
		pw_err(vm->dev, "page-table page at PA 0x%llx unusable by the GPU",
		       (unsigned long long)pt->pa);
		goto err_mem;
	}
	if (level < 3) {
		pt->child = pw_os_zalloc(os, PW_PT_ENTRIES * sizeof(*pt->child));
		if (!pt->child)
			goto err_mem;
	}
	pw_memset(pt->ptes, 0, PW_SZ_4K);
	pt_flush(vm, pt, 0, PW_PT_ENTRIES);
	vm->pt_pages++;
	return pt;

err_mem:
	pw_os_mem_free(os, pt->mem);
err_free:
	pw_os_free(os, pt);
	return NULL;
}

static void pt_free(struct pw_vm *vm, struct pw_pt *pt, unsigned int level)
{
	struct pw_os *os = vm->dev->os;

	if (!pt)
		return;
	if (pt->child) {
		for (unsigned int i = 0; i < PW_PT_ENTRIES; i++)
			pt_free(vm, pt->child[i], level + 1);
		pw_os_free(os, pt->child);
	}
	pw_os_mem_free(os, pt->mem);
	pw_os_free(os, pt);
	vm->pt_pages--;
}

/* Descend to the table at @target level covering @va, allocating missing tables. */
static int pt_walk_alloc(struct pw_vm *vm, uint64_t va, unsigned int target, struct pw_pt **out)
{
	struct pw_pt *pt = vm->root;

	for (unsigned int lvl = vm->start_level; lvl < target; lvl++) {
		unsigned int idx = level_index(va, lvl);
		uint64_t e = pt->ptes[idx];

		if (!(e & PW_PTE_VALID)) {
			struct pw_pt *child = pt_alloc(vm, lvl + 1);

			if (!child)
				return PW_ENOMEM;
			pt->child[idx] = child;
			PW_WRITE_ONCE(pt->ptes[idx], (child->pa & PW_PTE_OA_MASK) | PW_PTE_TYPE_TABLE);
			pt->nvalid++;
			pt_flush(vm, pt, idx, 1);
		} else if ((e & PW_PTE_TYPE_MASK) != PW_PTE_TYPE_TABLE || !pt->child[idx]) {
			return PW_EBUSY;	/* a block mapping covers this VA */
		}
		pt = pt->child[idx];
	}
	*out = pt;
	return PW_OK;
}

static int vm_map_pa_locked(struct pw_vm *vm, uint64_t va, uint64_t pa, uint64_t size,
			    uint64_t attrs, uint64_t *mapped)
{
	struct pw_pt *pt;
	int ret;

	while (size) {
		if (!((va | pa) & (PW_SZ_2M - 1)) && size >= PW_SZ_2M) {
			unsigned int idx = level_index(va, 2);

			ret = pt_walk_alloc(vm, va, 2, &pt);
			if (ret)
				return ret;
			/* A level-3 table here (possibly empty: unmap keeps tables until VM destroy)
			 * isn't an overlap by itself; use 4K pages, which detect real conflicts.
			 * Replacing an empty table by a block would need a deferred free. */
			if ((pt->ptes[idx] & PW_PTE_VALID) &&
			    (pt->ptes[idx] & PW_PTE_TYPE_MASK) == PW_PTE_TYPE_TABLE && pt->child[idx])
				goto map_4k;
			if (pt->ptes[idx] & PW_PTE_VALID)
				return PW_EBUSY;
			PW_WRITE_ONCE(pt->ptes[idx], (pa & PW_PTE_OA_MASK) | attrs | PW_PTE_TYPE_BLOCK);
			pt->nvalid++;
			pt_flush(vm, pt, idx, 1);
			va += PW_SZ_2M;
			pa += PW_SZ_2M;
			size -= PW_SZ_2M;
			*mapped += PW_SZ_2M;
			continue;
		}
map_4k:
		ret = pt_walk_alloc(vm, va, 3, &pt);
		if (ret)
			return ret;

		/* Fill up to the end of this level-3 table; a 2 MiB-aligned VA starts a new table
		 * so block eligibility is re-evaluated there. */
		unsigned int idx = level_index(va, 3);
		unsigned int n = PW_PT_ENTRIES - idx, k;

		if ((uint64_t)n > (size >> 12))
			n = (unsigned int)(size >> 12);
		for (k = 0; k < n; k++) {
			if (pt->ptes[idx + k] & PW_PTE_VALID) {
				pt_flush(vm, pt, idx, k);
				return PW_EBUSY;
			}
			PW_WRITE_ONCE(pt->ptes[idx + k],
				      ((pa + ((uint64_t)k << 12)) & PW_PTE_OA_MASK) | attrs |
				      PW_PTE_TYPE_PAGE);
			pt->nvalid++;
			*mapped += PW_SZ_4K;
		}
		pt_flush(vm, pt, idx, n);
		va += (uint64_t)n << 12;
		pa += (uint64_t)n << 12;
		size -= (uint64_t)n << 12;
	}
	return PW_OK;
}

/* Replace the 2 MiB block at @pt2[@idx] by an equivalent level-3 table. */
static int pt_split_block(struct pw_vm *vm, struct pw_pt *pt2, unsigned int idx)
{
	uint64_t e = pt2->ptes[idx];
	uint64_t base = e & PW_PTE_OA_MASK & ~(PW_SZ_2M - 1);
	uint64_t attrs = e & PW_PTE_ATTRS_ALL;
	struct pw_pt *l3 = pt_alloc(vm, 3);

	if (!l3)
		return PW_ENOMEM;
	for (unsigned int i = 0; i < PW_PT_ENTRIES; i++)
		l3->ptes[i] = (base + ((uint64_t)i << 12)) | attrs | PW_PTE_TYPE_PAGE;
	l3->nvalid = PW_PT_ENTRIES;
	pt_flush(vm, l3, 0, PW_PT_ENTRIES);

	/* Block -> table without break-before-make: the caller holds an AS lock region over
	 * the range (or the AS is inactive), and the unlock path flushes the GPU caches/TLBs. */
	pt2->child[idx] = l3;
	PW_WRITE_ONCE(pt2->ptes[idx], (l3->pa & PW_PTE_OA_MASK) | PW_PTE_TYPE_TABLE);
	pt_flush(vm, pt2, idx, 1);
	return PW_OK;
}

static int vm_unmap_locked(struct pw_vm *vm, uint64_t va, uint64_t size)
{
	const uint64_t end = va + size;

	while (va < end) {
		struct pw_pt *pt = vm->root;
		unsigned int lvl, idx;
		uint64_t e;
		bool skipped = false;

		for (lvl = vm->start_level; lvl < 2; lvl++) {
			idx = level_index(va, lvl);
			e = pt->ptes[idx];
			if (!(e & PW_PTE_VALID)) {
				va = (va | ((1ull << level_shift(lvl)) - 1)) + 1;
				skipped = true;
				break;
			}
			if ((e & PW_PTE_TYPE_MASK) != PW_PTE_TYPE_TABLE || !pt->child[idx]) {
				pw_err(vm->dev, "unexpected level-%u block at VA 0x%llx", lvl,
				       (unsigned long long)va);
				return PW_EIO;
			}
			pt = pt->child[idx];
		}
		if (skipped)
			continue;

		/* level 2 */
		const uint64_t blk_start = va & ~(PW_SZ_2M - 1);
		const uint64_t blk_end = blk_start + PW_SZ_2M;

		idx = level_index(va, 2);
		e = pt->ptes[idx];
		if (!(e & PW_PTE_VALID)) {
			va = blk_end;
			continue;
		}
		if ((e & PW_PTE_TYPE_MASK) == PW_PTE_TYPE_BLOCK) {
			if (va == blk_start && end >= blk_end) {
				PW_WRITE_ONCE(pt->ptes[idx], 0);
				pt->nvalid--;
				pt_flush(vm, pt, idx, 1);
				va = blk_end;
				continue;
			}
			int ret = pt_split_block(vm, pt, idx);

			if (ret)
				return ret;
		}

		struct pw_pt *l3 = pt->child[idx];
		unsigned int i0 = level_index(va, 3), n = PW_PT_ENTRIES - i0;

		if ((uint64_t)n > ((end - va) >> 12))
			n = (unsigned int)((end - va) >> 12);
		for (unsigned int k = 0; k < n; k++) {
			if (l3->ptes[i0 + k] & PW_PTE_VALID) {
				PW_WRITE_ONCE(l3->ptes[i0 + k], 0);
				l3->nvalid--;
			}
		}
		pt_flush(vm, l3, i0, n);
		/* TODO: free empty tables once we can defer the free past the TLB flush. */
		va += (uint64_t)n << 12;
	}
	return PW_OK;
}

int pw_vm_translate(struct pw_vm *vm, uint64_t va, uint64_t *pa, uint64_t *pte, int *level)
{
	struct pw_pt *pt = vm->root;

	if (va >= vm->va_size)
		return PW_EINVAL;

	for (unsigned int lvl = vm->start_level; lvl <= 3; lvl++) {
		unsigned int idx = level_index(va, lvl);
		uint64_t e = PW_READ_ONCE(pt->ptes[idx]);
		uint64_t type = e & PW_PTE_TYPE_MASK;

		if (!(e & PW_PTE_VALID))
			return PW_ENOENT;
		if (lvl == 3 || type == PW_PTE_TYPE_BLOCK) {
			uint64_t bs = 1ull << level_shift(lvl);

			if ((lvl == 3 && type != PW_PTE_TYPE_PAGE) || lvl == 0)
				return PW_ENOENT;
			if (pa)
				*pa = (e & PW_PTE_OA_MASK & ~(bs - 1)) | (va & (bs - 1));
			if (pte)
				*pte = e;
			if (level)
				*level = (int)lvl;
			return PW_OK;
		}
		pt = pt->child[idx];
		if (!pt)
			return PW_EIO;
	}
	return PW_ENOENT;
}

/* ------------------------------------------------------------------------------------------ */
/* AS slots                                                                                   */
/* ------------------------------------------------------------------------------------------ */

static int as_wait_ready(struct pw_device *dev, uint32_t as)
{
	uint32_t val;
	int ret;

	/* mmu.c:537-555 */
	ret = pw_poll_reg32(dev, AS_STATUS(as), AS_STATUS_AS_ACTIVE, 0, 10, PW_AS_TIMEOUT_US, &val);
	if (ret)
		pw_err(dev, "AS%u: AS_ACTIVE bit stuck (AS_STATUS 0x%08x)", as, val);
	return ret;
}

int pw_mmu_as_send_cmd(struct pw_device *dev, uint32_t as, uint32_t cmd)
{
	int ret = as_wait_ready(dev, as);

	if (!ret) {
		gpu_write(dev, AS_COMMAND(as), cmd);
		ret = as_wait_ready(dev, as);
	}
	return ret;
}

/* mmu.c:604-617 */
static int as_enable(struct pw_device *dev, uint32_t as, uint64_t transtab, uint64_t transcfg,
		     uint64_t memattr)
{
	pw_irq_enable_events(dev, PW_IRQ_MMU, MMU_INT_AS_FAULT(as));

	gpu_write64(dev, AS_TRANSTAB(as), transtab);
	gpu_write64(dev, AS_MEMATTR(as), memattr);
	gpu_write64(dev, AS_TRANSCFG(as), transcfg);

	return pw_mmu_as_send_cmd(dev, as, AS_COMMAND_UPDATE);
}

/* mmu.c:619-655. Called with mmu.slots_lock held. */
static int as_disable(struct pw_device *dev, uint32_t as, bool recycle)
{
	struct pw_vm *vm = dev->mmu.slots[as];
	int ret;

	pw_irq_disable_events(dev, PW_IRQ_MMU, MMU_INT_AS_FAULT(as));

	/* Flush+invalidate RW caches, invalidate RO ones. */
	ret = pw_gpu_flush_caches(dev, CACHE_CLEAN | CACHE_INV, CACHE_CLEAN | CACHE_INV, CACHE_INV);
	if (ret)
		return ret;

	if (vm && vm->locked.size) {
		ret = pw_mmu_as_send_cmd(dev, as, AS_COMMAND_UNLOCK);
		if (ret)
			return ret;
	}

	if (recycle)
		return PW_OK;

	gpu_write64(dev, AS_TRANSTAB(as), 0);
	gpu_write64(dev, AS_MEMATTR(as), 0);
	gpu_write64(dev, AS_TRANSCFG(as), AS_TRANSCFG_ADRMODE_UNMAPPED);

	return pw_mmu_as_send_cmd(dev, as, AS_COMMAND_UPDATE);
}

uint64_t pw_vm_root_pa(const struct pw_vm *vm)
{
	return vm->root->pa;
}

/* mmu.c:788-793 */
uint64_t pw_vm_transcfg(const struct pw_vm *vm)
{
	uint64_t transcfg = AS_TRANSCFG_PTW_MEMATTR_WB | AS_TRANSCFG_PTW_RA |
			    AS_TRANSCFG_ADRMODE_AARCH64_4K |
			    AS_TRANSCFG_INA_BITS(55 - vm->dev->mmu.va_bits);

	if (vm->dev->coherent)
		transcfg |= AS_TRANSCFG_PTW_SH_OS;
	return transcfg;
}

static void vm_release_as_locked(struct pw_vm *vm)
{
	struct pw_mmu *mmu = &vm->dev->mmu;

	if (vm->as < 0)
		return;
	mmu->slots[vm->as] = NULL;
	mmu->alloc_mask &= ~PW_BIT(vm->as);
	vm->as = -1;
}

/*
 * mmu.c:709-830 (panthor_vm_active), minus the LRU: GPU VMs get the lowest free AS >= 1 and we
 * fail with PW_EBUSY when all are taken (TODO: LRU eviction of idle VMs, needed at M4).
 */
int pw_vm_activate(struct pw_vm *vm)
{
	struct pw_device *dev = vm->dev;
	struct pw_mmu *mmu = &dev->mmu;
	uint32_t present = dev->gpu_info.as_present & PW_GENMASK(PW_MAX_AS - 1, 0);
	int ret = PW_OK, as;

	pw_os_mutex_lock(vm->op_lock);
	pw_os_mutex_lock(mmu->slots_lock);

	as = vm->as;
	if (as >= 0) {
		/* Unhandled fault on this AS: the MMU was disabled, re-enable it. */
		if (mmu->faulty_mask & PW_BIT(as))
			goto enable;
		goto out;
	}

	if (vm->for_mcu) {
		if (mmu->alloc_mask & PW_BIT(0)) {
			ret = PW_EBUSY;
			goto out;
		}
		as = 0;
	} else {
		uint32_t free = present & ~mmu->alloc_mask & ~PW_BIT(0);

		if (!free) {
			ret = PW_EBUSY;
			goto out;
		}
		as = (int)pw_ffs32(free) - 1;
	}
	if (!(present & PW_BIT(as))) {
		ret = PW_EBUSY;
		goto out;
	}
	vm->as = as;
	mmu->alloc_mask |= PW_BIT(as);
	mmu->slots[as] = vm;

enable:
	vm->unhandled_fault = false;
	if (mmu->faulty_mask & PW_BIT(as)) {
		gpu_write(dev, MMU_INT_BASE + INT_CLEAR, MMU_INT_AS_FAULT(as));
		mmu->faulty_mask &= ~PW_BIT(as);
	}
	ret = as_enable(dev, (uint32_t)as, vm->root->pa, pw_vm_transcfg(vm), mmu->memattr);
	if (ret) {
		pw_err(dev, "AS%d: enable failed (%d)", as, ret);
		vm_release_as_locked(vm);
	} else {
		pw_dbg(dev, "AS%d: TRANSTAB 0x%llx TRANSCFG 0x%llx MEMATTR 0x%016llx", as,
		       (unsigned long long)vm->root->pa, (unsigned long long)pw_vm_transcfg(vm),
		       (unsigned long long)mmu->memattr);
	}
out:
	pw_os_mutex_unlock(mmu->slots_lock);
	pw_os_mutex_unlock(vm->op_lock);
	return ret;
}

void pw_vm_deactivate(struct pw_vm *vm)
{
	struct pw_mmu *mmu = &vm->dev->mmu;

	pw_os_mutex_lock(vm->op_lock);
	pw_os_mutex_lock(mmu->slots_lock);
	if (vm->as >= 0) {
		int ret = as_disable(vm->dev, (uint32_t)vm->as, false);

		if (ret)
			pw_err(vm->dev, "AS%d: disable failed (%d)", vm->as, ret);
		mmu->faulty_mask &= ~PW_BIT(vm->as);
		vm_release_as_locked(vm);
	}
	vm->locked.start = 0;
	vm->locked.size = 0;
	pw_os_mutex_unlock(mmu->slots_lock);
	pw_os_mutex_unlock(vm->op_lock);
}

int pw_vm_as(struct pw_vm *vm)
{
	return vm->as;
}

/* mmu.c:1776-1825, minus the remap-union logic (no VM_BIND remaps yet). op_lock held. */
static int vm_lock_region(struct pw_vm *vm, uint64_t start, uint64_t size)
{
	struct pw_device *dev = vm->dev;
	int ret = PW_OK;

	pw_os_mutex_lock(dev->mmu.slots_lock);
	if (vm->as >= 0 && size) {
		gpu_write64(dev, AS_LOCKADDR(vm->as), pw_mmu_pack_region_range(&start, &size));
		ret = pw_mmu_as_send_cmd(dev, (uint32_t)vm->as, AS_COMMAND_LOCK);
	}
	if (!ret) {
		vm->locked.start = start;
		vm->locked.size = vm->as >= 0 ? size : 0;
	}
	pw_os_mutex_unlock(dev->mmu.slots_lock);
	return ret;
}

/* mmu.c:1827-1856. op_lock held. */
static int vm_unlock_region(struct pw_vm *vm)
{
	struct pw_device *dev = vm->dev;
	int ret = PW_OK;

	pw_os_mutex_lock(dev->mmu.slots_lock);
	if (vm->as >= 0 && vm->locked.size) {
		ret = pw_gpu_flush_caches(dev, CACHE_CLEAN | CACHE_INV, CACHE_CLEAN | CACHE_INV,
					  CACHE_INV);
		if (!ret)
			ret = pw_mmu_as_send_cmd(dev, (uint32_t)vm->as, AS_COMMAND_UNLOCK);
		if (ret)
			pw_err(dev, "AS%d: flush/unlock failed (%d), GPU needs a reset", vm->as, ret);
	}
	vm->locked.start = 0;
	vm->locked.size = 0;
	pw_os_mutex_unlock(dev->mmu.slots_lock);
	return ret;
}

/* ------------------------------------------------------------------------------------------ */
/* VM objects                                                                                 */
/* ------------------------------------------------------------------------------------------ */

int pw_vm_create(struct pw_device *dev, bool for_mcu, struct pw_vm **out)
{
	uint32_t va_bits = dev->mmu.va_bits;
	struct pw_vm *vm;

	if (!dev->mmu.ready)
		return PW_EINVAL;
	/* 4K granule, at most 4 levels: 48-bit VAs. Below 32 bits the MCU VM wouldn't fit. */
	if (va_bits < 32 || va_bits > 48) {
		pw_err(dev, "unsupported MMU VA bits %u", va_bits);
		return PW_EOPNOTSUPP;
	}

	vm = pw_os_zalloc(dev->os, sizeof(*vm));
	if (!vm)
		return PW_ENOMEM;
	vm->dev = dev;
	vm->for_mcu = for_mcu;
	vm->as = -1;
	/* The CSF MCU is a Cortex-M7 and can only address 4 GiB (mmu.c:2893). */
	vm->va_size = for_mcu ? PW_SZ_4G : (1ull << va_bits);
	/* Number of levels needed to resolve va_bits with 9 bits per level above the 12-bit page
	 * offset; the root table may be partially used (e.g. 40 bits -> 2 entries at level 0). */
	vm->start_level = 4 - PW_DIV_ROUND_UP(va_bits - 12, 9);
	vm->op_lock = pw_os_mutex_create(dev->os);
	if (!vm->op_lock)
		goto err;
	vm->root = pt_alloc(vm, vm->start_level);
	if (!vm->root)
		goto err;
	*out = vm;
	return PW_OK;

err:
	if (vm->op_lock)
		pw_os_mutex_destroy(dev->os, vm->op_lock);
	pw_os_free(dev->os, vm);
	return PW_ENOMEM;
}

void pw_vm_destroy(struct pw_vm *vm)
{
	if (!vm)
		return;
	pw_vm_deactivate(vm);
	pt_free(vm, vm->root, vm->start_level);
	pw_os_mutex_destroy(vm->dev->os, vm->op_lock);
	pw_os_free(vm->dev->os, vm);
}

static int vm_check_range(struct pw_vm *vm, uint64_t va, uint64_t size)
{
	if (!size || (va & (PW_SZ_4K - 1)) || (size & (PW_SZ_4K - 1)))
		return PW_EINVAL;
	if (va >= vm->va_size || size > vm->va_size - va)
		return PW_EINVAL;
	return PW_OK;
}

static int vm_check_pa(struct pw_vm *vm, uint64_t pa, uint64_t size)
{
	uint32_t pa_bits = vm->dev->mmu.pa_bits;
	uint64_t last = pa + size - 1;

	if ((pa & (PW_SZ_4K - 1)) || last < pa || (last & ~PW_GENMASK64(47, 0)))
		return PW_EINVAL;
	if (pa_bits && pa_bits < 48 && (last >> pa_bits)) {
		pw_err(vm->dev, "PA 0x%llx beyond the GPU's %u PA bits", (unsigned long long)last,
		       pa_bits);
		return PW_ERANGE;
	}
	return PW_OK;
}

struct map_run {
	uint64_t va, pa, size;
};

typedef int (*next_run_fn)(void *ctx, struct map_run *run);

/* Common map path: lock region (if active), map runs, roll back on failure, unlock. */
static int vm_map_runs(struct pw_vm *vm, uint64_t va, uint64_t size, uint32_t flags,
		       next_run_fn next, void *ctx)
{
	uint64_t attrs = pw_mmu_pte_attrs(flags, vm->dev->coherent);
	uint64_t mapped = 0;
	struct map_run run;
	int ret, ret2;

	ret = vm_check_range(vm, va, size);
	if (ret)
		return ret;

	pw_os_mutex_lock(vm->op_lock);
	ret = vm_lock_region(vm, va, size);
	if (ret)
		goto out;

	while (!(ret = next(ctx, &run)) && run.size) {
		ret = vm_check_pa(vm, run.pa, run.size);
		if (ret)
			break;
		ret = vm_map_pa_locked(vm, run.va, run.pa, run.size, attrs, &mapped);
		if (ret)
			break;
	}
	if (ret && mapped)
		vm_unmap_locked(vm, va, mapped);

	ret2 = vm_unlock_region(vm);
	if (!ret)
		ret = ret2;
out:
	pw_os_mutex_unlock(vm->op_lock);
	return ret;
}

struct pa_ctx {
	uint64_t va, pa, size;
	bool done;
};

static int pa_next(void *ctx, struct map_run *run)
{
	struct pa_ctx *c = ctx;

	if (c->done) {
		run->size = 0;
		return PW_OK;
	}
	run->va = c->va;
	run->pa = c->pa;
	run->size = c->size;
	c->done = true;
	return PW_OK;
}

int pw_vm_map_pa(struct pw_vm *vm, uint64_t va, uint64_t pa, uint64_t size, uint32_t flags)
{
	struct pa_ctx c = { va, pa, size, false };

	return vm_map_runs(vm, va, size, flags, pa_next, &c);
}

struct mem_ctx {
	struct pw_os_mem *mem;
	uint64_t va;
	size_t page;		/* next page index in mem */
	size_t end_page;
};

/* Coalesce physically contiguous pages into one run. */
static int mem_next(void *ctx, struct map_run *run)
{
	struct mem_ctx *c = ctx;
	size_t n = 1;
	uint64_t pa0;

	if (c->page >= c->end_page) {
		run->size = 0;
		return PW_OK;
	}
	pa0 = pw_os_mem_page_pa(c->mem, c->page);
	while (c->page + n < c->end_page &&
	       pw_os_mem_page_pa(c->mem, c->page + n) == pa0 + ((uint64_t)n << 12))
		n++;
	run->va = c->va;
	run->pa = pa0;
	run->size = (uint64_t)n << 12;
	c->va += run->size;
	c->page += n;
	return PW_OK;
}

int pw_vm_map_mem(struct pw_vm *vm, uint64_t va, struct pw_os_mem *mem, size_t offset,
		  uint64_t size, uint32_t flags)
{
	struct mem_ctx c;

	if (!mem || (offset & (PW_SZ_4K - 1)) || (size & (PW_SZ_4K - 1)) ||
	    offset > pw_os_mem_size(mem) || size > pw_os_mem_size(mem) - offset)
		return PW_EINVAL;
	c.mem = mem;
	c.va = va;
	c.page = offset >> 12;
	c.end_page = (offset + size) >> 12;
	return vm_map_runs(vm, va, size, flags, mem_next, &c);
}

int pw_vm_unmap(struct pw_vm *vm, uint64_t va, uint64_t size)
{
	int ret, ret2;

	ret = vm_check_range(vm, va, size);
	if (ret)
		return ret;

	pw_os_mutex_lock(vm->op_lock);
	ret = vm_lock_region(vm, va, size);
	if (!ret) {
		ret = vm_unmap_locked(vm, va, size);
		ret2 = vm_unlock_region(vm);
		if (!ret)
			ret = ret2;
	}
	pw_os_mutex_unlock(vm->op_lock);
	return ret;
}

/* ------------------------------------------------------------------------------------------ */
/* MMU block                                                                                  */
/* ------------------------------------------------------------------------------------------ */

/* mmu.c:1858-1913 */
void pw_mmu_irq_handler(struct pw_device *dev, uint32_t status)
{
	struct pw_mmu *mmu = &dev->mmu;

	status &= MMU_INT_FAULT_MASK;	/* bits 16-31 are REQ_COMPLETE, never unmasked */
	while (status) {
		uint32_t as = pw_ffs32(status) - 1;
		uint32_t mask = MMU_INT_AS_FAULT(as);
		uint32_t fault_status = gpu_read(dev, AS_FAULTSTATUS(as));
		uint64_t addr = gpu_read64(dev, AS_FAULTADDRESS(as));
		uint32_t exception_type = AS_FAULTSTATUS_EXCEPTION_TYPE(fault_status);
		uint32_t access_type = (fault_status >> 8) & 0x3;
		uint32_t source_id = AS_FAULTSTATUS_SOURCE_ID(fault_status);
		struct pw_vm *vm;

		pw_os_mutex_lock(mmu->slots_lock);
		mmu->faulty_mask |= mask;
		vm = mmu->slots[as];

		pw_err(dev, "Unhandled page fault in AS%u at VA 0x%016llX%s", as,
		       (unsigned long long)addr, vm ? (vm->for_mcu ? " (MCU VM)" : "") : " (no VM)");
		pw_err(dev, "  raw fault status 0x%X: %s, exception type 0x%X: %s, "
		       "access type 0x%X: %s, source id 0x%X", fault_status,
		       (fault_status & AS_FAULTSTATUS_DECODER_FAULT) ? "DECODER FAULT" : "SLAVE FAULT",
		       exception_type, pw_exception_name(exception_type), access_type,
		       pw_mmu_access_type_name(fault_status), source_id);

		/* Faults aren't handled (no growable BOs): clear and let the job die. */
		gpu_write(dev, MMU_INT_BASE + INT_CLEAR, mask);
		if (vm)
			vm->unhandled_fault = true;

		/* Disable the MMU to kill jobs on this AS. */
		as_disable(dev, as, false);
		pw_os_mutex_unlock(mmu->slots_lock);

		status &= ~mask;
	}
	/* TODO(M3): report to the scheduler (panthor_sched_report_mmu_fault). */
}

int pw_mmu_init(struct pw_device *dev)
{
	struct pw_mmu *mmu = &dev->mmu;
	int ret;

	if (!mmu->slots_lock)
		mmu->slots_lock = pw_os_mutex_create(dev->os);
	if (!mmu->slots_lock)
		return PW_ENOMEM;
	mmu->va_bits = GPU_MMU_FEATURES_VA_BITS(dev->gpu_info.mmu_features);
	mmu->pa_bits = GPU_MMU_FEATURES_PA_BITS(dev->gpu_info.mmu_features);
	mmu->memattr = pw_mmu_mair_to_memattr(PW_MAIR_VALUE, dev->coherent);

	ret = pw_irq_init(dev, PW_IRQ_MMU, MMU_INT_BASE, pw_mmu_irq_handler);
	if (ret)
		return ret;
	/* mmu.c:3440-3441 */
	pw_irq_enable_events(dev, PW_IRQ_MMU, MMU_INT_FAULT_MASK);
	pw_irq_resume(dev, PW_IRQ_MMU);
	mmu->ready = true;
	return PW_OK;
}

void pw_mmu_fini(struct pw_device *dev)
{
	struct pw_mmu *mmu = &dev->mmu;

	if (!mmu->ready)
		return;
	mmu->ready = false;
	pw_os_mutex_lock(mmu->slots_lock);
	for (unsigned int i = 0; i < PW_MAX_AS; i++) {
		if (mmu->slots[i]) {
			pw_warn(dev, "AS%u still assigned at MMU teardown", i);
			as_disable(dev, i, false);
			mmu->slots[i]->as = -1;
			mmu->slots[i] = NULL;
		}
	}
	mmu->alloc_mask = 0;
	pw_os_mutex_unlock(mmu->slots_lock);
	pw_irq_suspend(dev, PW_IRQ_MMU);
}

void pw_mmu_release(struct pw_device *dev)
{
	if (dev->mmu.slots_lock)
		pw_os_mutex_destroy(dev->os, dev->mmu.slots_lock);
	dev->mmu.slots_lock = NULL;
}
