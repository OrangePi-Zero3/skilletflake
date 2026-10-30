// SPDX-License-Identifier: MIT
/*
 * pw_jm_mem.c - job-manager memory (pw_jm_mem.h): panfrost BO ioctls, the per-file GPU address
 * space and its VA allocator, HEAP growth from the MMU fault handler, per-BO implicit-sync
 * fences. Owner: jm-mem. Design: docs/status/jm-arch.md (tasks B1-B8).
 *
 * Behaviour reference: Linux drivers/gpu/drm/panfrost v7.3 (panfrost_drv.c, panfrost_gem.c,
 * panfrost_mmu.c), GPL-2.0-only: read for behaviour and errnos only; no code or comments were
 * copied. Citations are file:function.
 *
 * Objects:
 *   struct pw_jm_mem       dev->jm_mem: the locks shared by all files and the VM registry.
 *   struct pw_jm_vm        one per file (panfrost_mmu_ctx_create): a pw_vm (page tables), the
 *                          VA allocator (a list of the file's BOs sorted by VA) and a refcount.
 *                          The file holds one reference, every BO of the file one, jobs one.
 *   struct pw_jm_bo        bo->ext of a panfrost BO: flags, GPU VA, heap chunks, the last job
 *                          fence. Freed from bo->release_ext with the BO's last reference, which
 *                          is also when its VA range is unmapped and released (Linux unmaps at
 *                          GEM_CLOSE unless a job still holds the mapping; jobs hold BO
 *                          references here, and without PRIME a BO has a single file).
 *
 * Locking (see also pw_jm_mem.h):
 *   jm->vms_lock     registry of live pw_jm_vm; held by the fault handler across a heap growth
 *     jvm->lock      the VA list, mapped state, heap chunk array of the BOs of one VM
 *       vm->op_lock, mmu.slots_lock   (pw_mmu.c, taken by pw_vm_map_mem / pw_vm_unmap)
 *   jm->resv_lock    pw_jm_resv_lock(), the scheduler's "collect + publish" section
 *     jm->fence_lock leaf: the fence pointer of every BO
 */

#include "pw_jm_hw.h"
#include "pw_jm_mem.h"

#define PW_SZ_16M		(16ull << 20)

struct pw_jm_mem {
	struct pw_os_mutex *resv_lock;
	struct pw_os_mutex *fence_lock;
	struct pw_os_mutex *vms_lock;
	struct pw_jm_vm *vms;		/* registry, under vms_lock */
};

struct pw_jm_vm {
	uint32_t refcount;		/* __atomic */
	struct pw_device *dev;
	struct pw_vm *vm;
	struct pw_os_mutex *lock;
	struct pw_jm_bo *bos;		/* sorted by va, non-overlapping, under @lock */
	struct pw_jm_vm *reg_prev, *reg_next;	/* jm->vms, under jm->vms_lock */
};

struct pw_jm_bo {
	struct pw_bo *bo;		/* back pointer (weak) */
	struct pw_jm_vm *jvm;		/* reference */
	struct pw_jm_bo *next;		/* jvm->bos */
	uint64_t va;
	uint32_t pf_flags;		/* PANFROST_BO_* given at creation */
	uint32_t madv;			/* PANFROST_MADV_*, informational (no shrinker) */
	bool linked;			/* in jvm->bos (owns [va, va + size)) */
	bool mapped;			/* non-heap: the whole range is mapped */
	struct pw_os_mem **chunks;	/* heap: size / 2 MiB slots, populated on fault */
	uint32_t nchunks, nmapped;
	struct pw_fence *fence;		/* implicit sync: last job, under jm->fence_lock */
};

struct pw_jm_mem_file {
	struct pw_jm_vm *jvm;
};

static void jm_bo_release(struct pw_bo *bo);

/* ------------------------------------------------------------------------------------------ */
/* device and file lifetime                                                                   */
/* ------------------------------------------------------------------------------------------ */

int pw_jm_mem_init(struct pw_device *dev)
{
	struct pw_jm_mem *jm;

	if (dev->jm_mem)
		return PW_OK;
	jm = pw_os_zalloc(dev->os, sizeof(*jm));
	if (!jm)
		return PW_ENOMEM;
	dev->jm_mem = jm;	/* published first: release frees a partial init */
	jm->resv_lock = pw_os_mutex_create(dev->os);
	jm->fence_lock = pw_os_mutex_create(dev->os);
	jm->vms_lock = pw_os_mutex_create(dev->os);
	if (!jm->resv_lock || !jm->fence_lock || !jm->vms_lock)
		return PW_ENOMEM;
	return PW_OK;
}

void pw_jm_mem_fini(struct pw_device *dev)
{
	(void)dev;	/* nothing runs on its own: the fault path only acts on GPU faults */
}

void pw_jm_mem_release(struct pw_device *dev)
{
	struct pw_jm_mem *jm = dev->jm_mem;

	if (!jm)
		return;
	if (jm->vms)
		pw_err(dev, "jm-mem: address spaces still alive at device release");
	if (jm->resv_lock)
		pw_os_mutex_destroy(dev->os, jm->resv_lock);
	if (jm->fence_lock)
		pw_os_mutex_destroy(dev->os, jm->fence_lock);
	if (jm->vms_lock)
		pw_os_mutex_destroy(dev->os, jm->vms_lock);
	pw_os_free(dev->os, jm);
	dev->jm_mem = NULL;
}

/* panfrost_mmu.c:panfrost_mmu_ctx_create: page tables + an empty [32 MiB, 4 GiB) range. */
static int jm_vm_create(struct pw_device *dev, struct pw_jm_vm **out)
{
	struct pw_jm_mem *jm = dev->jm_mem;
	struct pw_jm_vm *jvm;
	int ret;

	jvm = pw_os_zalloc(dev->os, sizeof(*jvm));
	if (!jvm)
		return PW_ENOMEM;
	jvm->dev = dev;
	jvm->refcount = 1;
	jvm->lock = pw_os_mutex_create(dev->os);
	if (!jvm->lock) {
		ret = PW_ENOMEM;
		goto err_free;
	}
	ret = pw_vm_create(dev, false, &jvm->vm);
	if (ret)
		goto err_lock;
	/* The VA range must fit in the GPU's input address size (the core needs >= 33 bits). */
	if (jvm->vm->va_size < PW_JM_VA_END) {
		pw_err(dev, "jm-mem: GPU VA size 0x%llx below the 4 GiB panfrost range",
		       (unsigned long long)jvm->vm->va_size);
		ret = PW_EOPNOTSUPP;
		goto err_vm;
	}

	pw_os_mutex_lock(jm->vms_lock);
	jvm->reg_next = jm->vms;
	if (jm->vms)
		jm->vms->reg_prev = jvm;
	jm->vms = jvm;
	pw_os_mutex_unlock(jm->vms_lock);
	*out = jvm;
	return PW_OK;

err_vm:
	pw_vm_destroy(jvm->vm);
err_lock:
	pw_os_mutex_destroy(dev->os, jvm->lock);
err_free:
	pw_os_free(dev->os, jvm);
	return ret;
}

int pw_jm_mem_file_init(struct pw_file *file)
{
	struct pw_jm_mem_file *mf;
	int ret;

	if (!file->dev->jm_mem)
		return PW_EINVAL;
	mf = pw_os_zalloc(file->os, sizeof(*mf));
	if (!mf)
		return PW_ENOMEM;
	ret = jm_vm_create(file->dev, &mf->jvm);
	if (ret) {
		pw_os_free(file->os, mf);
		return ret;
	}
	file->jm_mem = mf;
	return PW_OK;
}

void pw_jm_mem_file_fini(struct pw_file *file)
{
	struct pw_jm_mem_file *mf = file->jm_mem;

	if (!mf)
		return;
	file->jm_mem = NULL;
	pw_jm_vm_put(mf->jvm);	/* BOs and jobs still alive keep the VM */
	pw_os_free(file->os, mf);
}

/* ------------------------------------------------------------------------------------------ */
/* address space                                                                              */
/* ------------------------------------------------------------------------------------------ */

struct pw_jm_vm *pw_jm_file_vm(struct pw_file *file)
{
	return file->jm_mem ? file->jm_mem->jvm : NULL;
}

struct pw_jm_vm *pw_jm_vm_get(struct pw_jm_vm *jvm)
{
	if (jvm)
		__atomic_add_fetch(&jvm->refcount, 1, __ATOMIC_RELAXED);
	return jvm;
}

struct pw_jm_vm *pw_jm_vm_tryget(struct pw_jm_vm *jvm)
{
	uint32_t cur;

	if (!jvm)
		return NULL;
	cur = __atomic_load_n(&jvm->refcount, __ATOMIC_RELAXED);
	do {
		if (!cur)
			return NULL;
	} while (!__atomic_compare_exchange_n(&jvm->refcount, &cur, cur + 1, true,
					      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
	return jvm;
}

/* panfrost_mmu.c:panfrost_mmu_release_ctx */
void pw_jm_vm_put(struct pw_jm_vm *jvm)
{
	struct pw_device *dev;
	struct pw_jm_mem *jm;

	if (!jvm || __atomic_sub_fetch(&jvm->refcount, 1, __ATOMIC_ACQ_REL))
		return;
	dev = jvm->dev;
	jm = dev->jm_mem;

	/* Out of the registry first: once the lock is released, the fault handler cannot be
	 * looking at this VM any more (it holds vms_lock throughout). */
	pw_os_mutex_lock(jm->vms_lock);
	if (jvm->reg_prev)
		jvm->reg_prev->reg_next = jvm->reg_next;
	else
		jm->vms = jvm->reg_next;
	if (jvm->reg_next)
		jvm->reg_next->reg_prev = jvm->reg_prev;
	pw_os_mutex_unlock(jm->vms_lock);

	if (jvm->bos)	/* every BO holds a reference: cannot happen */
		pw_err(dev, "jm-mem: VM destroyed with BOs still mapped");
	pw_jm_as_release(dev, jvm);
	pw_vm_destroy(jvm->vm);	/* also unbinds an AS the LRU did not know about */
	pw_os_mutex_destroy(dev->os, jvm->lock);
	pw_os_free(dev->os, jvm);
}

struct pw_vm *pw_jm_vm_mmu(struct pw_jm_vm *jvm)
{
	return jvm ? jvm->vm : NULL;
}

/*
 * VA placement, the rules of panfrost_gem.c:panfrost_gem_open (alignment) and
 * panfrost_mmu.c:panfrost_drm_mm_color_adjust (executable BOs), applied to each free hole
 * [*start, *end) of [32 MiB, 4 GiB) in turn, first fit (Linux uses drm_mm's best fit: the
 * address chosen may differ, the constraints are the same).
 * Executable holes: never start or end on a 4 GiB boundary, and a hole that starts within
 * 16 MiB of the next 4 GiB boundary is moved past it, then clipped one page short of the
 * boundary after its (new) start. With our 4 GiB limit this means executable BOs end at most
 * at 4 GiB - 4 KiB and need a hole starting more than 16 MiB below 4 GiB.
 */
static void va_color_adjust(bool exec, uint64_t *start, uint64_t *end)
{
	uint64_t next_seg;

	if (!exec)
		return;
	if (!(*start & (PW_SZ_4G - 1)))
		*start += PW_SZ_4K;
	if (!(*end & (PW_SZ_4G - 1)))
		*end -= PW_SZ_4K;
	next_seg = PW_ALIGN_UP(*start, PW_SZ_4G);
	if (next_seg - *start <= PW_SZ_16M)
		*start = next_seg + PW_SZ_4K;
	*end = PW_MIN(*end, PW_ALIGN_UP(*start, PW_SZ_4G) - PW_SZ_4K);
}

/*
 * Executable BOs are aligned to their own size, which need not be a power of two (drm_mm
 * aligns to any multiple): they then never cross a 16 MiB boundary as long as they are at
 * most 16 MiB (the shader PC is 24 bits). NOEXEC BOs of 2 MiB or more are 2 MiB aligned.
 */
static int va_alloc_locked(struct pw_jm_vm *jvm, struct pw_jm_bo *jbo, uint64_t size, bool exec)
{
	uint64_t align = exec ? size : (size >= PW_SZ_2M ? PW_SZ_2M : PW_SZ_4K);
	uint64_t hole_start = PW_JM_VA_START;
	struct pw_jm_bo **link = &jvm->bos;

	for (;;) {
		struct pw_jm_bo *it = *link;
		uint64_t s = hole_start, e = it ? it->va : PW_JM_VA_END, rem;

		va_color_adjust(exec, &s, &e);
		rem = s % align;
		if (rem)
			s += align - rem;
		if (s < e && e - s >= size) {
			jbo->va = s;
			jbo->next = it;
			*link = jbo;
			jbo->linked = true;
			return PW_OK;
		}
		if (!it)
			return PW_ENOSPC;	/* drm_mm_insert_node_generic */
		hole_start = it->va + it->bo->size;
		link = &it->next;
	}
}

static void va_free_locked(struct pw_jm_vm *jvm, struct pw_jm_bo *jbo)
{
	struct pw_jm_bo **link;

	if (!jbo->linked)
		return;
	for (link = &jvm->bos; *link; link = &(*link)->next) {
		if (*link == jbo) {
			*link = jbo->next;
			break;
		}
	}
	jbo->next = NULL;
	jbo->linked = false;
}

static struct pw_jm_bo *va_find_locked(struct pw_jm_vm *jvm, uint64_t addr)
{
	for (struct pw_jm_bo *it = jvm->bos; it && it->va <= addr; it = it->next) {
		if (addr - it->va < it->bo->size)
			return it;
	}
	return NULL;
}

/* ------------------------------------------------------------------------------------------ */
/* BOs                                                                                        */
/* ------------------------------------------------------------------------------------------ */

struct pw_jm_bo *pw_jm_bo_of(struct pw_bo *bo)
{
	return (bo && bo->release_ext == jm_bo_release) ? bo->ext : NULL;
}

uint64_t pw_jm_bo_va(struct pw_bo *bo)
{
	struct pw_jm_bo *jbo = pw_jm_bo_of(bo);

	return jbo ? jbo->va : 0;
}

/*
 * Last reference gone (pw_bo_put): unmap the GPU range, free the heap chunks (after the unmap
 * and its FLUSH_MEM, so the GPU cannot write them any more), release the VA, drop the fence
 * and the VM (panfrost_gem.c:panfrost_gem_teardown_mapping + panfrost_gem_free_object).
 */
static void jm_bo_release(struct pw_bo *bo)
{
	struct pw_jm_bo *jbo = bo->ext;
	struct pw_jm_vm *jvm = jbo->jvm;
	struct pw_os *os = bo->dev->os;
	bool leak = false;
	int ret;

	pw_os_mutex_lock(jvm->lock);
	if (jbo->mapped || jbo->nmapped) {
		ret = pw_vm_unmap(jvm->vm, jbo->va, bo->size);
		if (ret) {
			/* (review) The descriptors may still point at the pages (a failed LOCK
			 * leaves them untouched, a failed split leaves part of them) and the TLB
			 * may still hold them: freeing the pages now would let the GPU, or the
			 * AS once a reset re-enables it, write memory Windows reuses. Leak them
			 * instead; a later map of this VA range fails with PW_EBUSY rather than
			 * aliasing. Only reachable with a hung GPU or out of memory. */
			pw_err(bo->dev, "jm-mem: unmap of BO at VA 0x%llx failed (%d), leaking "
			       "its pages", (unsigned long long)jbo->va, ret);
			leak = true;
		}
	}
	va_free_locked(jvm, jbo);
	pw_os_mutex_unlock(jvm->lock);

	if (leak)
		bo->mem = NULL;	/* pw_bo_put() then skips the free */
	if (jbo->chunks) {
		for (uint32_t i = 0; !leak && i < jbo->nchunks; i++) {
			if (jbo->chunks[i])
				pw_os_mem_free(os, jbo->chunks[i]);
		}
		pw_os_free(os, jbo->chunks);
	}
	pw_fence_put(jbo->fence);
	bo->ext = NULL;
	pw_os_free(os, jbo);
	pw_jm_vm_put(jvm);
}

/*
 * panfrost_drv.c:panfrost_ioctl_create_bo + panfrost_gem.c:panfrost_gem_create/_open +
 * panfrost_mmu.c:panfrost_mmu_map. The GPU mapping is always cacheable (IOMMU_CACHE in Linux);
 * the CPU side is write-combined unless WB_MMAP (panfrost_gem.c:should_map_wc).
 */
int pw_jm_bo_create_user(struct pw_file *file, struct drm_panfrost_create_bo *args,
			 uint32_t allowed_flags)
{
	struct pw_device *dev = file->dev;
	struct pw_jm_vm *jvm = pw_jm_file_vm(file);
	const uint32_t flags = args->flags;
	const bool heap = flags & PANFROST_BO_HEAP;
	const bool noexec = flags & PANFROST_BO_NOEXEC;
	const bool wb = flags & PANFROST_BO_WB_MMAP;
	struct pw_jm_bo *jbo;
	struct pw_bo *bo;
	uint64_t size = args->size, va;
	uint32_t handle, mem_flags;
	bool cpu_cached;
	int ret;

	if (!args->size || args->pad || (flags & ~allowed_flags))
		return PW_EINVAL;
	if (heap && !noexec)		/* heaps are never executable */
		return PW_EINVAL;
	if (heap && wb)			/* heaps are not CPU-visible */
		return PW_EINVAL;
	if (!jvm)
		return PW_EINVAL;

	/* Heaps are 2 MiB multiples so faults can populate whole chunks. */
	if (heap)
		size = PW_ALIGN_UP(size, PW_JM_HEAP_CHUNK);
	cpu_cached = dev->coherent || wb;
	mem_flags = PW_MEM_ZEROED | (cpu_cached ? 0 : PW_MEM_UNCACHED);
	ret = pw_bo_alloc(dev, size, mem_flags, cpu_cached, !heap, &bo);
	if (ret)
		return ret;
	jbo = pw_os_zalloc(dev->os, sizeof(*jbo));
	if (!jbo) {
		pw_bo_put(bo);
		return PW_ENOMEM;
	}
	jbo->bo = bo;
	jbo->jvm = pw_jm_vm_get(jvm);
	jbo->pf_flags = flags;
	jbo->madv = PANFROST_MADV_WILLNEED;
	bo->ext = jbo;
	bo->release_ext = jm_bo_release;
	/* DRM_PANTHOR_BO_* meaning for the shared mmap code (pw_bo.h). */
	bo->flags = (heap ? DRM_PANTHOR_BO_NO_MMAP : 0) | (wb ? DRM_PANTHOR_BO_WB_MMAP : 0);

	pw_os_mutex_lock(jvm->lock);
	ret = va_alloc_locked(jvm, jbo, bo->size, !noexec);
	if (!ret && !heap) {
		ret = pw_vm_map_mem(jvm->vm, jbo->va, bo->mem, 0, bo->size,
				    noexec ? PW_VM_MAP_NOEXEC : 0);
		jbo->mapped = !ret;
	}
	va = jbo->va;
	pw_os_mutex_unlock(jvm->lock);
	if (ret) {
		pw_bo_put(bo);	/* release_ext undoes whatever was done */
		return ret;
	}

	ret = pw_handle_alloc(file->os, &file->bos, bo, &handle);
	if (ret) {
		pw_bo_put(bo);
		return ret;
	}
	/* The handle owns the BO now (a racing GEM_CLOSE may already have freed it). */
	args->handle = handle;
	args->offset = va;
	return PW_OK;
}

int pw_jm_ioctl_create_bo(struct pw_file *file, void *arg)
{
	uint32_t major, minor, patch, allowed = PANFROST_BO_NOEXEC | PANFROST_BO_HEAP;

	pw_jm_drm_version(&major, &minor, &patch);
	if (major > 1 || minor >= 6)	/* panfrost 1.6 added WB_MMAP */
		allowed |= PANFROST_BO_WB_MMAP;
	return pw_jm_bo_create_user(file, arg, allowed);
}

/* panfrost_drv.c:panfrost_ioctl_mmap_bo */
int pw_jm_ioctl_mmap_bo(struct pw_file *file, void *arg)
{
	struct drm_panfrost_mmap_bo *args = arg;
	struct pw_jm_bo *jbo;
	struct pw_bo *bo;
	uint64_t offset;
	int ret;

	if (args->flags)
		return PW_EINVAL;
	bo = pw_file_bo_get(file, args->handle);
	if (!bo)
		return PW_ENOENT;
	jbo = pw_jm_bo_of(bo);
	/* Heap pages come and go with faults: never CPU-mappable. */
	if (jbo && (jbo->pf_flags & PANFROST_BO_HEAP))
		ret = PW_EINVAL;
	else
		ret = pw_bo_get_mmap_offset(file, args->handle, &offset);
	if (!ret)
		args->offset = offset;
	pw_bo_put(bo);
	return ret;
}

/* panfrost_drv.c:panfrost_ioctl_get_bo_offset (no pad check there either) */
int pw_jm_ioctl_get_bo_offset(struct pw_file *file, void *arg)
{
	struct drm_panfrost_get_bo_offset *args = arg;
	struct pw_jm_bo *jbo;
	struct pw_bo *bo;
	int ret = PW_OK;

	bo = pw_file_bo_get(file, args->handle);
	if (!bo)
		return PW_ENOENT;
	jbo = pw_jm_bo_of(bo);
	if (!jbo || jbo->jvm != pw_jm_file_vm(file))
		ret = PW_EINVAL;	/* no mapping in this file */
	else
		args->offset = jbo->va;
	pw_bo_put(bo);
	return ret;
}

/*
 * panfrost_drv.c:panfrost_ioctl_wait_bo: drm_timeout_abs_to_jiffies() turns the absolute
 * CLOCK_MONOTONIC deadline into a remaining time (0 or past: poll), then
 * dma_resv_wait_timeout(); "not idle" is -EBUSY for a poll and -ETIMEDOUT for an expired wait.
 * Mesa (panfrost_kmod.c:panfrost_kmod_bo_wait) passes 0 or INT64_MAX.
 */
int pw_jm_ioctl_wait_bo(struct pw_file *file, void *arg)
{
	struct drm_panfrost_wait_bo *args = arg;
	struct pw_fence *fence;
	struct pw_bo *bo;
	uint64_t deadline;
	int ret;

	if (args->pad)
		return PW_EINVAL;
	bo = pw_file_bo_get(file, args->handle);
	if (!bo)
		return PW_ENOENT;
	fence = pw_jm_bo_fence_get(bo);
	pw_bo_put(bo);
	if (!fence || pw_fence_is_signalled(fence)) {
		pw_fence_put(fence);
		return PW_OK;
	}

	if (args->timeout_ns <= 0 ||
	    (uint64_t)args->timeout_ns <= pw_os_time_ns(file->os)) {
		ret = PW_EBUSY;
	} else {
		deadline = args->timeout_ns == INT64_MAX ? UINT64_MAX : (uint64_t)args->timeout_ns;
		ret = pw_fence_wait(fence, deadline);
		if (ret == PW_ETIME)
			ret = PW_ETIMEDOUT;
	}
	pw_fence_put(fence);
	return ret;
}

/*
 * panfrost_drv.c:panfrost_ioctl_madvise. There is no shrinker on Windows yet, so nothing is
 * ever purged and the pages are always retained (pw_jm_mem.h).
 */
int pw_jm_ioctl_madvise(struct pw_file *file, void *arg)
{
	struct drm_panfrost_madvise *args = arg;
	struct pw_jm_bo *jbo;
	struct pw_bo *bo;
	int ret = PW_OK;

	bo = pw_file_bo_get(file, args->handle);
	if (!bo)
		return PW_ENOENT;
	jbo = pw_jm_bo_of(bo);
	if (!jbo) {
		ret = PW_EINVAL;
	} else {
		__atomic_store_n(&jbo->madv, args->madv, __ATOMIC_RELAXED);
		args->retained = 1;
	}
	pw_bo_put(bo);
	return ret;
}

/* panfrost_drv.c:panfrost_ioctl_set_label_bo (1.4) */
int pw_jm_ioctl_set_label_bo(struct pw_file *file, void *arg)
{
	struct drm_panfrost_set_label_bo *args = arg;
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

/* panfrost_gem.c:panfrost_gem_sync, checks in Linux order. Offsets/sizes are u32. */
static int jm_bo_sync_one(struct pw_file *file, const struct drm_panfrost_bo_sync_op *op)
{
	struct pw_bo *bo = pw_file_bo_get(file, op->handle);
	uint32_t end = op->offset + op->size;
	int ret = PW_OK;

	if (!bo)
		return PW_ENOENT;
	if (end < op->offset || end > bo->size)
		ret = PW_EINVAL;
	else if (op->type != PANFROST_BO_SYNC_CPU_CACHE_FLUSH &&
		 op->type != PANFROST_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE)
		ret = PW_EINVAL;
	else if (!pw_bo_cpu_cached(bo) || !bo->mem || !op->size)
		ret = PW_OK;	/* write-combined (or unbacked heap) mapping, or nothing to do */
	else if (op->type == PANFROST_BO_SYNC_CPU_CACHE_FLUSH)
		pw_os_mem_flush(file->os, bo->mem, op->offset, op->size);
	else
		pw_os_mem_invalidate(file->os, bo->mem, op->offset, op->size);
	pw_bo_put(bo);
	return ret;
}

/* panfrost_drv.c:panfrost_ioctl_sync_bo (1.6): all ops are copied in first, then run in
 * order until one fails. */
int pw_jm_ioctl_sync_bo(struct pw_file *file, void *arg)
{
	struct drm_panfrost_sync_bo *args = arg;
	struct drm_panfrost_bo_sync_op *ops;
	size_t bytes;
	int ret;

	if (args->pad)
		return PW_EINVAL;
	if (!args->op_count)
		return PW_OK;
	if (args->op_count > PW_JM_MAX_SYNC_OPS)
		return PW_ENOMEM;
	bytes = (size_t)args->op_count * sizeof(*ops);
	ops = pw_os_zalloc(file->os, bytes);
	if (!ops)
		return PW_ENOMEM;
	ret = pw_os_copy_from_user(file->os, ops, args->ops, bytes);
	for (uint32_t i = 0; !ret && i < args->op_count; i++)
		ret = jm_bo_sync_one(file, &ops[i]);
	pw_os_free(file->os, ops);
	return ret;
}

/* panfrost_drv.c:panfrost_ioctl_query_bo_info (1.6). Nothing is ever imported on Windows. */
int pw_jm_ioctl_query_bo_info(struct pw_file *file, void *arg)
{
	struct drm_panfrost_query_bo_info *args = arg;
	struct pw_jm_bo *jbo;
	struct pw_bo *bo;

	bo = pw_file_bo_get(file, args->handle);
	if (!bo)
		return PW_ENOENT;
	jbo = pw_jm_bo_of(bo);
	args->pad = 0;
	args->extra_flags = 0;
	args->create_flags = jbo ? jbo->pf_flags : 0;
	pw_bo_put(bo);
	return PW_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* SUBMIT's BO list                                                                           */
/* ------------------------------------------------------------------------------------------ */

/* panfrost_drv.c:panfrost_lookup_bos + drm_gem_objects_lookup: copy, look every handle up
 * (any missing -> ENOENT), then require a mapping in this file (-> EINVAL). Duplicate
 * handles are allowed and give duplicate entries. */
int pw_jm_bos_lookup(struct pw_file *file, uint64_t handles, uint32_t count,
		     struct pw_bo ***out)
{
	struct pw_jm_vm *jvm = pw_jm_file_vm(file);
	struct pw_bo **bos = NULL;
	uint32_t *h = NULL, n = 0;
	int ret;

	*out = NULL;
	if (!count)
		return PW_OK;
	if (count > PW_JM_MAX_JOB_BOS)
		return PW_ENOMEM;
	h = pw_os_zalloc(file->os, (size_t)count * sizeof(*h));
	bos = pw_os_zalloc(file->os, (size_t)count * sizeof(*bos));
	if (!h || !bos) {
		ret = PW_ENOMEM;
		goto err;
	}
	ret = pw_os_copy_from_user(file->os, h, handles, (size_t)count * sizeof(*h));
	if (ret)
		goto err;
	for (n = 0; n < count; n++) {
		bos[n] = pw_file_bo_get(file, h[n]);
		if (!bos[n]) {
			ret = PW_ENOENT;
			goto err;
		}
	}
	for (uint32_t i = 0; i < count; i++) {
		struct pw_jm_bo *jbo = pw_jm_bo_of(bos[i]);

		if (!jbo || jbo->jvm != jvm) {
			ret = PW_EINVAL;
			goto err;
		}
	}
	pw_os_free(file->os, h);
	*out = bos;
	return PW_OK;

err:
	if (bos) {
		for (uint32_t i = 0; i < n; i++)
			pw_bo_put(bos[i]);
		pw_os_free(file->os, bos);
	}
	pw_os_free(file->os, h);
	return ret;
}

void pw_jm_bos_put(struct pw_device *dev, struct pw_bo **bos, uint32_t count)
{
	if (!bos)
		return;
	for (uint32_t i = 0; i < count; i++)
		pw_bo_put(bos[i]);
	pw_os_free(dev->os, bos);
}

/* ------------------------------------------------------------------------------------------ */
/* implicit sync                                                                              */
/* ------------------------------------------------------------------------------------------ */

void pw_jm_resv_lock(struct pw_device *dev)
{
	if (dev->jm_mem)
		pw_os_mutex_lock(dev->jm_mem->resv_lock);
}

void pw_jm_resv_unlock(struct pw_device *dev)
{
	if (dev->jm_mem)
		pw_os_mutex_unlock(dev->jm_mem->resv_lock);
}

struct pw_fence *pw_jm_bo_fence_get(struct pw_bo *bo)
{
	struct pw_jm_bo *jbo = pw_jm_bo_of(bo);
	struct pw_jm_mem *jm;
	struct pw_fence *f;

	if (!jbo)
		return NULL;
	jm = bo->dev->jm_mem;
	pw_os_mutex_lock(jm->fence_lock);
	f = pw_fence_get(jbo->fence);
	pw_os_mutex_unlock(jm->fence_lock);
	return f;
}

void pw_jm_bo_fence_set(struct pw_bo *bo, struct pw_fence *fence)
{
	struct pw_jm_bo *jbo = pw_jm_bo_of(bo);
	struct pw_jm_mem *jm;
	struct pw_fence *old;

	if (!jbo)
		return;
	jm = bo->dev->jm_mem;
	pw_fence_get(fence);
	pw_os_mutex_lock(jm->fence_lock);
	old = jbo->fence;
	jbo->fence = fence;
	pw_os_mutex_unlock(jm->fence_lock);
	pw_fence_put(old);
}

/* ------------------------------------------------------------------------------------------ */
/* GPU page faults                                                                            */
/* ------------------------------------------------------------------------------------------ */

/* The live VM whose page tables are bound to @as, or NULL. vms_lock held. */
static struct pw_jm_vm *jm_vm_bound_to(struct pw_device *dev, uint32_t as)
{
	struct pw_vm *vm = NULL;

	if (as >= PW_MAX_AS)
		return NULL;
	pw_os_mutex_lock(dev->mmu.slots_lock);
	vm = dev->mmu.slots[as];
	pw_os_mutex_unlock(dev->mmu.slots_lock);
	if (!vm)
		return NULL;
	for (struct pw_jm_vm *it = dev->jm_mem->vms; it; it = it->reg_next) {
		if (it->vm == vm && __atomic_load_n(&it->refcount, __ATOMIC_ACQUIRE))
			return it;
	}
	return NULL;
}

/*
 * panfrost_mmu.c:panfrost_mmu_map_fault_addr: populate the 2 MiB chunk of the heap BO
 * containing @addr (zeroed pages, RW NOEXEC cacheable), or do nothing if a previous fault
 * already did. pw_vm_map_mem() does the TLB maintenance (LOCK, FLUSH_PT) on the bound AS.
 */
static int jm_heap_grow(struct pw_jm_vm *jvm, uint64_t addr)
{
	struct pw_device *dev = jvm->dev;
	struct pw_os_mem *mem = NULL;
	struct pw_jm_bo *jbo;
	uint64_t chunk_va;
	uint32_t idx;
	int ret = PW_OK;

	pw_os_mutex_lock(jvm->lock);
	jbo = va_find_locked(jvm, addr);
	if (!jbo) {
		ret = PW_ENOENT;
		goto out;
	}
	if (!(jbo->pf_flags & PANFROST_BO_HEAP)) {
		pw_warn(dev, "GPU fault at VA 0x%llx in a BO at 0x%llx that is not a heap",
			(unsigned long long)addr, (unsigned long long)jbo->va);
		ret = PW_EINVAL;
		goto out;
	}
	/* Heap BOs are NOEXEC multiples of 2 MiB, hence 2 MiB aligned. */
	idx = (uint32_t)((addr - jbo->va) / PW_JM_HEAP_CHUNK);
	chunk_va = jbo->va + (uint64_t)idx * PW_JM_HEAP_CHUNK;
	if (!jbo->chunks) {
		jbo->nchunks = (uint32_t)(jbo->bo->size / PW_JM_HEAP_CHUNK);
		jbo->chunks = pw_os_zalloc(dev->os, (size_t)jbo->nchunks * sizeof(*jbo->chunks));
		if (!jbo->chunks) {
			jbo->nchunks = 0;
			ret = PW_ENOMEM;
			goto out;
		}
	}
	if (jbo->chunks[idx])
		goto out;	/* already populated: the GPU just retries */

	ret = pw_os_mem_alloc(dev->os, PW_JM_HEAP_CHUNK, PW_MEM_ZEROED | PW_MEM_UNCACHED, &mem);
	if (ret) {
		ret = PW_ENOMEM;
		goto out;
	}
	ret = pw_vm_map_mem(jvm->vm, chunk_va, mem, 0, PW_JM_HEAP_CHUNK, PW_VM_MAP_NOEXEC);
	if (ret) {
		pw_os_mem_free(dev->os, mem);
		goto out;
	}
	jbo->chunks[idx] = mem;
	jbo->nmapped++;
	pw_dbg(dev, "heap BO at 0x%llx: mapped chunk 0x%llx on fault at 0x%llx",
	       (unsigned long long)jbo->va, (unsigned long long)chunk_va,
	       (unsigned long long)addr);
out:
	pw_os_mutex_unlock(jvm->lock);
	return ret;
}

/* panfrost_mmu.c:panfrost_mmu_irq_handler_thread: only translation faults ("page fault
 * only": exception types 0xc0-0xc7) are candidates for growing a heap. */
int pw_jm_mmu_fault(struct pw_device *dev, uint32_t as, uint32_t fault_status, uint64_t addr)
{
	struct pw_jm_mem *jm = dev->jm_mem;
	struct pw_jm_vm *jvm;
	int ret;

	if (!jm)
		return PW_ENOENT;
	if ((AS_FAULTSTATUS_EXCEPTION_TYPE(fault_status) & 0xf8) != 0xc0)
		return PW_EINVAL;

	pw_os_mutex_lock(jm->vms_lock);
	jvm = jm_vm_bound_to(dev, as);
	ret = jvm ? jm_heap_grow(jvm, addr) : PW_ENOENT;
	pw_os_mutex_unlock(jm->vms_lock);
	return ret;
}
