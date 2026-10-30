/* SPDX-License-Identifier: MIT */
/*
 * pw_file.h - per-open-file state, handle tables and the ioctl handler convention.
 *
 * Module ownership (each module implements the functions declared in its own header):
 *   pw_file.c   file open/close, ioctl dispatch table, DEV_QUERY/VERSION/GET_CAP, mmap lookup
 *   pw_bo.c     BOs + user VMs (pw_bo.h): BO_*, VM_*, GEM_CLOSE, SET_USER_MMIO_OFFSET
 *   pw_sync.c   fences + syncobjs (pw_fence.h, pw_sync.h): SYNCOBJ_*
 *   pw_sched.c  groups/queues/submit + tiler heaps (pw_sched.h): GROUP_*, TILER_HEAP_*
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

struct pw_file {
	struct pw_device *dev;
	struct pw_os *os;

	struct pw_handle_table bos;	/* GEM handles -> struct pw_bo * (pw_bo.c) */
	struct pw_handle_table vms;	/* VM ids -> struct pw_uvm * (pw_bo.c) */
	struct pw_sync_file *sync;	/* pw_sync.c */
	struct pw_sched_file *sched;	/* pw_sched.c */

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

#endif /* PW_FILE_H */
