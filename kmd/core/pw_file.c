// SPDX-License-Identifier: MIT
/* pw_file.c - MINIMAL VERSION (to be replaced): file open/close and a dispatch table that only
 * routes the syncobj ioctls, so the sync module can be tested before the memory module lands. */

#include "pw_bo.h"
#include "pw_sched.h"
#include "pw_sync.h"

void pw_core_drm_version(uint32_t *major, uint32_t *minor, uint32_t *patch)
{
	*major = 1;
	*minor = 0;
	*patch = 0;
}

int pw_file_open(struct pw_device *dev, struct pw_file **out)
{
	struct pw_file *file = pw_os_zalloc(dev->os, sizeof(*file));
	int ret;

	if (!file)
		return PW_ENOMEM;
	file->dev = dev;
	file->os = dev->os;
	file->user_mmio_offset = DRM_PANTHOR_USER_MMIO_OFFSET_64BIT;
	ret = pw_bo_file_init(file);
	if (ret)
		goto err_free;
	ret = pw_sync_file_init(file);
	if (ret)
		goto err_bo;
	ret = pw_sched_file_init(file);
	if (ret)
		goto err_sync;
	*out = file;
	return PW_OK;

err_sync:
	pw_sync_file_fini(file);
err_bo:
	pw_bo_file_fini(file);
err_free:
	pw_os_free(dev->os, file);
	return ret;
}

void pw_file_close(struct pw_file *file)
{
	if (!file)
		return;
	pw_sched_file_fini(file);
	pw_sync_file_fini(file);
	pw_bo_file_fini(file);
	pw_os_free(file->os, file);
}

struct pw_ioctl_desc {
	pw_ioctl_fn fn;
	uint32_t size;
};

#define PW_DRM_NR(ioctl) ((ioctl) & 0xff)

int pw_file_ioctl(struct pw_file *file, uint32_t nr, void *arg, uint32_t size)
{
	static const struct {
		uint32_t nr;
		struct pw_ioctl_desc d;
	} table[] = {
		{ PW_DRM_NR(DRM_IOCTL_SYNCOBJ_CREATE), { pw_ioctl_syncobj_create, sizeof(struct drm_syncobj_create) } },
		{ PW_DRM_NR(DRM_IOCTL_SYNCOBJ_DESTROY), { pw_ioctl_syncobj_destroy, sizeof(struct drm_syncobj_destroy) } },
		{ PW_DRM_NR(DRM_IOCTL_SYNCOBJ_WAIT), { pw_ioctl_syncobj_wait, sizeof(struct drm_syncobj_wait) } },
		{ PW_DRM_NR(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT), { pw_ioctl_syncobj_timeline_wait, sizeof(struct drm_syncobj_timeline_wait) } },
		{ PW_DRM_NR(DRM_IOCTL_SYNCOBJ_RESET), { pw_ioctl_syncobj_reset, sizeof(struct drm_syncobj_array) } },
		{ PW_DRM_NR(DRM_IOCTL_SYNCOBJ_SIGNAL), { pw_ioctl_syncobj_signal, sizeof(struct drm_syncobj_array) } },
		{ PW_DRM_NR(DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL), { pw_ioctl_syncobj_timeline_signal, sizeof(struct drm_syncobj_timeline_array) } },
		{ PW_DRM_NR(DRM_IOCTL_SYNCOBJ_QUERY), { pw_ioctl_syncobj_query, sizeof(struct drm_syncobj_timeline_array) } },
		{ PW_DRM_NR(DRM_IOCTL_SYNCOBJ_TRANSFER), { pw_ioctl_syncobj_transfer, sizeof(struct drm_syncobj_transfer) } },
	};

	for (size_t i = 0; i < PW_ARRAY_SIZE(table); i++) {
		if (table[i].nr != nr)
			continue;
		if (size != table[i].d.size)
			return PW_EINVAL;
		return table[i].d.fn(file, arg);
	}
	return PW_ENOTTY;
}

int pw_file_mmap_lookup(struct pw_file *file, uint64_t offset, uint64_t size,
			struct pw_mmap_target *t)
{
	(void)file; (void)offset; (void)size; (void)t;
	return PW_ENOSYS;
}

int pw_copy_obj_array(struct pw_file *file, const struct drm_panthor_obj_array *arr,
		      uint32_t kernel_stride, uint32_t min_stride, void **out)
{
	(void)file; (void)arr; (void)kernel_stride; (void)min_stride;
	*out = NULL;
	return PW_ENOSYS;
}
