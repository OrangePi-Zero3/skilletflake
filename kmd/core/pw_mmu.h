/* SPDX-License-Identifier: MIT */
/*
 * pw_mmu.h - GPU virtual address spaces: AArch64 stage-1 page tables (4 KiB granule, 4K and 2M
 * mappings) and Mali address-space (AS) slot management.
 *
 * The descriptor format below is written from the Arm Architecture Reference Manual (VMSAv8-64
 * translation table descriptors, 4 KB granule). Linux's io-pgtable-arm.c is GPL-2.0-only and was
 * deliberately NOT used. AS programming follows panthor_mmu.c (dual GPL-2.0/MIT, used under MIT).
 */
#ifndef PW_MMU_H
#define PW_MMU_H

#include "pw_priv.h"

/* ---- VMSAv8-64 descriptors, 4 KB granule ---- */
#define PW_PT_ENTRIES		512
#define PW_PT_LEVELS		4		/* levels 0..3 */
#define PW_PTE_VALID		PW_BIT64(0)
#define PW_PTE_TYPE_MASK	3ull
#define PW_PTE_TYPE_BLOCK	1ull		/* levels 1-2: block */
#define PW_PTE_TYPE_TABLE	3ull		/* levels 0-2: next-level table */
#define PW_PTE_TYPE_PAGE	3ull		/* level 3: page */
#define PW_PTE_ATTRINDX(n)	((uint64_t)(n) << 2)	/* MAIR index, AttrIndx[2:0] */
#define PW_PTE_ATTRINDX_MASK	(7ull << 2)
#define PW_PTE_NS		PW_BIT64(5)
#define PW_PTE_AP_UNPRIV	PW_BIT64(6)	/* AP[1] */
#define PW_PTE_AP_RDONLY	PW_BIT64(7)	/* AP[2] */
#define PW_PTE_SH_NS		(0ull << 8)
#define PW_PTE_SH_OS		(2ull << 8)
#define PW_PTE_SH_IS		(3ull << 8)
#define PW_PTE_SH_MASK		(3ull << 8)
#define PW_PTE_AF		PW_BIT64(10)
#define PW_PTE_NG		PW_BIT64(11)
#define PW_PTE_CONT		PW_BIT64(52)
#define PW_PTE_PXN		PW_BIT64(53)
#define PW_PTE_XN		PW_BIT64(54)	/* UXN/XN */
#define PW_PTE_OA_MASK		PW_GENMASK64(47, 12)
#define PW_PTE_ATTR_MASK	(~PW_PTE_OA_MASK & ~PW_PTE_TYPE_MASK)

/*
 * Our MAIR layout (we own the page-table format, so we pick it; same attribute set as the Linux
 * ARM_64_LPAE_S1 tables panthor uses, so mair_to_memattr() yields the same AS_MEMATTR):
 *   idx0 0x44 Normal, inner+outer Non-cacheable
 *   idx1 0xff Normal, inner+outer Write-Back, R/W-allocate
 *   idx2 0x04 Device-nGnRE
 *   idx3 0xf4 Normal, inner Non-cacheable, outer Write-Back R/W-allocate
 */
#define PW_MAIR_IDX_NC		0
#define PW_MAIR_IDX_WB		1
#define PW_MAIR_IDX_DEV		2
#define PW_MAIR_IDX_INC_OWB	3
#define PW_MAIR_VALUE		(0x44ull | (0xffull << 8) | (0x04ull << 16) | (0xf4ull << 24))

/* Map flags: same values as DRM_PANTHOR_VM_BIND_OP_MAP_{READONLY,NOEXEC,UNCACHED}. */
#define PW_VM_MAP_READONLY	PW_BIT(0)
#define PW_VM_MAP_NOEXEC	PW_BIT(1)
#define PW_VM_MAP_UNCACHED	PW_BIT(2)
#define PW_VM_MAP_DEVICE	PW_BIT(8)	/* core-internal: Device-nGnRE (IOMMU_MMIO) */

struct pw_pt {
	struct pw_os_mem *mem;
	uint64_t *ptes;			/* kernel mapping of the 512 descriptors */
	uint64_t pa;
	struct pw_pt **child;		/* NULL at level 3 */
	uint32_t nvalid;
};

struct pw_vm {
	struct pw_device *dev;
	struct pw_os_mutex *op_lock;	/* serialises page-table updates on this VM */
	struct pw_pt *root;
	uint32_t start_level;
	uint64_t va_size;		/* VAs in [0, va_size) are mappable */
	bool for_mcu;
	int as;				/* AS slot, -1 when not active; under mmu.slots_lock */
	bool unhandled_fault;
	/* (added by core-mem) Number of unhandled faults ever reported on this VM; never reset
	 * (pw_vm_activate() clears unhandled_fault). __atomic. pw_uvm_is_unusable() reads it. */
	uint32_t fault_count;
	struct {
		uint64_t start, size;
	} locked;
	uint32_t pt_pages;		/* statistics */
};

int  pw_vm_create(struct pw_device *dev, bool for_mcu, struct pw_vm **out);
void pw_vm_destroy(struct pw_vm *vm);

/* Map @size bytes of @mem starting at byte @offset at GPU VA @va. Uses 2 MiB blocks when VA and
 * PA are both 2 MiB aligned and the pages are physically contiguous. All 4 KiB aligned.
 * Fails with PW_EBUSY if any page in the range is already mapped (nothing is changed then). */
int  pw_vm_map_mem(struct pw_vm *vm, uint64_t va, struct pw_os_mem *mem, size_t offset,
		   uint64_t size, uint32_t flags);
/* Same, for a physically contiguous range. */
int  pw_vm_map_pa(struct pw_vm *vm, uint64_t va, uint64_t pa, uint64_t size, uint32_t flags);
/* Unmap; partially unmapping a 2 MiB block splits it. Unmapped holes are skipped. */
int  pw_vm_unmap(struct pw_vm *vm, uint64_t va, uint64_t size);
/* Software walk (CPU view). Returns PW_ENOENT on a translation fault. @level may be NULL. */
int  pw_vm_translate(struct pw_vm *vm, uint64_t va, uint64_t *pa, uint64_t *pte, int *level);

/* Assign an AS slot (AS0 for the MCU VM, AS1.. otherwise) and program it. */
int  pw_vm_activate(struct pw_vm *vm);
void pw_vm_deactivate(struct pw_vm *vm);
int  pw_vm_as(struct pw_vm *vm);
uint64_t pw_vm_root_pa(const struct pw_vm *vm);
uint64_t pw_vm_transcfg(const struct pw_vm *vm);

/* Exposed for unit tests. */
uint64_t pw_mmu_pte_attrs(uint32_t flags, bool coherent);
uint64_t pw_mmu_mair_to_memattr(uint64_t mair, bool coherent);
uint64_t pw_mmu_pack_region_range(uint64_t *region_start, uint64_t *size);
const char *pw_mmu_access_type_name(uint32_t fault_status);
int  pw_mmu_as_send_cmd(struct pw_device *dev, uint32_t as, uint32_t cmd);

#endif /* PW_MMU_H */
