/* SPDX-License-Identifier: MIT */
/*
 * pw_jm_mem.h - job-manager memory: panfrost BOs, the per-file GPU address space and its VA
 * allocator, HEAP growth on GPU fault, per-BO implicit-sync fences. Implemented in pw_jm_mem.c.
 * Owner: jm-mem. Design: docs/status/jm-arch.md.
 *
 * Behaviour reference: Linux drivers/gpu/drm/panfrost (panfrost_drv.c BO ioctls,
 * panfrost_gem.c, panfrost_mmu.c), GPL-2.0-only: read it for behaviour, never copy code.
 * Citations are file:function, Linux v7.3.
 *
 * Reuse (decided in jm-arch.md):
 *  - A panfrost BO is a struct pw_bo (pw_bo.h) created with pw_bo_alloc(), published in the
 *    shared GEM handle table file->bos, so GEM_CLOSE (pw_ioctl_gem_close), pw_file_bo_get(), the
 *    fake mmap offsets (pw_bo_get_mmap_offset) and pw_file_mmap_lookup() work unchanged.
 *    bo->flags keeps DRM_PANTHOR_BO_* meaning: NO_MMAP for HEAP BOs, WB_MMAP for
 *    PANFROST_BO_WB_MMAP. The JM state is a struct pw_jm_bo in bo->ext, freed by
 *    bo->release_ext when the last reference goes.
 *  - A BO's GPU VA lives exactly as long as the BO: release_ext unmaps it and frees the VA.
 *    Linux unmaps at GEM_CLOSE unless a job still holds the mapping; here jobs (and user CPU
 *    mappings) hold BO references, which is equivalent for a single-file BO (no PRIME on Windows).
 *  - The address space is a struct pw_vm (pw_mmu.h page tables), wrapped in a refcounted
 *    struct pw_jm_vm with the VA allocator; one per file. AS slots: pw_jm_as_get (pw_jm_hw.h).
 *
 * Contexts: passive level. Ioctl handlers run in the caller's thread (user copies allowed).
 */
#ifndef PW_JM_MEM_H
#define PW_JM_MEM_H

#include "pw_bo.h"
#include "pw_fence.h"

#include "panfrost_drm.h"

/*
 * GPU VA rules (panfrost_mmu.c:panfrost_mmu_ctx_create drm_mm range, panfrost_gem.c:
 * panfrost_gem_open alignment, panfrost_mmu.c:panfrost_drm_mm_color_adjust; Mesa's
 * panfrost_kmod.c PANFROST_KMOD_VA_START/END assumes the same range):
 *  - user VAs are allocated in [32 MiB, 4 GiB); 0 is never returned (various units treat it
 *    specially);
 *  - executable BOs (no PANFROST_BO_NOEXEC) are aligned to their own size (the shader PC is
 *    24 bits, so they must not cross a 16 MiB boundary) and must not start or end on a 4 GiB
 *    boundary (with the 4 GiB limit: they end at most 4 GiB - 4 KiB);
 *  - NOEXEC BOs of 2 MiB or more are 2 MiB aligned (so 2 MiB blocks can be used), smaller ones
 *    page aligned;
 *  - HEAP BOs are NOEXEC, sizes rounded up to 2 MiB (panfrost_gem.c:panfrost_gem_create).
 * A full range fails CREATE_BO with PW_ENOSPC (drm_mm_insert_node_generic).
 */
#define PW_JM_VA_START		(32ull << 20)
#define PW_JM_VA_END		PW_SZ_4G
#define PW_JM_HEAP_CHUNK	PW_SZ_2M	/* panfrost_mmu.c NUM_FAULT_PAGES */
#define PW_JM_BO_LABEL_MAXLEN	4096		/* PANFROST_BO_LABEL_MAXLEN */

struct pw_jm_vm;	/* per-file address space: pw_vm + VA allocator, refcounted */
struct pw_jm_bo;	/* bo->ext of a panfrost BO */
struct pw_jm_mem_file;	/* file->jm_mem */

/* ---- device and file lifetime ---- */

/* Device-level state (dev->jm_mem), e.g. the implicit-sync lock below. After pw_jm_hw_init.
 * fini: quiesce (nothing to do unless state needs it); release: free dev->jm_mem. Both must
 * cope with init never having run. */
int  pw_jm_mem_init(struct pw_device *dev);
void pw_jm_mem_fini(struct pw_device *dev);
void pw_jm_mem_release(struct pw_device *dev);

/* Per file (panfrost_drv.c:panfrost_open -> panfrost_mmu_ctx_create): create the file's
 * pw_jm_vm (pw_vm_create(dev, false, ...), VA range above). Called after pw_bo_file_init().
 * fini drops the file's reference to the VM; BOs still alive (user mappings, jobs) keep it
 * alive until their release_ext ran. PW_OK / PW_ENOMEM / pw_vm_create() errors. */
int  pw_jm_mem_file_init(struct pw_file *file);
void pw_jm_mem_file_fini(struct pw_file *file);

/* ---- ioctl handlers (pw_ioctl_fn, panfrost structs; errnos as Linux panfrost) ---- */

/*
 * CREATE_BO (panfrost_drv.c:panfrost_ioctl_create_bo): size == 0, pad != 0 or unknown flags
 * -> PW_EINVAL (WB_MMAP is a 1.6 flag: unknown while PW_JM_DRM_MINOR < 6); HEAP without NOEXEC
 * -> PW_EINVAL; HEAP | WB_MMAP -> PW_EINVAL. Size page aligned (HEAP: 2 MiB aligned, no pages
 * allocated: pw_bo_alloc(backed = false)). Memory PW_MEM_ZEROED, PW_MEM_UNCACHED unless WB_MMAP
 * (non-coherent GPU, like pw_bo_create_user). Allocate the VA (PW_ENOSPC), map all pages
 * (non-heap) with PW_VM_MAP_NOEXEC for NOEXEC BOs, publish the handle, return handle + offset
 * (= GPU VA, never 0). args->size is not written back (Linux leaves it).
 */
int pw_jm_ioctl_create_bo(struct pw_file *file, void *arg);
/* MMAP_BO: flags != 0 -> PW_EINVAL; unknown handle -> PW_ENOENT; HEAP BO -> PW_EINVAL (pages
 * not pinned); else the fake mmap offset via pw_bo_get_mmap_offset(). */
int pw_jm_ioctl_mmap_bo(struct pw_file *file, void *arg);
/* GET_BO_OFFSET: unknown handle -> PW_ENOENT; BO without a mapping in this file -> PW_EINVAL
 * (cannot happen without PRIME); else offset = GPU VA. No pad check (Linux has none). */
int pw_jm_ioctl_get_bo_offset(struct pw_file *file, void *arg);
/*
 * WAIT_BO (panfrost_drv.c:panfrost_ioctl_wait_bo): pad != 0 -> PW_EINVAL; unknown handle ->
 * PW_ENOENT. Waits for the BO's implicit-sync fence (every job that used it). timeout_ns is
 * ABSOLUTE on pw_os_time_ns() (Linux: CLOCK_MONOTONIC; on Windows the QPC clock, PLAN.md
 * contract 3). A deadline that is 0 or already passed polls. Returns PW_OK when idle (a job
 * error does not matter), PW_EBUSY if not idle and the deadline had already passed (Linux:
 * zero jiffies), PW_ETIMEDOUT if the wait expired, PW_EINTR if the thread is being torn down.
 * Mesa asserts errno is ETIMEDOUT or EBUSY on failure.
 */
int pw_jm_ioctl_wait_bo(struct pw_file *file, void *arg);
/*
 * MADVISE (panfrost_drv.c:panfrost_ioctl_madvise): unknown handle -> PW_ENOENT. madv
 * PANFROST_MADV_DONTNEED marks the BO purgeable, WILLNEED unmarks it; retained = whether the
 * backing pages still exist. Windows has no shrinker yet, so pages are never purged and
 * retained is always 1 (documented deviation; a later shrinker must unmap the GPU VA range and
 * report 0 once purged). Linux's "only the owning file" EINVAL rule cannot trigger (no PRIME).
 */
int pw_jm_ioctl_madvise(struct pw_file *file, void *arg);
/* SET_LABEL_BO (1.4): pad != 0 -> PW_EINVAL; unknown handle -> PW_ENOENT; then
 * pw_bo_set_label_from_user() (NULL clears; no NUL within 4096 bytes -> PW_E2BIG). */
int pw_jm_ioctl_set_label_bo(struct pw_file *file, void *arg);
/*
 * SYNC_BO (1.6, panfrost_drv.c:panfrost_ioctl_sync_bo + panfrost_gem.c:panfrost_gem_sync): pad
 * != 0 -> PW_EINVAL; op_count 0 -> PW_OK; ops array copy -> PW_EFAULT; per op: unknown handle ->
 * PW_ENOENT, unknown type -> PW_EINVAL, range outside the BO -> PW_EINVAL; flush /
 * flush+invalidate with pw_os_mem_flush/_invalidate on cached BOs, no-op for write-combined ones
 * and HEAP BOs. Stops at the first failing op.
 */
int pw_jm_ioctl_sync_bo(struct pw_file *file, void *arg);
/* QUERY_BO_INFO (1.6): unknown handle -> PW_ENOENT; create_flags = the PANFROST_BO_* flags
 * given at creation, extra_flags = 0 (nothing is ever imported), pad = 0. */
int pw_jm_ioctl_query_bo_info(struct pw_file *file, void *arg);

/* ---- address space ---- */

/* The file's VM (borrowed: valid while the file is open). */
struct pw_jm_vm *pw_jm_file_vm(struct pw_file *file);
struct pw_jm_vm *pw_jm_vm_get(struct pw_jm_vm *jvm);	/* returns jvm; NULL-safe */
/* Reference unless the count already dropped to 0 (then NULL): for lookups that can race with
 * the final put, i.e. pw_jm_as_vm_get() under slots_lock. */
struct pw_jm_vm *pw_jm_vm_tryget(struct pw_jm_vm *jvm);
/* Final put: pw_jm_as_release() (unbind the AS slot), unmap/free what is left, pw_vm_destroy().
 * May run from a work item; not from an IRQ bottom half. NULL-safe. */
void pw_jm_vm_put(struct pw_jm_vm *jvm);
/* Its page tables (for pw_jm_as_get and the AS primitives in pw_mmu.h). */
struct pw_vm *pw_jm_vm_mmu(struct pw_jm_vm *jvm);

/* ---- BOs, for the scheduler ---- */

/* @bo's JM state, or NULL if @bo is not a panfrost BO (kernel BOs). */
struct pw_jm_bo *pw_jm_bo_of(struct pw_bo *bo);
/* GPU VA of a panfrost BO in its file's address space. */
uint64_t pw_jm_bo_va(struct pw_bo *bo);

/*
 * SUBMIT's BO list (panfrost_drv.c:panfrost_lookup_bos): copy @count u32 handles from user
 * pointer @handles, look each up with a reference. Returns PW_OK and a pw_os_zalloc'ed array
 * in *out (NULL when count == 0), PW_EFAULT, PW_ENOENT (any unknown handle, like
 * drm_gem_objects_lookup), PW_EINVAL (BO not mapped in this file), PW_ENOMEM. Nothing is held
 * on failure. Release with pw_jm_bos_put().
 */
int  pw_jm_bos_lookup(struct pw_file *file, uint64_t handles, uint32_t count,
		      struct pw_bo ***out);
void pw_jm_bos_put(struct pw_device *dev, struct pw_bo **bos, uint32_t count);	/* NULL-safe */

/*
 * Implicit sync. Linux panfrost treats every BO of a job as written (panfrost_job.c:
 * panfrost_job_push: implicit dependencies with write = true, then the job's fence added with
 * DMA_RESV_USAGE_WRITE), so jobs touching a BO are totally ordered and one fence per BO -
 * the last job's - covers all of them. WAIT_BO waits on it.
 *  lock/unlock: device-wide mutex the scheduler holds across "collect the fences of all BOs of
 *    a job as dependencies" and "publish the job's fence on all of them", so two concurrent
 *    submits sharing BOs cannot interleave (Linux: drm_gem_lock_reservations over the list).
 *  fence_get: the last job fence of @bo with a reference (may already be signalled), or NULL.
 *    Callable with or without the lock (WAIT_BO does not take it).
 *  fence_set: replace it with @fence (takes its own reference; drops the old one). Lock held.
 */
void pw_jm_resv_lock(struct pw_device *dev);
void pw_jm_resv_unlock(struct pw_device *dev);
struct pw_fence *pw_jm_bo_fence_get(struct pw_bo *bo);
void pw_jm_bo_fence_set(struct pw_bo *bo, struct pw_fence *fence);

/* ---- GPU page faults ---- */

/*
 * Called by pw_mmu_irq_handler() (MMU bottom half, slots_lock NOT held) for every AS fault on
 * a JM device, before the generic unhandled-fault path (panfrost_mmu.c:
 * panfrost_mmu_irq_handler_thread). Only translation faults (exception type 0xc0-0xc7) in a
 * HEAP BO are handled: find the VM bound to @as (pw_jm_as_vm_get), the BO covering @addr, and
 * populate the 2 MiB chunk containing it (allocate, map RW NOEXEC, TLB maintenance) unless it
 * is already mapped (panfrost_mmu.c:panfrost_mmu_map_fault_addr). Return PW_OK when the
 * access can be retried: the caller then clears the fault and leaves the AS enabled.
 * PW_ENOENT (no VM/BO there), PW_EINVAL (BO is not a heap, or not a translation fault),
 * PW_ENOMEM: the caller reports an unhandled fault and disables the AS, so the job fails.
 */
int pw_jm_mmu_fault(struct pw_device *dev, uint32_t as, uint32_t fault_status, uint64_t addr);

#endif /* PW_JM_MEM_H */
