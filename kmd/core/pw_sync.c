// SPDX-License-Identifier: MIT
/* pw_sync.c - STUB (to be replaced by the real fence/syncobj implementation). */

#include "pw_sync.h"

struct pw_fence *pw_fence_create(struct pw_device *dev, uint64_t seqno) { (void)dev; (void)seqno; return NULL; }
struct pw_fence *pw_fence_create_signalled(struct pw_device *dev, int error) { (void)dev; (void)error; return NULL; }
struct pw_fence *pw_fence_get(struct pw_fence *f) { return f; }
void pw_fence_put(struct pw_fence *f) { (void)f; }
bool pw_fence_signal(struct pw_fence *f, int error) { (void)f; (void)error; return false; }
bool pw_fence_is_signalled(struct pw_fence *f) { (void)f; return true; }
int  pw_fence_error(struct pw_fence *f) { (void)f; return 0; }
uint64_t pw_fence_seqno(struct pw_fence *f) { (void)f; return 0; }
uint64_t pw_fence_signal_time_ns(struct pw_fence *f) { (void)f; return 0; }
int  pw_fence_add_callback(struct pw_fence *f, struct pw_fence_cb *cb, pw_fence_cb_fn fn) { (void)f; (void)cb; (void)fn; return PW_ENOENT; }
bool pw_fence_remove_callback(struct pw_fence *f, struct pw_fence_cb *cb) { (void)f; (void)cb; return false; }
int  pw_fence_wait(struct pw_fence *f, uint64_t deadline_ns) { (void)f; (void)deadline_ns; return PW_OK; }

int  pw_sync_file_init(struct pw_file *file) { file->sync = NULL; return PW_OK; }
void pw_sync_file_fini(struct pw_file *file) { (void)file; }

#define PW_SYNC_ENOTTY(name) \
	int name(struct pw_file *file, void *arg) { (void)file; (void)arg; return PW_ENOTTY; }
PW_SYNC_ENOTTY(pw_ioctl_syncobj_create)
PW_SYNC_ENOTTY(pw_ioctl_syncobj_destroy)
PW_SYNC_ENOTTY(pw_ioctl_syncobj_wait)
PW_SYNC_ENOTTY(pw_ioctl_syncobj_timeline_wait)
PW_SYNC_ENOTTY(pw_ioctl_syncobj_reset)
PW_SYNC_ENOTTY(pw_ioctl_syncobj_signal)
PW_SYNC_ENOTTY(pw_ioctl_syncobj_timeline_signal)
PW_SYNC_ENOTTY(pw_ioctl_syncobj_query)
PW_SYNC_ENOTTY(pw_ioctl_syncobj_transfer)

int pw_syncobj_find_fence(struct pw_file *file, uint32_t handle, uint64_t point, uint32_t flags,
			  uint64_t deadline_ns, struct pw_fence **out)
{ (void)file; (void)handle; (void)point; (void)flags; (void)deadline_ns; *out = NULL; return PW_EINVAL; }
int pw_syncobj_attach_fence(struct pw_file *file, uint32_t handle, uint64_t point, struct pw_fence *fence)
{ (void)file; (void)handle; (void)point; (void)fence; return PW_EINVAL; }

int pw_file_syncobj_notify(struct pw_file *file, uint32_t handle, uint64_t point, uint32_t flags,
			   pw_notify_fn fn, void *ctx, void **cookie)
{ (void)file; (void)handle; (void)point; (void)flags; (void)fn; (void)ctx; *cookie = NULL; return PW_EINVAL; }
bool pw_file_syncobj_notify_cancel(struct pw_file *file, void *cookie) { (void)file; (void)cookie; return false; }
