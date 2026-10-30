/* SPDX-License-Identifier: MIT */
/*
 * pw_file.h - per-open-file state, handle tables and the ioctl handler convention.
 *
 * Module ownership (each module implements the functions declared in its own header):
 *   pw_file.c   file open/close, the DRM core table + one driver table per backend, DEV_QUERY/
 *               VERSION/GET_CAP, emulated driver versions (panthor and panfrost), mmap lookup
 *   pw_bo.c     BOs + user VMs (pw_bo.h): BO_*, VM_*, GEM_CLOSE, SET_USER_MMIO_OFFSET; also
 *               the shared struct pw_bo / GEM handle table / mmap offsets used by JM BOs
 *   pw_sync.c   fences + syncobjs (pw_fence.h, pw_sync.h): SYNCOBJ_*, both backends
 *   pw_sched.c  groups/queues/submit + tiler heaps (pw_sched.h): GROUP_*, TILER_HEAP_*
 *  (added by jm-arch: the job-manager backend, docs/status/jm-arch.md)
 *   pw_jm_hw.c    JM GPU (pw_jm_hw.h): shader/tiler power, GET_PARAM, job-slot registers, job
 *                 IRQ, soft reset/recovery, AS slot LRU. Also owns pw_regs_jm.h.
 *   pw_jm_mem.c   JM memory (pw_jm_mem.h): CREATE_BO, MMAP_BO, GET_BO_OFFSET, WAIT_BO, MADVISE,
 *                 SET_LABEL_BO, SYNC_BO, QUERY_BO_INFO; per-file AS + VA allocator, HEAP growth
 *                 from the MMU fault handler, per-BO implicit-sync fences, the TLB-maintenance
 *                 part of pw_mmu.c on JM.
 *   pw_jm_sched.c JM scheduler (pw_jm_sched.h): SUBMIT, JM_CTX_CREATE/DESTROY, slot queues,
 *                 dependencies, completion, timeouts, reset policy.
 * Shared code the JM modules call but do not own: pw_gpu.c (GPU_ID, reset, L2, flush), pw_irq.c,
 * pw_mmu.c (page tables + pw_mmu_as_bind/unbind_locked), pw_handle.c, pw_sync.c, pw_bo.c.
 */
#ifndef PW_FILE_H
#define PW_FILE_H

#include "pw_priv.h"

/* ---- handle tables (pw_handle.c): small-int handles >= 1 -> object pointers ---- */
struct pw_handle_table {
	struct pw_os_mutex *lock;
	void **objs;			/* objs[h - 1] */
	uint32_t cap;
	uint32_t count;
};

int   pw_handle_table_init(struct pw_os *os, struct pw_handle_table *t);
/* Calls @release(obj) (if non-NULL) for every remaining entry, then frees the table. */
void  pw_handle_table_fini(struct pw_os *os, struct pw_handle_table *t, void (*release)(void *obj));
/* Lowest free handle. PW_ENOMEM on failure. The table takes over the caller's reference. */
int   pw_handle_alloc(struct pw_os *os, struct pw_handle_table *t, void *obj, uint32_t *handle);
/* Look up and, under the table lock, take a reference with @get (if non-NULL). NULL if absent. */
void *pw_handle_get(struct pw_handle_table *t, uint32_t handle, void (*get)(void *obj));
/* Remove; returns the object with the table's reference transferred to the caller, or NULL. */
void *pw_handle_remove(struct pw_handle_table *t, uint32_t handle);
/* Replace the object at an existing handle; returns the old one (reference to caller), or NULL
 * (and does nothing) if the handle is not allocated. */
void *pw_handle_replace(struct pw_handle_table *t, uint32_t handle, void *obj);

/* ---- the file ---- */
struct pw_sync_file;
struct pw_sched_file;
struct pw_bo_file;
struct pw_jm_mem_file;
struct pw_jm_sched_file;

struct pw_file {
	struct pw_device *dev;
	struct pw_os *os;

	struct pw_handle_table bos;	/* GEM handles -> struct pw_bo * (pw_bo.c) */
	struct pw_handle_table vms;	/* VM ids -> struct pw_uvm * (pw_bo.c) */
	struct pw_sync_file *sync;	/* pw_sync.c */
	struct pw_sched_file *sched;	/* pw_sched.c */
	struct pw_bo_file *bo_file;	/* pw_bo.c: mmap-offset map, labels (added by core-mem) */
	/* (added by jm-arch) JM files only; NULL on CSF. `bos`, `bo_file` and `sync` are used
	 * by both backends; `vms` and `sched` stay empty/NULL on JM. */
	struct pw_jm_mem_file *jm_mem;		/* pw_jm_mem.c: the file's AS + VA allocator */
	struct pw_jm_sched_file *jm_sched;	/* pw_jm_sched.c: JM contexts */

	/* DRM_PANTHOR_SET_USER_MMIO_OFFSET; defaults to the 64-bit value. Both offsets are accepted
	 * by pw_file_mmap_lookup() regardless (Mesa on Windows computes the 32-bit one). */
	uint64_t user_mmio_offset;
};

/*
 * ioctl handler convention: @arg is the kernel copy of the Linux uAPI struct (already validated
 * to be exactly the expected size by the dispatcher). Return PW_OK or a negative errno. Nested
 * user pointers go through pw_os_copy_{from,to}_user(file->os, ...).
 */
typedef int (*pw_ioctl_fn)(struct pw_file *file, void *arg);

/* Validate a drm_panthor_obj_array and copy it in, element-size tolerant like panthor's
 * PANTHOR_UOBJ_GET_ARRAY (smaller user stride: zero-extend; larger: trailing bytes must be 0).
 * *out is pw_os_zalloc'ed (count * kernel_stride), caller frees. count==0 -> *out = NULL, PW_OK. */
int pw_copy_obj_array(struct pw_file *file, const struct drm_panthor_obj_array *arr,
		      uint32_t kernel_stride, uint32_t min_stride, void **out);

/* ---- additions (jm-arch) ---- */

/*
 * Emulated Linux panfrost DRM version (panfrost_drv.c changelog). Cumulative; Mesa's
 * panfrost_kmod.c switches on it: 1.1 HEAP/NOEXEC (Mesa refuses < 1.1), 1.2 AFBC_FEATURES,
 * 1.3 JD_REQ_CYCLE_COUNT + SYSTEM_TIMESTAMP(_FREQUENCY), 1.4 SET_LABEL_BO, 1.5 JM_CTX_*,
 * 1.6 WB_MMAP + SYNC_BO + QUERY_BO_INFO + SELECTED_COHERENCY, 1.7 PERFCNT_DUMP state.
 * We report 1.5: 1.6 makes Mesa use cached CPU mappings with user-mode cache maintenance, the
 * same unverified-on-Windows feature that keeps panthor at 1.6 instead of 1.7 (see pw_file.c).
 * Driver ioctls newer than the reported minor return PW_ENOTTY.
 */
#define PW_JM_DRM_NAME		"panfrost"	/* pan_kmod_dev_create() matches on this */
#define PW_JM_DRM_MAJOR		1
#define PW_JM_DRM_MINOR		5
#define PW_JM_DRM_PATCH		0
void pw_jm_drm_version(uint32_t *major, uint32_t *minor, uint32_t *patch);

#endif /* PW_FILE_H */
