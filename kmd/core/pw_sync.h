/* SPDX-License-Identifier: MIT */
/*
 * pw_sync.h - DRM syncobjs (binary + timeline) per file, implemented in pw_sync.c.
 * Semantics follow Linux drivers/gpu/drm/drm_syncobj.c (timeline = dma_fence_chain of points).
 */
#ifndef PW_SYNC_H
#define PW_SYNC_H

#include "pw_fence.h"
#include "pw_file.h"

int  pw_sync_file_init(struct pw_file *file);
void pw_sync_file_fini(struct pw_file *file);

/* DRM core ioctl handlers (pw_ioctl_fn), structs from drm.h. */
int pw_ioctl_syncobj_create(struct pw_file *file, void *arg);		/* drm_syncobj_create */
int pw_ioctl_syncobj_destroy(struct pw_file *file, void *arg);		/* drm_syncobj_destroy */
int pw_ioctl_syncobj_wait(struct pw_file *file, void *arg);		/* drm_syncobj_wait */
int pw_ioctl_syncobj_timeline_wait(struct pw_file *file, void *arg);	/* drm_syncobj_timeline_wait */
int pw_ioctl_syncobj_reset(struct pw_file *file, void *arg);		/* drm_syncobj_array */
int pw_ioctl_syncobj_signal(struct pw_file *file, void *arg);		/* drm_syncobj_array */
int pw_ioctl_syncobj_timeline_signal(struct pw_file *file, void *arg);	/* drm_syncobj_timeline_array */
int pw_ioctl_syncobj_query(struct pw_file *file, void *arg);		/* drm_syncobj_timeline_array */
int pw_ioctl_syncobj_transfer(struct pw_file *file, void *arg);		/* drm_syncobj_transfer */
/* HANDLE_TO_FD / FD_TO_HANDLE / EVENTFD are not supported on Windows: the dispatcher returns
 * PW_EOPNOTSUPP for them. */

/*
 * Kernel-internal use (scheduler, VM_BIND):
 * find: the fence for @point (0 = binary/current) with a reference, like drm_syncobj_find_fence.
 *   PW_EINVAL if no such syncobj or no fence yet (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT in @flags
 *   makes it wait for a fence to be attached, interruptibly, until @deadline_ns).
 * attach: point 0 replaces the binary fence; point > 0 adds a timeline point (must be > the last
 *   point; otherwise it's still added but ordered like Linux's chain does: see drm_syncobj.c).
 */
int pw_syncobj_find_fence(struct pw_file *file, uint32_t handle, uint64_t point, uint32_t flags,
			  uint64_t deadline_ns, struct pw_fence **out);
int pw_syncobj_attach_fence(struct pw_file *file, uint32_t handle, uint64_t point,
			    struct pw_fence *fence);

/* ---- additions (core-sync) ---- */

/* Upper bound on count_handles of SYNCOBJ_WAIT/TIMELINE_WAIT/RESET/SIGNAL/TIMELINE_SIGNAL/QUERY
 * (Linux is only bounded by kmalloc); larger arrays fail with PW_EINVAL. */
#define PW_SYNC_MAX_HANDLES (1u << 14)

/*
 * Object-level API, for callers that must not fail after the point of no return (panthor's
 * GROUP_SUBMIT and VM_BIND resolve the syncobjs and preallocate the chain nodes up front, then
 * call drm_syncobj_add_point()/drm_syncobj_replace_fence(), which cannot fail).
 * lookup: a reference to the syncobj behind @handle, or NULL (Linux: drm_syncobj_find).
 * point_alloc: preallocated timeline point (Linux: dma_fence_chain_alloc); NULL on ENOMEM.
 * add_point: add (@point, @fence) to the timeline, consuming @pt (never fails); @point 0 or a
 *   point <= the last one follows drm_syncobj_add_point() exactly (new chain context, seqno =
 *   max(last, point)). The caller holds a reference on @so for the duration of the call.
 * replace_fence: binary semantics, NULL resets (drm_syncobj_replace_fence).
 * Waiters (SYNCOBJ_WAIT with WAIT_FOR_SUBMIT, notifies, find_fence) are woken by both.
 */
struct pw_syncobj;
struct pw_syncobj_point;
struct pw_syncobj *pw_syncobj_lookup(struct pw_file *file, uint32_t handle);
struct pw_syncobj *pw_syncobj_get(struct pw_syncobj *so);
void pw_syncobj_put(struct pw_syncobj *so);	/* NULL-safe */
struct pw_syncobj_point *pw_syncobj_point_alloc(struct pw_device *dev);
void pw_syncobj_point_free(struct pw_syncobj_point *pt);	/* unused preallocation; NULL-safe */
void pw_syncobj_add_point(struct pw_syncobj *so, struct pw_syncobj_point *pt,
			  struct pw_fence *fence, uint64_t point);
void pw_syncobj_replace_fence(struct pw_syncobj *so, struct pw_fence *fence);

#endif /* PW_SYNC_H */
