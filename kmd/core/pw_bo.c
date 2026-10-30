// SPDX-License-Identifier: MIT
/*
 * pw_bo.c - buffer objects and user VMs (pw_bo.h): BO_CREATE/MMAP_OFFSET/SET_LABEL/SYNC/
 * QUERY_INFO, GEM_CLOSE, VM_CREATE/DESTROY/BIND/GET_STATE, kernel BOs and kernel mappings.
 *
 * Derived from Linux panthor (dual GPL-2.0/MIT, used under MIT): panthor_gem.c, panthor_drv.c
 * and the VM side of panthor_mmu.c (Copyright 2019 Linaro Ltd, 2023 Collabora ltd.).
 * drm_gpuvm's split/merge machinery is replaced by a sorted interval list per VM ("VMAs"),
 * each holding a BO reference for the range it maps. File/line citations are Linux v7.3.
 *
 * Locking:
 *   uvm->lock          serialises every page-table + VMA update of one VM (outer)
 *     vm->op_lock      (pw_mmu.c) taken inside by pw_vm_map_mem/pw_vm_unmap
 *   bo_file->lock      mmap-offset list and labels of one file (outer)
 *     bos table lock   (pw_handle.c) taken inside to check a handle is still live
 * The MMU fault handler only bumps vm->fault_count (atomic), so it never nests with these.
 *
 * Memory types: RK3588 is not IO-coherent. User BOs are allocated PW_MEM_UNCACHED (CPU
 * mappings write-combined, the M4 default) unless DRM_PANTHOR_BO_WB_MMAP asks for a cached CPU
 * view; the kernel never maps user BOs (no PW_MEM_KMAP). The GPU-side cacheability is chosen
 * per mapping by VM_BIND (DRM_PANTHOR_VM_BIND_OP_MAP_UNCACHED), as in panthor.
 */

#include "pw_bo.h"
#include "pw_sync.h"

/* kmd/win's pw_os_mem_alloc() limit; larger requests fail with ENOMEM like a failed shmem
 * allocation would on Linux. */
#define PW_BO_MAX_SIZE			(1ull << 40)
/* Fake mmap offsets live in [4 GiB, 1 << 43): above drm_vma_manager's 64-bit
 * DRM_FILE_PAGE_OFFSET_START and below both user-MMIO offsets (panthor_drm.h:68-69), so they
 * can never be mistaken for the LATEST_FLUSH_ID page. */
#define PW_MMAP_OFFSET_START		(1ull << 32)
#define PW_MMAP_OFFSET_END		DRM_PANTHOR_USER_MMIO_OFFSET_32BIT
#define PW_MAX_VMS_PER_FILE		32		/* mmu.c:1576 */
#define PW_VM_MIN_KERNEL_VA_SIZE	(256ull << 20)	/* mmu.c:1506 */
/* TASK_SIZE_OF(current) equivalent: user-mode VA size of an ARM64 Windows process (128 TiB). */
#define PW_USER_TASK_SIZE		(1ull << 47)
#define PW_BO_LABEL_MAXLEN		4096		/* panthor_gem.h:17 */

#define PW_BIND_TYPE(flags)		((flags) & (uint32_t)DRM_PANTHOR_VM_BIND_OP_TYPE_MASK)
#define PW_BIND_TYPE_MAP		((uint32_t)DRM_PANTHOR_VM_BIND_OP_TYPE_MAP)
#define PW_BIND_TYPE_UNMAP		((uint32_t)DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP)
#define PW_BIND_TYPE_SYNC_ONLY		((uint32_t)DRM_PANTHOR_VM_BIND_OP_TYPE_SYNC_ONLY)
#define PW_BIND_TYPE_MASK_U32		((uint32_t)DRM_PANTHOR_VM_BIND_OP_TYPE_MASK)
/* mmu.c:1311 minus MAP_SPARSE: sparse binding is a 1.9 feature we don't implement. */
#define PW_BIND_MAP_FLAGS		((uint32_t)DRM_PANTHOR_VM_BIND_OP_MAP_READONLY | \
					 (uint32_t)DRM_PANTHOR_VM_BIND_OP_MAP_NOEXEC | \
					 (uint32_t)DRM_PANTHOR_VM_BIND_OP_MAP_UNCACHED | \
					 (uint32_t)DRM_PANTHOR_VM_BIND_OP_TYPE_MASK)
#define PW_BIND_MAP_ATTRS		(PW_VM_MAP_READONLY | PW_VM_MAP_NOEXEC | PW_VM_MAP_UNCACHED)
/* drv.c:313 */
#define PW_SYNC_OP_FLAGS_MASK		((uint32_t)DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_MASK | \
					 (uint32_t)DRM_PANTHOR_SYNC_OP_SIGNAL)

_Static_assert(PW_VM_MAP_READONLY == DRM_PANTHOR_VM_BIND_OP_MAP_READONLY &&
	       PW_VM_MAP_NOEXEC == DRM_PANTHOR_VM_BIND_OP_MAP_NOEXEC &&
	       PW_VM_MAP_UNCACHED == DRM_PANTHOR_VM_BIND_OP_MAP_UNCACHED, "map flag values");

/* ------------------------------------------------------------------------------------------ */
/* objects                                                                                    */
/* ------------------------------------------------------------------------------------------ */

struct pw_bo_priv {
	struct pw_bo base;		/* first: pw_bo * <-> pw_bo_priv * */
	struct pw_bo_priv *mm_prev, *mm_next;	/* file mmap-offset list, under bo_file->lock */
	bool mm_linked;
	bool cpu_cached;
	uint64_t excl_serial;		/* serial of the exclusive VM, 0 = none */
};

struct pw_bo_file {
	struct pw_os_mutex *lock;
	struct pw_bo_priv *mm_head;	/* sorted by mmap_offset */
};

struct pw_vma {
	struct pw_vma *prev, *next;
	uint64_t va, size;
	struct pw_bo *bo;		/* reference held for the mapped range */
	uint64_t bo_offset;
	uint32_t flags;			/* PW_VM_MAP_* */
	bool kernel;			/* pw_uvm_map_kernel_bo() mapping */
};

struct pw_uvm {
	uint32_t refcount;		/* __atomic */
	struct pw_device *dev;
	struct pw_vm *vm;
	struct pw_os_mutex *lock;
	uint64_t user_va_range;		/* == kernel VA start (mmu.c:2900) */
	uint64_t va_end;		/* 1 << va_bits */
	struct pw_vma *vmas;		/* sorted by va, non-overlapping */
	uint64_t serial;
	uint32_t unusable;		/* __atomic, sticky (panthor_vm_declare_unusable) */
	uint32_t destroyed;		/* __atomic: VM_DESTROY'ed, no new maps */
};

static uint64_t g_uvm_serial;		/* __atomic; identity of exclusive VMs (never reused) */

static inline struct pw_bo_priv *to_priv(struct pw_bo *bo)
{
	return (struct pw_bo_priv *)bo;
}

static inline bool range_overflows(uint64_t start, uint64_t size, uint64_t max)
{
	return start >= max || size > max - start;
}

/* ------------------------------------------------------------------------------------------ */
/* BO lifetime                                                                                */
/* ------------------------------------------------------------------------------------------ */

static int bo_alloc_common(struct pw_device *dev, uint64_t size, uint32_t mem_flags,
			   bool cpu_cached, bool backed, struct pw_bo **out)
{
	struct pw_bo_priv *p;
	int ret;

	if (!size || size > PW_BO_MAX_SIZE)
		return size ? PW_ENOMEM : PW_EINVAL;
	size = PW_ALIGN_UP(size, PW_SZ_4K);
	p = pw_os_zalloc(dev->os, sizeof(*p));
	if (!p)
		return PW_ENOMEM;
	if (backed) {
		ret = pw_os_mem_alloc(dev->os, (size_t)size, mem_flags, &p->base.mem);
		if (ret) {
			pw_os_free(dev->os, p);
			return ret == PW_EINVAL ? PW_ENOMEM : ret;
		}
	}
	p->base.refcount = 1;
	p->base.dev = dev;
	p->base.size = size;
	p->cpu_cached = cpu_cached;
	*out = &p->base;
	return PW_OK;
}

static int bo_alloc(struct pw_device *dev, uint64_t size, uint32_t mem_flags, bool cpu_cached,
		    struct pw_bo **out)
{
	return bo_alloc_common(dev, size, mem_flags, cpu_cached, true, out);
}

/* (jm-arch) pw_bo.h */
int pw_bo_alloc(struct pw_device *dev, uint64_t size, uint32_t mem_flags, bool cpu_cached,
		bool backed, struct pw_bo **out)
{
	*out = NULL;
	return bo_alloc_common(dev, size, mem_flags, cpu_cached, backed, out);
}

void pw_bo_get(struct pw_bo *bo)
{
	__atomic_add_fetch(&bo->refcount, 1, __ATOMIC_RELAXED);
}

void pw_bo_put(struct pw_bo *bo)
{
	struct pw_os *os;

	if (!bo || __atomic_sub_fetch(&bo->refcount, 1, __ATOMIC_ACQ_REL))
		return;
	os = bo->dev->os;
	/* (jm-arch) backend state first: it may still reference the pages (GPU mappings). */
	if (bo->release_ext)
		bo->release_ext(bo);
	if (bo->mem)
		pw_os_mem_free(os, bo->mem);
	pw_os_free(os, to_priv(bo));
}

struct pw_os_mem *pw_bo_os_mem(struct pw_bo *bo)
{
	return bo->mem;
}

bool pw_bo_cpu_cached(struct pw_bo *bo)
{
	return to_priv(bo)->cpu_cached;
}

static void bo_get_cb(void *obj)
{
	pw_bo_get(obj);
}

int pw_bo_create_kernel(struct pw_device *dev, uint64_t size, uint32_t mem_flags,
			struct pw_bo **out)
{
	struct pw_bo *bo;
	int ret;

	*out = NULL;
	ret = bo_alloc(dev, size, mem_flags, !(mem_flags & PW_MEM_UNCACHED), &bo);
	if (ret)
		return ret;
	bo->flags = DRM_PANTHOR_BO_NO_MMAP;	/* never user-mappable */
	*out = bo;
	return PW_OK;
}

struct pw_bo *pw_file_bo_get(struct pw_file *file, uint32_t handle)
{
	return pw_handle_get(&file->bos, handle, bo_get_cb);
}

/* ------------------------------------------------------------------------------------------ */
/* mmap offsets                                                                               */
/* ------------------------------------------------------------------------------------------ */

static void mm_unlink_locked(struct pw_bo_file *bf, struct pw_bo_priv *p)
{
	if (!p->mm_linked)
		return;
	if (p->mm_prev)
		p->mm_prev->mm_next = p->mm_next;
	else
		bf->mm_head = p->mm_next;
	if (p->mm_next)
		p->mm_next->mm_prev = p->mm_prev;
	p->mm_prev = p->mm_next = NULL;
	p->mm_linked = false;
}

/* First fit in [PW_MMAP_OFFSET_START, PW_MMAP_OFFSET_END). */
static int mm_assign_locked(struct pw_bo_file *bf, struct pw_bo_priv *p)
{
	uint64_t size = p->base.size, cand = PW_MMAP_OFFSET_START;
	struct pw_bo_priv *prev = NULL, *it;

	for (it = bf->mm_head; it; prev = it, it = it->mm_next) {
		if (it->base.mmap_offset - cand >= size)
			break;
		cand = it->base.mmap_offset + it->base.size;
	}
	if (cand > PW_MMAP_OFFSET_END || PW_MMAP_OFFSET_END - cand < size)
		return PW_ENOSPC;
	p->base.mmap_offset = cand;
	p->mm_prev = prev;
	p->mm_next = it;
	if (prev)
		prev->mm_next = p;
	else
		bf->mm_head = p;
	if (it)
		it->mm_prev = p;
	p->mm_linked = true;
	return PW_OK;
}

struct pw_bo *pw_file_bo_from_mmap_offset(struct pw_file *file, uint64_t offset, uint64_t *rel)
{
	struct pw_bo_file *bf = file->bo_file;
	struct pw_bo_priv *it;
	struct pw_bo *bo = NULL;

	pw_os_mutex_lock(bf->lock);
	for (it = bf->mm_head; it && it->base.mmap_offset <= offset; it = it->mm_next) {
		if (offset - it->base.mmap_offset < it->base.size) {
			bo = &it->base;
			pw_bo_get(bo);
			*rel = offset - it->base.mmap_offset;
			break;
		}
	}
	pw_os_mutex_unlock(bf->lock);
	return bo;
}

/* Drop a GEM handle's reference. Like drm_gem_object_release_handle(), this also revokes the
 * file's mmap offset (later lookups fail) while existing user mappings keep the BO alive. */
static void bo_handle_release(struct pw_file *file, struct pw_bo *bo)
{
	pw_os_mutex_lock(file->bo_file->lock);
	mm_unlink_locked(file->bo_file, to_priv(bo));
	pw_os_mutex_unlock(file->bo_file->lock);
	pw_bo_put(bo);
}

/* ------------------------------------------------------------------------------------------ */
/* BO ioctls                                                                                  */
/* ------------------------------------------------------------------------------------------ */

static void uvm_get_cb(void *obj)
{
	pw_uvm_get(obj);
}

/* drv.c:1032 panthor_ioctl_bo_create + gem.c:1003 panthor_gem_create */
int pw_bo_create_user(struct pw_file *file, struct drm_panthor_bo_create *args,
		      uint32_t allowed_flags)
{
	struct pw_device *dev = file->dev;
	struct pw_uvm *uvm = NULL;
	struct pw_bo *bo;
	uint32_t handle, mem_flags;
	bool cpu_cached;
	int ret;

	if (!args->size || args->pad || (args->flags & ~allowed_flags))
		return PW_EINVAL;
	if ((args->flags & DRM_PANTHOR_BO_NO_MMAP) && (args->flags & DRM_PANTHOR_BO_WB_MMAP))
		return PW_EINVAL;
	if (args->exclusive_vm_id) {
		uvm = pw_file_vm_get(file, args->exclusive_vm_id);
		if (!uvm)
			return PW_EINVAL;
	}

	/* gem.c:89 should_map_wc(): cached CPU mappings only if asked for (or IO-coherent). */
	cpu_cached = dev->coherent || (args->flags & DRM_PANTHOR_BO_WB_MMAP);
	mem_flags = PW_MEM_ZEROED | (cpu_cached ? 0 : PW_MEM_UNCACHED);
	ret = bo_alloc(dev, args->size, mem_flags, cpu_cached, &bo);
	if (ret)
		goto out;
	bo->flags = args->flags;
	if (uvm) {
		bo->exclusive_vm = uvm;		/* weak: identity via excl_serial */
		to_priv(bo)->excl_serial = uvm->serial;
	}

	ret = pw_handle_alloc(file->os, &file->bos, bo, &handle);
	if (ret) {
		pw_bo_put(bo);
		goto out;
	}
	args->size = bo->size;
	args->handle = handle;
out:
	pw_uvm_put(uvm);
	return ret;
}

int pw_ioctl_bo_create(struct pw_file *file, void *arg)
{
	uint32_t major, minor, patch, allowed = DRM_PANTHOR_BO_NO_MMAP;

	pw_core_drm_version(&major, &minor, &patch);
	if (major > 1 || minor >= 7)	/* drv.c:1785: WB_MMAP is a 1.7 feature */
		allowed |= DRM_PANTHOR_BO_WB_MMAP;
	return pw_bo_create_user(file, arg, allowed);
}

/* drm_gem_close_ioctl(): unknown handle -> EINVAL. */
int pw_ioctl_gem_close(struct pw_file *file, void *arg)
{
	struct drm_gem_close *args = arg;
	struct pw_bo *bo = pw_handle_remove(&file->bos, args->handle);

	if (!bo)
		return PW_EINVAL;
	bo_handle_release(file, bo);
	return PW_OK;
}

/* (jm-arch) pw_bo.h; the body of drv.c:1077 panthor_ioctl_bo_mmap_offset after its pad check. */
int pw_bo_get_mmap_offset(struct pw_file *file, uint32_t handle, uint64_t *offset)
{
	struct pw_bo_file *bf = file->bo_file;
	struct pw_bo *bo;
	int ret = PW_OK;

	bo = pw_file_bo_get(file, handle);
	if (!bo)
		return PW_ENOENT;
	if (bo->flags & DRM_PANTHOR_BO_NO_MMAP) {
		ret = PW_EPERM;
		goto out;
	}
	pw_os_mutex_lock(bf->lock);
	/* A racing GEM_CLOSE removes the handle before unlinking under bf->lock: re-check under
	 * the lock so a closed BO never (re)enters the list. */
	if (pw_handle_get(&file->bos, handle, NULL) != bo)
		ret = PW_ENOENT;
	else if (!to_priv(bo)->mm_linked)
		ret = mm_assign_locked(bf, to_priv(bo));
	if (!ret)
		*offset = bo->mmap_offset;
	pw_os_mutex_unlock(bf->lock);
out:
	pw_bo_put(bo);
	return ret;
}

/* drv.c:1077 */
int pw_ioctl_bo_mmap_offset(struct pw_file *file, void *arg)
{
	struct drm_panthor_bo_mmap_offset *args = arg;
	uint64_t offset;
	int ret;

	if (args->pad)
		return PW_EINVAL;
	ret = pw_bo_get_mmap_offset(file, args->handle, &offset);
	if (!ret)
		args->offset = offset;
	return ret;
}

/* drv.c:1471 panthor_ioctl_bo_set_label: strndup_user(label, 4096); EINVAL from it -> E2BIG.
 * We keep the first sizeof(bo->label) - 1 bytes. */
int pw_ioctl_bo_set_label(struct pw_file *file, void *arg)
{
	struct drm_panthor_bo_set_label *args = arg;
	struct pw_bo *bo;
	int ret;

	if (args->pad)
		return PW_EINVAL;
	bo = pw_file_bo_get(file, args->handle);
	if (!bo)
		return PW_ENOENT;
	ret = pw_bo_set_label_from_user(file, bo, args->label);
	pw_bo_put(bo);
	return ret;
}

/* (jm-arch) pw_bo.h; the label part of pw_ioctl_bo_set_label() above. */
int pw_bo_set_label_from_user(struct pw_file *file, struct pw_bo *bo, uint64_t user_label)
{
	char label[sizeof(((struct pw_bo *)0)->label)];
	size_t len = 0;
	bool found_nul = false;
	int ret = PW_OK;

	label[0] = '\0';
	if (user_label) {
		/* Byte by byte, like strnlen_user(): never read past the terminator (the next byte
		 * may be on an unmapped page). Labels are short and this ioctl is rare. */
		for (len = 0; len < PW_BO_LABEL_MAXLEN; len++) {
			char c;

			ret = pw_os_copy_from_user(file->os, &c, user_label + len, 1);
			if (ret)
				return ret;
			if (len < sizeof(label))
				label[len] = c;
			if (!c) {
				found_nul = true;
				break;
			}
		}
		if (!found_nul)
			return PW_E2BIG;
		label[PW_MIN(len, sizeof(label) - 1)] = '\0';
	}

	pw_os_mutex_lock(file->bo_file->lock);
	pw_memcpy(bo->label, label, sizeof(label));
	pw_os_mutex_unlock(file->bo_file->lock);
	return ret;
}

/* drv.c:1531 + gem.c:1145 panthor_gem_sync */
int pw_ioctl_bo_sync(struct pw_file *file, void *arg)
{
	struct drm_panthor_bo_sync *args = arg;
	struct drm_panthor_bo_sync_op *ops;
	int ret;

	if (!args->ops.count)
		return PW_OK;
	ret = pw_copy_obj_array(file, &args->ops, sizeof(*ops), sizeof(*ops), (void **)&ops);
	if (ret)
		return ret;

	for (uint32_t i = 0; i < args->ops.count && !ret; i++) {
		const struct drm_panthor_bo_sync_op *op = &ops[i];
		struct pw_bo *bo = pw_file_bo_get(file, op->handle);

		if (!bo) {
			ret = PW_ENOENT;
			break;
		}
		if (op->offset + op->size < op->offset || op->offset + op->size > bo->size)
			ret = PW_EINVAL;
		else if (op->type != DRM_PANTHOR_BO_SYNC_CPU_CACHE_FLUSH &&
			 op->type != DRM_PANTHOR_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE)
			ret = PW_EINVAL;
		else if (!pw_bo_cpu_cached(bo) || !op->size)
			ret = PW_OK;	/* WC mapping or empty range: nothing to do */
		else if (op->type == DRM_PANTHOR_BO_SYNC_CPU_CACHE_FLUSH)
			pw_os_mem_flush(file->os, bo->mem, (size_t)op->offset, (size_t)op->size);
		else
			pw_os_mem_invalidate(file->os, bo->mem, (size_t)op->offset,
					     (size_t)op->size);
		pw_bo_put(bo);
	}
	pw_os_free(file->os, ops);
	return ret;
}

/* drv.c:1566 */
int pw_ioctl_bo_query_info(struct pw_file *file, void *arg)
{
	struct drm_panthor_bo_query_info *args = arg;
	struct pw_bo *bo = pw_file_bo_get(file, args->handle);

	if (!bo)
		return PW_ENOENT;
	args->pad = 0;
	args->create_flags = bo->flags;
	args->extra_flags = 0;		/* no PRIME import on Windows: never IS_IMPORTED */
	pw_bo_put(bo);
	return PW_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* user VMs: VMA list                                                                         */
/* ------------------------------------------------------------------------------------------ */

static struct pw_vma *vma_alloc(struct pw_os *os)
{
	return pw_os_zalloc(os, sizeof(struct pw_vma));
}

static void vma_free(struct pw_os *os, struct pw_vma *v)
{
	if (!v)
		return;
	pw_bo_put(v->bo);
	pw_os_free(os, v);
}

static void vma_unlink(struct pw_uvm *uvm, struct pw_vma *v)
{
	if (v->prev)
		v->prev->next = v->next;
	else
		uvm->vmas = v->next;
	if (v->next)
		v->next->prev = v->prev;
	v->prev = v->next = NULL;
}

static void vma_insert(struct pw_uvm *uvm, struct pw_vma *v)
{
	struct pw_vma *prev = NULL, *it = uvm->vmas;

	while (it && it->va < v->va) {
		prev = it;
		it = it->next;
	}
	v->prev = prev;
	v->next = it;
	if (prev)
		prev->next = v;
	else
		uvm->vmas = v;
	if (it)
		it->prev = v;
}

static bool vmas_overlap(struct pw_uvm *uvm, uint64_t start, uint64_t end)
{
	for (struct pw_vma *v = uvm->vmas; v && v->va < end; v = v->next) {
		if (v->va + v->size > start)
			return true;
	}
	return false;
}

/*
 * Drop the bookkeeping for [start, end) after the page tables were updated: whole VMAs go,
 * VMAs straddling an edge are trimmed, and a VMA strictly containing the range is split, which
 * needs one preallocated node (*spare, consumed). This is what drm_gpuvm_sm_unmap()'s
 * unmap/remap steps do for panthor (mmu.c:2348-2463).
 */
static void vmas_remove_range(struct pw_uvm *uvm, uint64_t start, uint64_t end,
			      struct pw_vma **spare)
{
	struct pw_os *os = uvm->dev->os;
	struct pw_vma *v = uvm->vmas, *next;

	for (; v && v->va < end; v = next) {
		uint64_t vend = v->va + v->size;

		next = v->next;
		if (vend <= start)
			continue;
		if (v->va >= start && vend <= end) {
			vma_unlink(uvm, v);
			vma_free(os, v);
		} else if (v->va < start && vend > end) {
			struct pw_vma *r = spare ? *spare : NULL;

			if (!r) {
				/* Callers preallocate; keep the whole VMA (and its BO ref) rather
				 * than lose track of a still-mapped part. */
				pw_err(uvm->dev, "VMA split without a spare node");
				break;
			}
			*spare = NULL;
			r->va = end;
			r->size = vend - end;
			r->bo = v->bo;
			pw_bo_get(r->bo);
			r->bo_offset = v->bo_offset + (end - v->va);
			r->flags = v->flags;
			r->kernel = v->kernel;
			v->size = start - v->va;
			vma_insert(uvm, r);
			break;
		} else if (v->va < start) {
			v->size = start - v->va;
		} else {
			v->bo_offset += end - v->va;
			v->size = vend - end;
			v->va = end;
		}
	}
}

static void uvm_declare_unusable(struct pw_uvm *uvm)
{
	/* mmu.c:907: panthor also disables the AS; here the VM simply can't be (re)activated
	 * any more (pw_uvm_activate) and maps are refused. */
	if (!__atomic_exchange_n(&uvm->unusable, 1, __ATOMIC_ACQ_REL))
		pw_warn(uvm->dev, "user VM declared unusable");
}

/* Unmap [va, va + size) (holes allowed). On a page-table failure the bookkeeping is kept, so
 * the BOs stay referenced while their pages may still be mapped, and the VM is unusable. */
static int uvm_unmap_locked(struct pw_uvm *uvm, uint64_t va, uint64_t size,
			    struct pw_vma **spare)
{
	int ret;

	if (!size || !vmas_overlap(uvm, va, va + size))
		return PW_OK;
	ret = pw_vm_unmap(uvm->vm, va, size);
	if (ret) {
		pw_err(uvm->dev, "VM unmap 0x%llx+0x%llx failed (%d)", (unsigned long long)va,
		       (unsigned long long)size, ret);
		uvm_declare_unusable(uvm);
		return ret;
	}
	vmas_remove_range(uvm, va, va + size, spare);
	return PW_OK;
}

/* MAP replaces whatever was mapped in the range (drm_gpuvm_sm_map semantics, mmu.c:2741):
 * overlapping mappings are unmapped/split first, then the new one is mapped. spare[0] becomes
 * the new VMA, spare[1] is used for a split. */
static int uvm_map_locked(struct pw_uvm *uvm, uint64_t va, uint64_t size, struct pw_bo *bo,
			  uint64_t bo_offset, uint32_t flags, bool kernel, struct pw_vma **spare)
{
	struct pw_vma *v;
	int ret;

	if (!size)
		return PW_OK;
	ret = uvm_unmap_locked(uvm, va, size, &spare[1]);
	if (ret)
		return ret;
	ret = pw_vm_map_mem(uvm->vm, va, bo->mem, (size_t)bo_offset, size, flags);
	if (ret) {
		/* mmu.c:1035: map_pages failure -> VM unusable (old mappings may be gone). */
		pw_err(uvm->dev, "VM map 0x%llx+0x%llx failed (%d)", (unsigned long long)va,
		       (unsigned long long)size, ret);
		uvm_declare_unusable(uvm);
		/* pw_vm_map_mem() rolls back its PTEs on a mapping error, but a failed final
		 * flush/UNLOCK (vm_unlock_region) is reported after the PTEs were written. No
		 * VMA (hence no BO reference) is recorded for the range, so make sure no PTE
		 * survives that could reach the BO's pages once it is freed. Holes are skipped. */
		(void)pw_vm_unmap(uvm->vm, va, size);
		return ret;
	}
	v = spare[0];
	spare[0] = NULL;
	v->va = va;
	v->size = size;
	v->bo = bo;
	pw_bo_get(bo);
	v->bo_offset = bo_offset;
	v->flags = flags;
	v->kernel = kernel;
	vma_insert(uvm, v);
	return PW_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* user VMs: lifetime                                                                         */
/* ------------------------------------------------------------------------------------------ */

void pw_uvm_get(struct pw_uvm *uvm)
{
	__atomic_add_fetch(&uvm->refcount, 1, __ATOMIC_RELAXED);
}

void pw_uvm_put(struct pw_uvm *uvm)
{
	struct pw_os *os;

	if (!uvm || __atomic_sub_fetch(&uvm->refcount, 1, __ATOMIC_ACQ_REL))
		return;
	os = uvm->dev->os;
	/* Deactivates the AS (cache flush) and frees the page tables first, so no BO page is
	 * released while the GPU could still reach it. */
	pw_vm_destroy(uvm->vm);
	while (uvm->vmas) {
		struct pw_vma *v = uvm->vmas;

		vma_unlink(uvm, v);
		vma_free(os, v);
	}
	pw_os_mutex_destroy(os, uvm->lock);
	pw_os_free(os, uvm);
}

struct pw_vm *pw_uvm_vm(struct pw_uvm *uvm)
{
	return uvm->vm;
}

void pw_uvm_kernel_va_range(struct pw_uvm *uvm, uint64_t *start, uint64_t *end)
{
	*start = uvm->user_va_range;
	*end = uvm->va_end;
}

struct pw_uvm *pw_file_vm_get(struct pw_file *file, uint32_t vm_id)
{
	return pw_handle_get(&file->vms, vm_id, uvm_get_cb);
}

bool pw_uvm_is_unusable(struct pw_uvm *uvm)
{
	return __atomic_load_n(&uvm->unusable, __ATOMIC_ACQUIRE) ||
	       __atomic_load_n(&uvm->vm->fault_count, __ATOMIC_ACQUIRE);
}

int pw_uvm_activate(struct pw_uvm *uvm)
{
	if (pw_uvm_is_unusable(uvm))
		return PW_EINVAL;
	return pw_vm_activate(uvm->vm);
}

/* mmu.c:1508 panthor_vm_create_get_user_va_range */
static uint64_t vm_user_va_range(const struct drm_panthor_vm_create *args, uint64_t full)
{
	uint64_t user;

	if (full < PW_VM_MIN_KERNEL_VA_SIZE)
		return 0;
	if (args->user_va_range)
		user = args->user_va_range;
	else if (PW_USER_TASK_SIZE < full)
		user = PW_USER_TASK_SIZE;
	else
		user = full > PW_SZ_4G ? full / 2 : full - PW_VM_MIN_KERNEL_VA_SIZE;
	if (full - PW_VM_MIN_KERNEL_VA_SIZE < user)
		user = full - PW_VM_MIN_KERNEL_VA_SIZE;
	return user;
}

static uint64_t rounddown_pow2(uint64_t x)
{
	return x ? 1ull << (pw_fls64(x) - 1) : 0;
}

/* Undo VM_CREATE: unmap everything (user and kernel ranges, mmu.c:1633), refuse new maps. */
static void uvm_destroy(struct pw_uvm *uvm)
{
	pw_os_mutex_lock(uvm->lock);
	__atomic_store_n(&uvm->destroyed, 1, __ATOMIC_RELEASE);
	(void)uvm_unmap_locked(uvm, 0, uvm->va_end, NULL);
	pw_os_mutex_unlock(uvm->lock);
}

static void vm_handle_release(void *obj)
{
	uvm_destroy(obj);
	pw_uvm_put(obj);
}

/* drv.c:1001 + mmu.c:1550-1617 */
int pw_ioctl_vm_create(struct pw_file *file, void *arg)
{
	struct drm_panthor_vm_create *args = arg;
	struct pw_device *dev = file->dev;
	uint64_t full, user, kernel_range;
	struct pw_uvm *uvm;
	uint32_t id;
	int ret;

	if (args->flags)
		return PW_EINVAL;
	if (!dev->mmu.ready || dev->mmu.va_bits < 32 || dev->mmu.va_bits > 48)
		return PW_EINVAL;
	full = 1ull << dev->mmu.va_bits;
	user = vm_user_va_range(args, full);
	if (!user || (args->user_va_range && args->user_va_range > user))
		return PW_EINVAL;
	/* A power-of-two kernel range, for a clear split (mmu.c:1567). */
	kernel_range = rounddown_pow2(full - user);

	uvm = pw_os_zalloc(file->os, sizeof(*uvm));
	if (!uvm)
		return PW_ENOMEM;
	uvm->refcount = 1;
	uvm->dev = dev;
	uvm->user_va_range = full - kernel_range;
	uvm->va_end = full;
	uvm->serial = __atomic_add_fetch(&g_uvm_serial, 1, __ATOMIC_RELAXED);
	uvm->lock = pw_os_mutex_create(file->os);
	if (!uvm->lock) {
		pw_os_free(file->os, uvm);
		return PW_ENOMEM;
	}
	ret = pw_vm_create(dev, false, &uvm->vm);
	if (ret) {
		pw_os_mutex_destroy(file->os, uvm->lock);
		pw_os_free(file->os, uvm);
		return ret;
	}

	ret = pw_handle_alloc(file->os, &file->vms, uvm, &id);
	if (!ret && id > PW_MAX_VMS_PER_FILE) {
		/* xa_alloc(XA_LIMIT(1, 32)) fails with -EBUSY when full. */
		pw_handle_remove(&file->vms, id);
		ret = PW_EBUSY;
	}
	if (ret) {
		pw_uvm_put(uvm);
		return ret;
	}
	args->id = id;
	args->user_va_range = uvm->user_va_range;
	return PW_OK;
}

/* drv.c:1022 */
int pw_ioctl_vm_destroy(struct pw_file *file, void *arg)
{
	struct drm_panthor_vm_destroy *args = arg;
	struct pw_uvm *uvm;

	if (args->pad)
		return PW_EINVAL;
	uvm = pw_handle_remove(&file->vms, args->id);
	if (!uvm)
		return PW_EINVAL;
	vm_handle_release(uvm);
	return PW_OK;
}

/* drv.c:1453 */
int pw_ioctl_vm_get_state(struct pw_file *file, void *arg)
{
	struct drm_panthor_vm_get_state *args = arg;
	struct pw_uvm *uvm = pw_file_vm_get(file, args->vm_id);

	if (!uvm)
		return PW_EINVAL;
	args->state = pw_uvm_is_unusable(uvm) ? DRM_PANTHOR_VM_STATE_UNUSABLE :
						 DRM_PANTHOR_VM_STATE_USABLE;
	pw_uvm_put(uvm);
	return PW_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* kernel mappings                                                                            */
/* ------------------------------------------------------------------------------------------ */

/* First fit in the kernel range, 2 MiB aligned for >= 2 MiB objects (mmu.c:1132). */
static int uvm_alloc_kernel_va(struct pw_uvm *uvm, uint64_t size, uint64_t *va)
{
	uint64_t align = size >= PW_SZ_2M ? PW_SZ_2M : PW_SZ_4K;
	uint64_t cand = PW_ALIGN_UP(uvm->user_va_range, align);

	for (struct pw_vma *v = uvm->vmas; v; v = v->next) {
		if (v->va + v->size <= cand)
			continue;
		if (v->va >= cand && v->va - cand >= size)
			break;
		cand = PW_ALIGN_UP(v->va + v->size, align);
	}
	if (cand >= uvm->va_end || uvm->va_end - cand < size)
		return PW_ENOSPC;
	*va = cand;
	return PW_OK;
}

int pw_uvm_map_kernel_bo(struct pw_uvm *uvm, struct pw_bo *bo, uint32_t map_flags,
			 uint64_t *va_out)
{
	struct pw_os *os = uvm->dev->os;
	struct pw_vma *spare[2] = { NULL, NULL };
	uint64_t va = 0;
	int ret;

	*va_out = 0;
	if (map_flags & ~(PW_BIND_MAP_ATTRS | PW_VM_MAP_DEVICE))
		return PW_EINVAL;
	spare[0] = vma_alloc(os);
	if (!spare[0])
		return PW_ENOMEM;

	pw_os_mutex_lock(uvm->lock);
	if (__atomic_load_n(&uvm->destroyed, __ATOMIC_ACQUIRE) || pw_uvm_is_unusable(uvm))
		ret = PW_EINVAL;
	else
		ret = uvm_alloc_kernel_va(uvm, bo->size, &va);
	if (!ret)
		ret = uvm_map_locked(uvm, va, bo->size, bo, 0, map_flags, true, spare);
	pw_os_mutex_unlock(uvm->lock);

	vma_free(os, spare[0]);
	if (!ret)
		*va_out = va;
	return ret;
}

void pw_uvm_unmap_kernel_bo(struct pw_uvm *uvm, uint64_t va)
{
	struct pw_vma *v;

	pw_os_mutex_lock(uvm->lock);
	for (v = uvm->vmas; v && v->va <= va; v = v->next) {
		if (v->va == va && v->kernel) {
			/* An exact VMA never needs a split node. */
			(void)uvm_unmap_locked(uvm, v->va, v->size, NULL);
			break;
		}
	}
	pw_os_mutex_unlock(uvm->lock);
}

/* ------------------------------------------------------------------------------------------ */
/* VM_BIND                                                                                    */
/* ------------------------------------------------------------------------------------------ */

struct bind_op {
	struct drm_panthor_vm_bind_op uop;
	struct pw_bo *bo;			/* MAP: reference */
	struct pw_vma *spare[2];
	/* ASYNC only */
	struct drm_panthor_sync_op *syncs;	/* uop.syncs.count entries */
	struct pw_fence **waits;		/* per sync op: fence to wait for, or NULL */
	struct pw_fence *done;			/* signalled after the op, if it has signal ops */
};

static void bind_op_cleanup(struct pw_os *os, struct bind_op *b)
{
	if (b->waits) {
		for (uint32_t i = 0; i < b->uop.syncs.count; i++)
			pw_fence_put(b->waits[i]);
		pw_os_free(os, b->waits);
	}
	pw_fence_put(b->done);
	pw_os_free(os, b->syncs);
	pw_bo_put(b->bo);
	vma_free(os, b->spare[0]);
	vma_free(os, b->spare[1]);
	pw_memset(b, 0, sizeof(*b));
}

/* mmu.c:2977 panthor_vm_bind_prepare_op_ctx + 1318/1413/1462 */
static int bind_op_prepare(struct pw_file *file, struct pw_uvm *uvm, struct bind_op *b)
{
	const struct drm_panthor_vm_bind_op *op = &b->uop;
	struct pw_os *os = file->os;

	if ((op->va | op->size | op->bo_offset) & (PW_SZ_4K - 1))
		return PW_EINVAL;
	/* mmu.c:2988: no user mapping may reach the kernel-reserved range. */
	if (range_overflows(op->va, op->size, uvm->user_va_range))
		return PW_EINVAL;

	switch (PW_BIND_TYPE(op->flags)) {
	case PW_BIND_TYPE_MAP:
		b->bo = pw_file_bo_get(file, op->bo_handle);
		if (!b->bo)
			return PW_EINVAL;
		if (op->flags & ~PW_BIND_MAP_FLAGS)
			return PW_EINVAL;
		if (op->size > b->bo->size || op->bo_offset > b->bo->size - op->size)
			return PW_EINVAL;
		/* A BO with an exclusive VM can't be mapped in other VMs (mmu.c:1350). */
		if (to_priv(b->bo)->excl_serial && to_priv(b->bo)->excl_serial != uvm->serial)
			return PW_EINVAL;
		b->spare[0] = vma_alloc(os);
		b->spare[1] = vma_alloc(os);
		return b->spare[0] && b->spare[1] ? PW_OK : PW_ENOMEM;

	case PW_BIND_TYPE_UNMAP:
		if (op->flags & ~PW_BIND_TYPE_MASK_U32)
			return PW_EINVAL;
		if (op->bo_handle || op->bo_offset)
			return PW_EINVAL;
		b->spare[1] = vma_alloc(os);
		return b->spare[1] ? PW_OK : PW_ENOMEM;

	case PW_BIND_TYPE_SYNC_ONLY:
		if (op->flags & ~PW_BIND_TYPE_MASK_U32)
			return PW_EINVAL;
		if (op->bo_handle || op->bo_offset || op->va || op->size)
			return PW_EINVAL;
		if (!op->syncs.count)
			return PW_EINVAL;
		return PW_OK;

	default:
		return PW_EINVAL;
	}
}

/* mmu.c:2713 panthor_vm_exec_op */
static int bind_op_exec(struct pw_uvm *uvm, struct bind_op *b, bool unusable_on_failure)
{
	const struct drm_panthor_vm_bind_op *op = &b->uop;
	int ret = PW_OK;

	if (PW_BIND_TYPE(op->flags) == PW_BIND_TYPE_SYNC_ONLY)
		return PW_OK;

	pw_os_mutex_lock(uvm->lock);
	if (PW_BIND_TYPE(op->flags) == PW_BIND_TYPE_MAP) {
		if (pw_uvm_is_unusable(uvm) || __atomic_load_n(&uvm->destroyed, __ATOMIC_ACQUIRE))
			ret = PW_EINVAL;
		else
			ret = uvm_map_locked(uvm, op->va, op->size, b->bo, op->bo_offset,
					     op->flags & PW_BIND_MAP_ATTRS, false, b->spare);
	} else {
		ret = uvm_unmap_locked(uvm, op->va, op->size, &b->spare[1]);
	}
	if (ret && unusable_on_failure)
		uvm_declare_unusable(uvm);
	pw_os_mutex_unlock(uvm->lock);
	return ret;
}

/* drv.c:1380 panthor_ioctl_vm_bind_sync + mmu.c:3160 */
static int vm_bind_sync(struct pw_file *file, struct pw_uvm *uvm, struct drm_panthor_vm_bind *args,
			struct drm_panthor_vm_bind_op *ops)
{
	int ret = PW_OK;
	uint32_t i;

	for (i = 0; i < args->ops.count; i++) {
		struct bind_op b;

		pw_memset(&b, 0, sizeof(b));
		b.uop = ops[i];
		/* No sync objects allowed on synchronous operations. */
		if (b.uop.syncs.count) {
			ret = PW_EINVAL;
			break;
		}
		if (!b.uop.size)
			continue;
		ret = bind_op_prepare(file, uvm, &b);
		if (!ret)
			ret = bind_op_exec(uvm, &b, false);
		bind_op_cleanup(file->os, &b);
		if (ret)
			break;
	}
	/* Update ops.count so the user knows where things failed (drv.c:1398). */
	if (ret)
		args->ops.count = i;
	return ret;
}

/* drv.c:313 panthor_check_sync_op */
static int check_sync_op(const struct drm_panthor_sync_op *s)
{
	uint32_t type = s->flags & (uint32_t)DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_MASK;

	if (s->flags & ~PW_SYNC_OP_FLAGS_MASK)
		return PW_EINVAL;
	if (type != DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ &&
	    type != DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ)
		return PW_EINVAL;
	if (type == DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ && s->timeline_value)
		return PW_EINVAL;
	return PW_OK;
}

static bool sync_op_is_signal(const struct drm_panthor_sync_op *s)
{
	return s->flags & (uint32_t)DRM_PANTHOR_SYNC_OP_SIGNAL;
}

/* One (handle, point) signalled by the call, deduplicated like panthor's ctx->signals list
 * (drv.c:375-421): resolved up front, published once after every op ran (drv.c:557-571). */
struct bind_signal {
	uint32_t handle;
	uint64_t point;
	uint32_t first_op;		/* index of the first op that signals it */
	struct pw_syncobj *so;		/* reference */
	struct pw_syncobj_point *pt;	/* point > 0: preallocated chain node (never fails later) */
	struct pw_fence *fence;		/* done fence of the last op that signals it */
};

static struct bind_signal *find_signal(struct bind_signal *sigs, uint32_t nsig, uint32_t handle,
				       uint64_t point)
{
	for (uint32_t i = 0; i < nsig; i++) {
		if (sigs[i].handle == handle && sigs[i].point == point)
			return &sigs[i];
	}
	return NULL;
}

/*
 * DRM_PANTHOR_VM_BIND_ASYNC. panthor queues each op as a drm_sched job on a per-VM queue
 * (drv.c:1336, mmu.c:2769); we run the queue synchronously inside the ioctl instead:
 *  1. validate every op, resolve every signal syncobj (EINVAL if missing, drv.c:396) with a
 *     preallocated timeline node, and every wait fence, up front; any failure: nothing done.
 *     A wait on a (handle, point) that an earlier op of the same call signals needs no fence:
 *     that op has completed before the next one runs (drv.c:622);
 *  2. per op, in order: wait for its fences, execute it (a failure makes the VM unusable and
 *     is reported through the op's fence, the ioctl still succeeds, mmu.c:2777), signal its
 *     fence;
 *  3. publish the last fence of every signalled (handle, point) (drv.c:557), which can't fail.
 * An interrupted wait (PW_EINTR: user APC or thread/file teardown on Windows) returns PW_EINTR
 * with nothing published and the VM left usable. libdrm-win restarts the ioctl on EINTR, and
 * replaying the whole batch is idempotent: every op sets a VA range to a fixed state (MAP
 * replaces, UNMAP clears), so re-running ops that already ran converges to the same result.
 * Deviation: the caller blocks until the waits are satisfied.
 */
static int vm_bind_async(struct pw_file *file, struct pw_uvm *uvm,
			 struct drm_panthor_vm_bind *args, struct drm_panthor_vm_bind_op *ops)
{
	struct pw_os *os = file->os;
	uint32_t n = args->ops.count, i, j, nsig = 0, max_sig = 0;
	struct bind_signal *sigs = NULL;
	struct bind_op *b;
	int ret = PW_OK;

	if (!n)
		return PW_OK;
	if (__atomic_load_n(&uvm->destroyed, __ATOMIC_ACQUIRE) || pw_uvm_is_unusable(uvm))
		return PW_EINVAL;	/* mmu.c:3067 */
	b = pw_os_zalloc(os, (size_t)n * sizeof(*b));
	if (!b)
		return PW_ENOMEM;

	/* 1a. per-op validation, sync arrays (panthor_vm_bind_job_create + add_job) */
	for (i = 0; i < n && !ret; i++) {
		b[i].uop = ops[i];
		ret = bind_op_prepare(file, uvm, &b[i]);
		if (ret)
			break;
		ret = pw_copy_obj_array(file, &b[i].uop.syncs, sizeof(struct drm_panthor_sync_op),
					sizeof(struct drm_panthor_sync_op), (void **)&b[i].syncs);
		if (ret || !b[i].uop.syncs.count)
			continue;
		for (j = 0; j < b[i].uop.syncs.count && !ret; j++) {
			ret = check_sync_op(&b[i].syncs[j]);
			if (!ret && sync_op_is_signal(&b[i].syncs[j]))
				max_sig++;
		}
		if (ret)
			break;
		b[i].waits = pw_os_zalloc(os, (size_t)b[i].uop.syncs.count * sizeof(*b[i].waits));
		if (!b[i].waits)
			ret = PW_ENOMEM;
	}
	if (!ret && max_sig) {
		sigs = pw_os_zalloc(os, (size_t)max_sig * sizeof(*sigs));
		if (!sigs)
			ret = PW_ENOMEM;
	}

	/* 1b. resolve waits (before this op's own signals) and signals, in op order */
	for (i = 0; i < n && !ret; i++) {
		bool has_signal = false;

		for (j = 0; j < b[i].uop.syncs.count && !ret; j++) {
			const struct drm_panthor_sync_op *s = &b[i].syncs[j];
			struct bind_signal *e;

			if (sync_op_is_signal(s))
				continue;
			e = find_signal(sigs, nsig, s->handle, s->timeline_value);
			if (e && e->first_op < i)
				continue;
			ret = pw_syncobj_find_fence(file, s->handle, s->timeline_value, 0, 0,
						    &b[i].waits[j]);
		}
		for (j = 0; j < b[i].uop.syncs.count && !ret; j++) {
			const struct drm_panthor_sync_op *s = &b[i].syncs[j];
			struct bind_signal *e;

			if (!sync_op_is_signal(s))
				continue;
			has_signal = true;
			if (find_signal(sigs, nsig, s->handle, s->timeline_value))
				continue;
			e = &sigs[nsig];
			e->handle = s->handle;
			e->point = s->timeline_value;
			e->first_op = i;
			e->so = pw_syncobj_lookup(file, s->handle);
			if (!e->so) {
				ret = PW_EINVAL;
				break;
			}
			nsig++;
			if (e->point) {
				e->pt = pw_syncobj_point_alloc(file->dev);
				if (!e->pt)
					ret = PW_ENOMEM;
			}
		}
		if (!ret && has_signal) {
			b[i].done = pw_fence_create(file->dev, 0);
			if (!b[i].done)
				ret = PW_ENOMEM;
		}
	}
	if (ret)
		goto out;

	/* 2. run the queue */
	for (i = 0; i < n; i++) {
		int op_ret = PW_OK;

		for (j = 0; j < b[i].uop.syncs.count && !op_ret; j++) {
			if (!b[i].waits[j])
				continue;
			op_ret = pw_fence_wait(b[i].waits[j], UINT64_MAX);
			if (op_ret == PW_EINTR) {
				ret = PW_EINTR;		/* restartable: nothing published */
				goto out;
			}
		}
		if (op_ret)
			uvm_declare_unusable(uvm);	/* e.g. ENOMEM: like a failed job */
		else
			op_ret = bind_op_exec(uvm, &b[i], true);
		if (!b[i].done)
			continue;
		pw_fence_signal(b[i].done, op_ret);
		for (j = 0; j < b[i].uop.syncs.count; j++) {
			const struct drm_panthor_sync_op *s = &b[i].syncs[j];
			struct bind_signal *e;

			if (!sync_op_is_signal(s))
				continue;
			e = find_signal(sigs, nsig, s->handle, s->timeline_value);
			pw_fence_put(e->fence);
			e->fence = pw_fence_get(b[i].done);
		}
	}

	/* 3. publish (drm_syncobj_add_point / replace_fence: cannot fail) */
	for (i = 0; i < nsig; i++) {
		if (sigs[i].pt) {
			pw_syncobj_add_point(sigs[i].so, sigs[i].pt, sigs[i].fence, sigs[i].point);
			sigs[i].pt = NULL;
		} else {
			pw_syncobj_replace_fence(sigs[i].so, sigs[i].fence);
		}
	}
out:
	for (i = 0; i < nsig; i++) {
		pw_syncobj_point_free(sigs[i].pt);
		pw_fence_put(sigs[i].fence);
		pw_syncobj_put(sigs[i].so);
	}
	pw_os_free(os, sigs);
	for (i = 0; i < n; i++)
		bind_op_cleanup(os, &b[i]);
	pw_os_free(os, b);
	return ret;
}

/* drv.c:1428 */
int pw_ioctl_vm_bind(struct pw_file *file, void *arg)
{
	struct drm_panthor_vm_bind *args = arg;
	struct drm_panthor_vm_bind_op *ops = NULL;
	struct pw_uvm *uvm;
	int ret;

	uvm = pw_file_vm_get(file, args->vm_id);
	if (!uvm)
		return PW_EINVAL;
	ret = pw_copy_obj_array(file, &args->ops, sizeof(*ops), sizeof(*ops), (void **)&ops);
	if (!ret) {
		if (args->flags & DRM_PANTHOR_VM_BIND_ASYNC)
			ret = vm_bind_async(file, uvm, args, ops);
		else
			ret = vm_bind_sync(file, uvm, args, ops);
	}
	pw_os_free(file->os, ops);
	pw_uvm_put(uvm);
	return ret;
}

/* ------------------------------------------------------------------------------------------ */
/* per-file state                                                                             */
/* ------------------------------------------------------------------------------------------ */

int pw_bo_file_init(struct pw_file *file)
{
	struct pw_bo_file *bf;
	int ret;

	bf = pw_os_zalloc(file->os, sizeof(*bf));
	if (!bf)
		return PW_ENOMEM;
	bf->lock = pw_os_mutex_create(file->os);
	if (!bf->lock) {
		ret = PW_ENOMEM;
		goto err_free;
	}
	ret = pw_handle_table_init(file->os, &file->bos);
	if (ret)
		goto err_lock;
	ret = pw_handle_table_init(file->os, &file->vms);
	if (ret)
		goto err_bos;
	file->bo_file = bf;
	return PW_OK;

err_bos:
	pw_handle_table_fini(file->os, &file->bos, NULL);
err_lock:
	pw_os_mutex_destroy(file->os, bf->lock);
err_free:
	pw_os_free(file->os, bf);
	return ret;
}

static void bo_table_release(void *obj)
{
	/* The mmap list is torn down wholesale by pw_bo_file_fini(). */
	pw_bo_put(obj);
}

void pw_bo_file_fini(struct pw_file *file)
{
	struct pw_bo_file *bf = file->bo_file;

	if (!bf)
		return;
	/* VMs first: their mappings hold BO references (and BOs only weakly name their VM). */
	pw_handle_table_fini(file->os, &file->vms, vm_handle_release);
	pw_os_mutex_lock(bf->lock);
	while (bf->mm_head)
		mm_unlink_locked(bf, bf->mm_head);
	pw_os_mutex_unlock(bf->lock);
	pw_handle_table_fini(file->os, &file->bos, bo_table_release);
	pw_os_mutex_destroy(file->os, bf->lock);
	pw_os_free(file->os, bf);
	file->bo_file = NULL;
}
