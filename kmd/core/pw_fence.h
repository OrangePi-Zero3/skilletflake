/* SPDX-License-Identifier: MIT */
/*
 * pw_fence.h - kernel fences (dma_fence equivalent), implemented in pw_sync.c.
 *
 * A fence is a refcounted one-shot completion with an error code. Producers: the scheduler
 * (job done), VM_BIND (bind done), userspace (SYNCOBJ_SIGNAL creates a pre-signalled fence).
 * Consumers: syncobjs, scheduler dependencies, waits, PANWIN_IOCTL_SYNCOBJ_EVENT notifies.
 * All functions are passive level. Callbacks run in the signaller's context with no fence lock
 * held; they must not block (they typically signal an event or queue work).
 */
#ifndef PW_FENCE_H
#define PW_FENCE_H

#include "pw_priv.h"

struct pw_fence;

struct pw_fence_cb;
typedef void (*pw_fence_cb_fn)(struct pw_fence *fence, struct pw_fence_cb *cb);

struct pw_fence_cb {
	struct pw_fence_cb *next, *prev;	/* owned by the fence while registered */
	pw_fence_cb_fn fn;
};

/* New unsignalled fence, refcount 1. NULL on allocation failure. @seqno is informational. */
struct pw_fence *pw_fence_create(struct pw_device *dev, uint64_t seqno);
/* Already-signalled fence with @error (0 = success). */
struct pw_fence *pw_fence_create_signalled(struct pw_device *dev, int error);
struct pw_fence *pw_fence_get(struct pw_fence *f);	/* returns f; NULL-safe */
void pw_fence_put(struct pw_fence *f);			/* NULL-safe */

/* Signal once (later calls are ignored, returns false). @error is 0 or a negative errno. */
bool pw_fence_signal(struct pw_fence *f, int error);
bool pw_fence_is_signalled(struct pw_fence *f);
int  pw_fence_error(struct pw_fence *f);	/* 0 while pending */
uint64_t pw_fence_seqno(struct pw_fence *f);
uint64_t pw_fence_signal_time_ns(struct pw_fence *f);	/* 0 while pending */

/* Register @fn; returns PW_OK, or PW_ENOENT (fn NOT called) if already signalled. */
int  pw_fence_add_callback(struct pw_fence *f, struct pw_fence_cb *cb, pw_fence_cb_fn fn);
/* True if removed before it ran; false if it already ran (or is running: the caller must then
 * synchronise with its own callback, e.g. via a refcount). */
bool pw_fence_remove_callback(struct pw_fence *f, struct pw_fence_cb *cb);

/* Wait. deadline_ns is absolute on pw_os_time_ns() (UINT64_MAX = forever).
 * PW_OK when signalled (regardless of the fence's error), PW_ETIME on deadline, PW_EINTR. */
int  pw_fence_wait(struct pw_fence *f, uint64_t deadline_ns);

#endif /* PW_FENCE_H */
