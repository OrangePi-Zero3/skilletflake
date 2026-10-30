// SPDX-License-Identifier: MIT
/*
 * pw_file.c - per-open-file layer (pw_core.h): open/close, the DRM ioctl dispatcher with
 * DRM-style struct size extension, DRM_IOCTL_VERSION/GET_CAP, DRM_PANTHOR_DEV_QUERY,
 * SET_USER_MMIO_OFFSET, mmap lookup and the panthor uobj copy helpers.
 *
 * Derived from Linux panthor (dual GPL-2.0/MIT, used under MIT): panthor_drv.c (Copyright
 * 2018 Marty E. Plummer, 2019 Linaro Ltd, 2019 Collabora ltd). The DRM core behaviour (size
 * extension, VERSION's two-pass strings, GET_CAP) is re-implemented from its documented uAPI
 * semantics; no DRM core code was used. Line citations are Linux v7.3.
 *
 * (jm-arch) Two backends (enum pw_backend): the DRM core table is shared, the driver ioctls
 * come from the panthor table (CSF) or the panfrost table (JM). Linux panfrost (GPL-2.0-only)
 * was read for behaviour only (panfrost_drv.c: ioctl list, version, driver features).
 */

#include "pw_bo.h"
#include "pw_jm_hw.h"
#include "pw_jm_mem.h"
#include "pw_jm_sched.h"
#include "pw_sched.h"
#include "pw_sync.h"

/*
 * Emulated panthor version (drv.c:1774-1792 changelog). Versions are cumulative and Mesa
 * (src/panfrost/lib/kmod/panthor_kmod.c) switches features on them, so we report the highest
 * minor whose features are all implemented and safe on Windows:
 *   1.1 TIMESTAMP_INFO, 1.2 GROUP_PRIORITIES_INFO   - here
 *   1.3 GROUP_STATE_INNOCENT                        - only set after a GPU reset (none yet)
 *   1.4 BO_SET_LABEL, 1.5 SET_USER_MMIO_OFFSET      - here / pw_bo.c
 *   1.6 GLB_COUNTER_EN                              - pw_fw.c global init
 * Not reported:
 *   1.7 WB_MMAP + BO_SYNC + BO_QUERY_INFO: implemented in the core, but reporting it makes
 *       panvk use cached CPU mappings and EL0 "dc cvac/civac" cache maintenance, which is
 *       unverified on Windows ARM64 (docs/status/mesa.md), and needs the OS layer to map BOs
 *       cacheable. Flip PW_DRM_MINOR to 7 once both are checked on hardware.
 *   1.8 timestamp flags: implemented (a superset is harmless) but 1.7 must come first.
 *   1.9 MAP_SPARSE: not implemented (MMU_INFO is answered anyway).
 */
#define PW_DRM_MAJOR	1
#define PW_DRM_MINOR	6
#define PW_DRM_PATCH	0

#define PW_DRM_NAME	"panthor"	/* Mesa's pan_kmod_drm_drv_match() looks for this */
#define PW_DRM_DATE	"0"		/* DRM core reports "0" since driver dates were removed */
#define PW_DRM_DESC	"Panthor DRM driver (panwin)"
#define PW_JM_DRM_DESC	"panfrost DRM (panwin)"	/* panfrost_drv.c .desc "panfrost DRM" */

#define PW_UOBJ_MAX_ARRAY_BYTES	(1u << 28)	/* sanity cap on pw_copy_obj_array() */

void pw_core_drm_version(uint32_t *major, uint32_t *minor, uint32_t *patch)
{
	*major = PW_DRM_MAJOR;
	*minor = PW_DRM_MINOR;
	*patch = PW_DRM_PATCH;
}

void pw_jm_drm_version(uint32_t *major, uint32_t *minor, uint32_t *patch)
{
	*major = PW_JM_DRM_MAJOR;
	*minor = PW_JM_DRM_MINOR;
	*patch = PW_JM_DRM_PATCH;
}

static bool file_is_jm(const struct pw_file *file)
{
	return file->dev->backend == PW_BACKEND_JM;
}

/* ------------------------------------------------------------------------------------------ */
/* open / close                                                                               */
/* ------------------------------------------------------------------------------------------ */

/*
 * (jm-arch) JM file: GEM handles + mmap offsets (pw_bo.c), the file's address space (jm_mem),
 * syncobjs, JM contexts (the default context 0 is created here, as panfrost_jm_open does).
 */
static int file_open_jm(struct pw_file *file)
{
	int ret;

	ret = pw_bo_file_init(file);
	if (ret)
		return ret;
	ret = pw_jm_mem_file_init(file);
	if (ret)
		goto err_bo;
	ret = pw_sync_file_init(file);
	if (ret)
		goto err_mem;
	ret = pw_jm_sched_file_init(file);
	if (ret)
		goto err_sync;
	return PW_OK;

err_sync:
	pw_sync_file_fini(file);
err_mem:
	pw_jm_mem_file_fini(file);
err_bo:
	pw_bo_file_fini(file);
	return ret;
}

/* Reverse of file_open_jm(): jobs hold BO, AS and fence references; BOs hold the AS. */
static void file_close_jm(struct pw_file *file)
{
	pw_jm_sched_file_fini(file);
	pw_sync_file_fini(file);
	pw_jm_mem_file_fini(file);
	pw_bo_file_fini(file);
}

int pw_file_open(struct pw_device *dev, struct pw_file **out)
{
	struct pw_file *file;
	int ret;

	if (!dev || !out)
		return PW_EINVAL;
	file = pw_os_zalloc(dev->os, sizeof(*file));
	if (!file)
		return PW_ENOMEM;
	file->dev = dev;
	file->os = dev->os;
	if (dev->backend == PW_BACKEND_JM) {
		ret = file_open_jm(file);
		if (ret)
			goto err_free;
		*out = file;
		return PW_OK;
	}
	/* drv.c:1604: DRM_PANTHOR_USER_MMIO_OFFSET of a 64-bit caller. */
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
	if (file_is_jm(file)) {
		file_close_jm(file);
		pw_os_free(file->os, file);
		return;
	}
	/* Reverse of open: groups/heaps hold VM and BO references, VM_BIND fences live in
	 * syncobjs, VMs hold BO references. */
	pw_sched_file_fini(file);
	pw_sync_file_fini(file);
	pw_bo_file_fini(file);
	pw_os_free(file->os, file);
}

/* ------------------------------------------------------------------------------------------ */
/* user copy helpers                                                                          */
/* ------------------------------------------------------------------------------------------ */

static const uint8_t pw_zeroes[64];

/* clear_user() */
static int pw_clear_user(struct pw_os *os, uint64_t ptr, uint64_t size)
{
	while (size) {
		size_t n = (size_t)PW_MIN(size, (uint64_t)sizeof(pw_zeroes));
		int ret = pw_os_copy_to_user(os, ptr, pw_zeroes, n);

		if (ret)
			return ret;
		ptr += n;
		size -= n;
	}
	return PW_OK;
}

/* check_zeroed_user(): PW_OK if all zero, PW_E2BIG if not, PW_EFAULT. */
static int pw_check_zeroed_user(struct pw_os *os, uint64_t ptr, uint64_t size)
{
	uint8_t buf[64];

	while (size) {
		size_t n = (size_t)PW_MIN(size, (uint64_t)sizeof(buf));
		int ret = pw_os_copy_from_user(os, buf, ptr, n);

		if (ret)
			return ret;
		if (pw_memcmp(buf, pw_zeroes, n))
			return PW_E2BIG;
		ptr += n;
		size -= n;
	}
	return PW_OK;
}

/* copy_struct_from_user(): a smaller user struct is zero-extended; a larger one is accepted only
 * if the bytes we don't know about are zero (PW_E2BIG otherwise). */
static int pw_copy_struct_from_user(struct pw_os *os, void *dst, size_t ksize, uint64_t src,
				    size_t usize)
{
	size_t n = PW_MIN(ksize, usize);
	int ret;

	if (usize > ksize) {
		ret = pw_check_zeroed_user(os, src + ksize, usize - ksize);
		if (ret)
			return ret;
	}
	ret = pw_os_copy_from_user(os, dst, src, n);
	if (ret)
		return ret;
	if (ksize > usize)
		pw_memset((uint8_t *)dst + usize, 0, ksize - usize);
	return PW_OK;
}

/* drv.c:59 panthor_set_uobj() */
static int pw_set_uobj(struct pw_os *os, uint64_t usr_ptr, uint32_t usr_size, uint32_t min_size,
		       const void *in, uint32_t kern_size)
{
	int ret;

	if (usr_size < min_size)
		return PW_EINVAL;
	ret = pw_os_copy_to_user(os, usr_ptr, in, PW_MIN(usr_size, kern_size));
	if (ret)
		return PW_EFAULT;
	if (usr_size > kern_size && pw_clear_user(os, usr_ptr + kern_size, usr_size - kern_size))
		return PW_EFAULT;
	return PW_OK;
}

/* drv.c:92 panthor_get_uobj_array() */
int pw_copy_obj_array(struct pw_file *file, const struct drm_panthor_obj_array *arr,
		      uint32_t kernel_stride, uint32_t min_stride, void **out)
{
	uint64_t total;
	uint8_t *buf;
	int ret = PW_OK;

	*out = NULL;
	if (!arr->count)
		return PW_OK;
	if (arr->stride < min_stride)
		return PW_EINVAL;
	total = (uint64_t)arr->count * kernel_stride;
	/* kvmalloc_array() would fail on absurd sizes too; keep the host allocator sane. */
	if (total > PW_UOBJ_MAX_ARRAY_BYTES)
		return PW_ENOMEM;
	buf = pw_os_zalloc(file->os, (size_t)total);
	if (!buf)
		return PW_ENOMEM;

	if (kernel_stride == arr->stride) {
		/* Fast path when user/kernel have the same uAPI header version. */
		ret = pw_os_copy_from_user(file->os, buf, arr->array, (size_t)total);
	} else {
		for (uint32_t i = 0; i < arr->count && !ret; i++)
			ret = pw_copy_struct_from_user(file->os, buf + (size_t)i * kernel_stride,
						       kernel_stride,
						       arr->array + (uint64_t)i * arr->stride,
						       arr->stride);
	}
	if (ret) {
		pw_os_free(file->os, buf);
		return ret;
	}
	*out = buf;
	return PW_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* DRM core ioctls                                                                            */
/* ------------------------------------------------------------------------------------------ */

/* DRM drm_copy_field(): copy at most *buf_len bytes (no terminator), report the full length. */
static int pw_copy_field(struct pw_os *os, char *ubuf, __kernel_size_t *buf_len, const char *value)
{
	size_t len = pw_strlen(value);
	size_t n = PW_MIN(len, (size_t)*buf_len);

	*buf_len = len;
	if (n && ubuf)
		return pw_os_copy_to_user(os, (uint64_t)(uintptr_t)ubuf, value, n);
	return PW_OK;
}

static int pw_ioctl_version(struct pw_file *file, void *arg)
{
	struct drm_version *v = arg;
	const bool jm = file_is_jm(file);
	uint32_t major, minor, patch;
	int ret;

	pw_device_drm_version(file->dev, &major, &minor, &patch, NULL);
	v->version_major = (int)major;
	v->version_minor = (int)minor;
	v->version_patchlevel = (int)patch;
	ret = pw_copy_field(file->os, v->name, &v->name_len, jm ? PW_JM_DRM_NAME : PW_DRM_NAME);
	if (!ret)
		ret = pw_copy_field(file->os, v->date, &v->date_len, PW_DRM_DATE);
	if (!ret)
		ret = pw_copy_field(file->os, v->desc, &v->desc_len, jm ? PW_JM_DRM_DESC : PW_DRM_DESC);
	return ret;
}

/* DRM drm_getcap() for a render-only driver with DRIVER_SYNCOBJ | DRIVER_SYNCOBJ_TIMELINE
 * (drv.c:1790). No PRIME on Windows: DRM_CAP_PRIME is 0.
 * (jm-arch) panfrost has DRIVER_SYNCOBJ only (panfrost_drv.c driver_features): no timelines. */
static int pw_ioctl_get_cap(struct pw_file *file, void *arg)
{
	struct drm_get_cap *c = arg;

	c->value = 0;
	switch (c->capability) {
	case DRM_CAP_TIMESTAMP_MONOTONIC:
	case DRM_CAP_SYNCOBJ:
		c->value = 1;
		return PW_OK;
	case DRM_CAP_SYNCOBJ_TIMELINE:
		c->value = file_is_jm(file) ? 0 : 1;
		return PW_OK;
	case DRM_CAP_PRIME:
		return PW_OK;
	default:
		return PW_EOPNOTSUPP;	/* other caps need DRIVER_MODESET */
	}
}

/* ------------------------------------------------------------------------------------------ */
/* panthor ioctls owned here                                                                  */
/* ------------------------------------------------------------------------------------------ */

/* drv.c:766 */
#define PW_VALID_TIMESTAMP_QUERY_FLAGS \
	((uint32_t)DRM_PANTHOR_TIMESTAMP_GPU | (uint32_t)DRM_PANTHOR_TIMESTAMP_CPU_TYPE_MASK | \
	 (uint32_t)DRM_PANTHOR_TIMESTAMP_GPU_OFFSET | (uint32_t)DRM_PANTHOR_TIMESTAMP_GPU_CYCLE_COUNT | \
	 (uint32_t)DRM_PANTHOR_TIMESTAMP_FREQ | (uint32_t)DRM_PANTHOR_TIMESTAMP_DURATION)

/*
 * drv.c:775 panthor_query_timestamp_info(). Windows has no CLOCK_MONOTONIC(_RAW): both CPU
 * clock types return pw_os_time_ns() (QueryPerformanceCounter, Mesa's os_time base). We can't
 * disable preemption/interrupts around the reads like panthor does.
 */
static int pw_query_timestamp_info(struct pw_device *dev, struct drm_panthor_timestamp_info *ts)
{
	uint32_t flags = ts->flags ? ts->flags :
		(uint32_t)(DRM_PANTHOR_TIMESTAMP_GPU | DRM_PANTHOR_TIMESTAMP_GPU_OFFSET |
			   DRM_PANTHOR_TIMESTAMP_FREQ);
	uint32_t cpu_type = flags & (uint32_t)DRM_PANTHOR_TIMESTAMP_CPU_TYPE_MASK;
	uint64_t t0 = 0, cpu_ns = 0;

	if (cpu_type != (uint32_t)DRM_PANTHOR_TIMESTAMP_CPU_NONE &&
	    cpu_type != (uint32_t)DRM_PANTHOR_TIMESTAMP_CPU_MONOTONIC &&
	    cpu_type != (uint32_t)DRM_PANTHOR_TIMESTAMP_CPU_MONOTONIC_RAW)
		return PW_EINVAL;
	if (flags & ~PW_VALID_TIMESTAMP_QUERY_FLAGS)
		return PW_EINVAL;

	/* CNTFRQ_EL0: the GPU timestamp counts the SoC system counter (fw.timer_hz, 24 MHz on
	 * RK3588 unless the OS passes another rate). */
	ts->timestamp_frequency = (flags & DRM_PANTHOR_TIMESTAMP_FREQ) ?
		(dev->fw.timer_hz ? dev->fw.timer_hz : 24000000u) : 0;
	ts->timestamp_offset = (flags & DRM_PANTHOR_TIMESTAMP_GPU_OFFSET) ?
		gpu_read64(dev, GPU_TIMESTAMP_OFFSET) : 0;		/* gpu.c:476 */
	if (flags & DRM_PANTHOR_TIMESTAMP_DURATION)
		t0 = pw_os_time_ns(dev->os);
	ts->current_timestamp = (flags & DRM_PANTHOR_TIMESTAMP_GPU) ?
		gpu_read64_counter(dev, GPU_TIMESTAMP) : 0;		/* gpu.c:471 */
	if (cpu_type)
		cpu_ns = pw_os_time_ns(dev->os);
	ts->cycle_count = (flags & DRM_PANTHOR_TIMESTAMP_GPU_CYCLE_COUNT) ?
		gpu_read64_counter(dev, GPU_CYCLE_COUNT) : 0;		/* gpu.c:481 */
	ts->duration_nsec = (flags & DRM_PANTHOR_TIMESTAMP_DURATION) ?
		(uint32_t)(pw_os_time_ns(dev->os) - t0) : 0;
	ts->cpu_timestamp_sec = cpu_type ? cpu_ns / 1000000000u : 0;
	ts->cpu_timestamp_nsec = cpu_type ? cpu_ns % 1000000000u : 0;
	return PW_OK;
}

/* drv.c:897 panthor_query_group_priorities_info(): LOW and MEDIUM are always allowed; HIGH and
 * REALTIME need CAP_SYS_NICE / DRM master, which have no equivalent here. */
static void pw_query_group_priorities_info(struct drm_panthor_group_priorities_info *p)
{
	pw_memset(p, 0, sizeof(*p));
	p->allowed_mask = (uint8_t)(PW_BIT(PANTHOR_GROUP_PRIORITY_LOW) |
				    PW_BIT(PANTHOR_GROUP_PRIORITY_MEDIUM));
}

#define PW_OFFSETOFEND(type, member) (offsetof(type, member) + sizeof(((type *)0)->member))

/* drv.c:173 PANTHOR_UOBJ_MIN_SIZE: the uAPI 1.0 size of each object. */
#define PW_MIN_GPU_INFO		PW_OFFSETOFEND(struct drm_panthor_gpu_info, tiler_present)
#define PW_MIN_CSIF_INFO	PW_OFFSETOFEND(struct drm_panthor_csif_info, pad)
#define PW_MIN_MMU_INFO		PW_OFFSETOFEND(struct drm_panthor_mmu_info, page_size_bitmap)
#define PW_MIN_TIMESTAMP_INFO	PW_OFFSETOFEND(struct drm_panthor_timestamp_info, current_timestamp)
#define PW_MIN_PRIO_INFO	PW_OFFSETOFEND(struct drm_panthor_group_priorities_info, pad)

/* drv.c:910 panthor_ioctl_dev_query() */
static int pw_ioctl_dev_query(struct pw_file *file, void *arg)
{
	struct drm_panthor_dev_query *q = arg;
	struct pw_device *dev = file->dev;
	struct pw_os *os = file->os;
	union {
		struct drm_panthor_gpu_info gpu;
		struct drm_panthor_csif_info csif;
		struct drm_panthor_mmu_info mmu;
		struct drm_panthor_timestamp_info ts;
		struct drm_panthor_group_priorities_info prio;
	} u;
	int ret;

	if (!q->pointer) {
		switch (q->type) {
		case DRM_PANTHOR_DEV_QUERY_GPU_INFO:
			q->size = sizeof(u.gpu);
			return PW_OK;
		case DRM_PANTHOR_DEV_QUERY_CSIF_INFO:
			q->size = sizeof(u.csif);
			return PW_OK;
		case DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO:
			q->size = sizeof(u.ts);
			return PW_OK;
		case DRM_PANTHOR_DEV_QUERY_GROUP_PRIORITIES_INFO:
			q->size = sizeof(u.prio);
			return PW_OK;
		case DRM_PANTHOR_DEV_QUERY_MMU_INFO:
			q->size = sizeof(u.mmu);
			return PW_OK;
		default:
			return PW_EINVAL;
		}
	}

	pw_memset(&u, 0, sizeof(u));
	switch (q->type) {
	case DRM_PANTHOR_DEV_QUERY_GPU_INFO:
		pw_device_get_gpu_info(dev, &u.gpu);
		return pw_set_uobj(os, q->pointer, q->size, PW_MIN_GPU_INFO, &u.gpu, sizeof(u.gpu));

	case DRM_PANTHOR_DEV_QUERY_CSIF_INFO:
		/* Only fails without firmware (M1 bring-up mode), which panthor never is in. */
		ret = pw_device_get_csif_info(dev, &u.csif);
		if (ret)
			return ret;
		return pw_set_uobj(os, q->pointer, q->size, PW_MIN_CSIF_INFO, &u.csif,
				   sizeof(u.csif));

	case DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO:
		ret = pw_copy_struct_from_user(os, &u.ts, sizeof(u.ts), q->pointer, q->size);
		if (ret)
			return ret;
		ret = pw_query_timestamp_info(dev, &u.ts);
		if (ret)
			return ret;
		return pw_set_uobj(os, q->pointer, q->size, PW_MIN_TIMESTAMP_INFO, &u.ts,
				   sizeof(u.ts));

	case DRM_PANTHOR_DEV_QUERY_GROUP_PRIORITIES_INFO:
		pw_query_group_priorities_info(&u.prio);
		return pw_set_uobj(os, q->pointer, q->size, PW_MIN_PRIO_INFO, &u.prio,
				   sizeof(u.prio));

	case DRM_PANTHOR_DEV_QUERY_MMU_INFO:
		pw_device_get_mmu_info(dev, &u.mmu);
		return pw_set_uobj(os, q->pointer, q->size, PW_MIN_MMU_INFO, &u.mmu, sizeof(u.mmu));

	default:
		return PW_EINVAL;
	}
}

/* drv.c:1518. Informational only: pw_file_mmap_lookup() accepts both offsets regardless. */
static int pw_ioctl_set_user_mmio_offset(struct pw_file *file, void *arg)
{
	struct drm_panthor_set_user_mmio_offset *args = arg;

	if (args->offset != DRM_PANTHOR_USER_MMIO_OFFSET_32BIT &&
	    args->offset != DRM_PANTHOR_USER_MMIO_OFFSET_64BIT)
		return PW_EINVAL;
	__atomic_store_n(&file->user_mmio_offset, args->offset, __ATOMIC_RELAXED);
	return PW_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* dispatcher                                                                                 */
/* ------------------------------------------------------------------------------------------ */

/* Every struct a handler can see; the zero-extension buffer must hold the largest. */
union pw_ioctl_arg {
	uint64_t align;
	struct drm_version version;
	struct drm_get_cap get_cap;
	struct drm_gem_close gem_close;
	struct drm_syncobj_create syncobj_create;
	struct drm_syncobj_destroy syncobj_destroy;
	struct drm_syncobj_wait syncobj_wait;
	struct drm_syncobj_timeline_wait syncobj_timeline_wait;
	struct drm_syncobj_array syncobj_array;
	struct drm_syncobj_timeline_array syncobj_timeline_array;
	struct drm_syncobj_transfer syncobj_transfer;
	struct drm_panthor_dev_query dev_query;
	struct drm_panthor_vm_create vm_create;
	struct drm_panthor_vm_destroy vm_destroy;
	struct drm_panthor_vm_bind vm_bind;
	struct drm_panthor_vm_get_state vm_get_state;
	struct drm_panthor_bo_create bo_create;
	struct drm_panthor_bo_mmap_offset bo_mmap_offset;
	struct drm_panthor_group_create group_create;
	struct drm_panthor_group_destroy group_destroy;
	struct drm_panthor_group_submit group_submit;
	struct drm_panthor_group_get_state group_get_state;
	struct drm_panthor_tiler_heap_create tiler_heap_create;
	struct drm_panthor_tiler_heap_destroy tiler_heap_destroy;
	struct drm_panthor_bo_set_label bo_set_label;
	struct drm_panthor_set_user_mmio_offset set_user_mmio_offset;
	struct drm_panthor_bo_sync bo_sync;
	struct drm_panthor_bo_query_info bo_query_info;
	/* (jm-arch) panfrost */
	struct drm_panfrost_submit pf_submit;
	struct drm_panfrost_wait_bo pf_wait_bo;
	struct drm_panfrost_create_bo pf_create_bo;
	struct drm_panfrost_mmap_bo pf_mmap_bo;
	struct drm_panfrost_get_param pf_get_param;
	struct drm_panfrost_get_bo_offset pf_get_bo_offset;
	struct drm_panfrost_madvise pf_madvise;
	struct drm_panfrost_set_label_bo pf_set_label_bo;
	struct drm_panfrost_jm_ctx_create pf_jm_ctx_create;
	struct drm_panfrost_jm_ctx_destroy pf_jm_ctx_destroy;
	struct drm_panfrost_sync_bo pf_sync_bo;
	struct drm_panfrost_query_bo_info pf_query_bo_info;
};

struct pw_ioctl_desc {
	uint8_t nr;		/* _IOC_NR */
	uint8_t min_minor;	/* emulated minor version (of this table's driver) that added it */
	uint16_t size;		/* sizeof the current uAPI struct */
	pw_ioctl_fn fn;		/* NULL: fail with @err */
	int err;
	uint32_t flags;		/* PW_IOCTL_F_* (core table only) */
};

/* DRM core ioctl that needs DRIVER_SYNCOBJ_TIMELINE (drm_syncobj.c returns -EOPNOTSUPP
 * without it): unavailable on JM, like on Linux panfrost. */
#define PW_IOCTL_F_TIMELINE	PW_BIT(0)

#define PW_NR(ioctl)		((uint8_t)((ioctl) & 0xff))
#define PW_IOCTL(ioctl, type, f, minor)	{ PW_NR(ioctl), minor, sizeof(type), f, 0, 0 }
#define PW_IOCTL_F(ioctl, type, f, fl)	{ PW_NR(ioctl), 0, sizeof(type), f, 0, fl }
#define PW_IOCTL_UNSUPPORTED(ioctl, e)	{ PW_NR(ioctl), 0, 0, NULL, e, 0 }

/*
 * SYNCOBJ_TRANSFER needs DRIVER_SYNCOBJ_TIMELINE, so Linux panfrost refuses it. On JM we accept
 * the binary-to-binary form (both points 0): it stands in for the HANDLE_TO_FD(EXPORT_SYNC_FILE)
 * + FD_TO_HANDLE(IMPORT_SYNC_FILE) round trip panvk uses to copy a fence between binary
 * syncobjs, since there is no sync_file on Windows. Timeline points stay EOPNOTSUPP and
 * DRM_CAP_SYNCOBJ_TIMELINE stays 0.
 */
static int ioctl_syncobj_transfer(struct pw_file *file, void *arg)
{
	const struct drm_syncobj_transfer *t = arg;

	if (file_is_jm(file) && (t->src_point || t->dst_point))
		return PW_EOPNOTSUPP;
	return pw_ioctl_syncobj_transfer(file, arg);
}

/* DRM core ioctls (nr 0x00-0x3f, 0xa0-0xcf), the same for both backends. */
static const struct pw_ioctl_desc pw_core_ioctls[] = {
	/* DRM core (render-node subset Mesa/libdrm use) */
	PW_IOCTL(DRM_IOCTL_VERSION, struct drm_version, pw_ioctl_version, 0),
	PW_IOCTL(DRM_IOCTL_GET_CAP, struct drm_get_cap, pw_ioctl_get_cap, 0),
	PW_IOCTL(DRM_IOCTL_GEM_CLOSE, struct drm_gem_close, pw_ioctl_gem_close, 0),
	PW_IOCTL(DRM_IOCTL_SYNCOBJ_CREATE, struct drm_syncobj_create, pw_ioctl_syncobj_create, 0),
	PW_IOCTL(DRM_IOCTL_SYNCOBJ_DESTROY, struct drm_syncobj_destroy, pw_ioctl_syncobj_destroy, 0),
	PW_IOCTL(DRM_IOCTL_SYNCOBJ_WAIT, struct drm_syncobj_wait, pw_ioctl_syncobj_wait, 0),
	PW_IOCTL_F(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, struct drm_syncobj_timeline_wait,
		   pw_ioctl_syncobj_timeline_wait, PW_IOCTL_F_TIMELINE),
	PW_IOCTL(DRM_IOCTL_SYNCOBJ_RESET, struct drm_syncobj_array, pw_ioctl_syncobj_reset, 0),
	PW_IOCTL(DRM_IOCTL_SYNCOBJ_SIGNAL, struct drm_syncobj_array, pw_ioctl_syncobj_signal, 0),
	PW_IOCTL_F(DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, struct drm_syncobj_timeline_array,
		   pw_ioctl_syncobj_timeline_signal, PW_IOCTL_F_TIMELINE),
	PW_IOCTL_F(DRM_IOCTL_SYNCOBJ_QUERY, struct drm_syncobj_timeline_array,
		   pw_ioctl_syncobj_query, PW_IOCTL_F_TIMELINE),
	PW_IOCTL(DRM_IOCTL_SYNCOBJ_TRANSFER, struct drm_syncobj_transfer,
		 ioctl_syncobj_transfer, 0),
	/* fd-based sharing doesn't exist on Windows (pw_sync.h) */
	PW_IOCTL_UNSUPPORTED(DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, PW_EOPNOTSUPP),
	PW_IOCTL_UNSUPPORTED(DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, PW_EOPNOTSUPP),
	PW_IOCTL_UNSUPPORTED(DRM_IOCTL_SYNCOBJ_EVENTFD, PW_EOPNOTSUPP),
	PW_IOCTL_UNSUPPORTED(DRM_IOCTL_PRIME_HANDLE_TO_FD, PW_EOPNOTSUPP),
	PW_IOCTL_UNSUPPORTED(DRM_IOCTL_PRIME_FD_TO_HANDLE, PW_EOPNOTSUPP),
};

/* panthor (drv.c:1650 panthor_drm_driver_ioctls): CSF driver ioctls. */
static const struct pw_ioctl_desc pw_panthor_ioctls[] = {
	PW_IOCTL(DRM_IOCTL_PANTHOR_DEV_QUERY, struct drm_panthor_dev_query, pw_ioctl_dev_query, 0),
	PW_IOCTL(DRM_IOCTL_PANTHOR_VM_CREATE, struct drm_panthor_vm_create, pw_ioctl_vm_create, 0),
	PW_IOCTL(DRM_IOCTL_PANTHOR_VM_DESTROY, struct drm_panthor_vm_destroy, pw_ioctl_vm_destroy, 0),
	PW_IOCTL(DRM_IOCTL_PANTHOR_VM_BIND, struct drm_panthor_vm_bind, pw_ioctl_vm_bind, 0),
	PW_IOCTL(DRM_IOCTL_PANTHOR_VM_GET_STATE, struct drm_panthor_vm_get_state,
		 pw_ioctl_vm_get_state, 0),
	PW_IOCTL(DRM_IOCTL_PANTHOR_BO_CREATE, struct drm_panthor_bo_create, pw_ioctl_bo_create, 0),
	PW_IOCTL(DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, struct drm_panthor_bo_mmap_offset,
		 pw_ioctl_bo_mmap_offset, 0),
	PW_IOCTL(DRM_IOCTL_PANTHOR_GROUP_CREATE, struct drm_panthor_group_create,
		 pw_ioctl_group_create, 0),
	PW_IOCTL(DRM_IOCTL_PANTHOR_GROUP_DESTROY, struct drm_panthor_group_destroy,
		 pw_ioctl_group_destroy, 0),
	PW_IOCTL(DRM_IOCTL_PANTHOR_GROUP_SUBMIT, struct drm_panthor_group_submit,
		 pw_ioctl_group_submit, 0),
	PW_IOCTL(DRM_IOCTL_PANTHOR_GROUP_GET_STATE, struct drm_panthor_group_get_state,
		 pw_ioctl_group_get_state, 0),
	PW_IOCTL(DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, struct drm_panthor_tiler_heap_create,
		 pw_ioctl_tiler_heap_create, 0),
	PW_IOCTL(DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY, struct drm_panthor_tiler_heap_destroy,
		 pw_ioctl_tiler_heap_destroy, 0),
	PW_IOCTL(DRM_IOCTL_PANTHOR_BO_SET_LABEL, struct drm_panthor_bo_set_label,
		 pw_ioctl_bo_set_label, 4),
	PW_IOCTL(DRM_IOCTL_PANTHOR_SET_USER_MMIO_OFFSET, struct drm_panthor_set_user_mmio_offset,
		 pw_ioctl_set_user_mmio_offset, 5),
	PW_IOCTL(DRM_IOCTL_PANTHOR_BO_SYNC, struct drm_panthor_bo_sync, pw_ioctl_bo_sync, 7),
	PW_IOCTL(DRM_IOCTL_PANTHOR_BO_QUERY_INFO, struct drm_panthor_bo_query_info,
		 pw_ioctl_bo_query_info, 7),
};

/*
 * (jm-arch) panfrost (panfrost_drv.c panfrost_drm_driver_ioctls): JM driver ioctls. The
 * minors are the panfrost versions that added each ioctl. PERFCNT_* are "unstable" ioctls
 * Linux only exposes with the unstable_ioctls module parameter; without it they fail with
 * -ENOSYS (panfrost_unstable_ioctl_check), which is what we return.
 */
static const struct pw_ioctl_desc pw_panfrost_ioctls[] = {
	PW_IOCTL(DRM_IOCTL_PANFROST_SUBMIT, struct drm_panfrost_submit, pw_jm_ioctl_submit, 0),
	PW_IOCTL(DRM_IOCTL_PANFROST_WAIT_BO, struct drm_panfrost_wait_bo, pw_jm_ioctl_wait_bo, 0),
	PW_IOCTL(DRM_IOCTL_PANFROST_CREATE_BO, struct drm_panfrost_create_bo,
		 pw_jm_ioctl_create_bo, 0),
	PW_IOCTL(DRM_IOCTL_PANFROST_MMAP_BO, struct drm_panfrost_mmap_bo, pw_jm_ioctl_mmap_bo, 0),
	PW_IOCTL(DRM_IOCTL_PANFROST_GET_PARAM, struct drm_panfrost_get_param,
		 pw_jm_ioctl_get_param, 0),
	PW_IOCTL(DRM_IOCTL_PANFROST_GET_BO_OFFSET, struct drm_panfrost_get_bo_offset,
		 pw_jm_ioctl_get_bo_offset, 0),
	PW_IOCTL_UNSUPPORTED(DRM_IOCTL_PANFROST_PERFCNT_ENABLE, PW_ENOSYS),
	PW_IOCTL_UNSUPPORTED(DRM_IOCTL_PANFROST_PERFCNT_DUMP, PW_ENOSYS),
	PW_IOCTL(DRM_IOCTL_PANFROST_MADVISE, struct drm_panfrost_madvise, pw_jm_ioctl_madvise, 0),
	PW_IOCTL(DRM_IOCTL_PANFROST_SET_LABEL_BO, struct drm_panfrost_set_label_bo,
		 pw_jm_ioctl_set_label_bo, 4),
	PW_IOCTL(DRM_IOCTL_PANFROST_JM_CTX_CREATE, struct drm_panfrost_jm_ctx_create,
		 pw_jm_ioctl_jm_ctx_create, 5),
	PW_IOCTL(DRM_IOCTL_PANFROST_JM_CTX_DESTROY, struct drm_panfrost_jm_ctx_destroy,
		 pw_jm_ioctl_jm_ctx_destroy, 5),
	PW_IOCTL(DRM_IOCTL_PANFROST_SYNC_BO, struct drm_panfrost_sync_bo, pw_jm_ioctl_sync_bo, 6),
	PW_IOCTL(DRM_IOCTL_PANFROST_QUERY_BO_INFO, struct drm_panfrost_query_bo_info,
		 pw_jm_ioctl_query_bo_info, 6),
};

static const struct pw_ioctl_desc *find_ioctl(const struct pw_ioctl_desc *tbl, size_t n,
					      uint32_t nr)
{
	for (size_t i = 0; i < n; i++) {
		if (tbl[i].nr == nr)
			return &tbl[i];
	}
	return NULL;
}

/*
 * Like drm_ioctl(): the user's struct size comes from the ioctl number. A smaller (older) struct
 * is zero-extended to the current size before the handler runs and only the user's bytes are
 * copied back (truncated); a larger (newer) one is passed through, the handler only touching the
 * fields it knows. So handlers always see at least the current struct.
 */
int pw_file_ioctl(struct pw_file *file, uint32_t nr, void *arg, uint32_t size)
{
	const struct pw_ioctl_desc *d = NULL;
	union pw_ioctl_arg k;
	uint32_t minor = PW_DRM_MINOR;
	bool jm;
	int ret;

	if (!file)
		return PW_ENOTTY;
	jm = file_is_jm(file);
	if (nr >= DRM_COMMAND_BASE && nr < DRM_COMMAND_END) {
		/* Driver ioctls: the table of this device's backend, gated on its version. */
		if (jm) {
			d = find_ioctl(pw_panfrost_ioctls, PW_ARRAY_SIZE(pw_panfrost_ioctls), nr);
			minor = PW_JM_DRM_MINOR;
		} else {
			d = find_ioctl(pw_panthor_ioctls, PW_ARRAY_SIZE(pw_panthor_ioctls), nr);
		}
	} else {
		d = find_ioctl(pw_core_ioctls, PW_ARRAY_SIZE(pw_core_ioctls), nr);
	}
	if (!d || d->min_minor > minor)
		return PW_ENOTTY;
	if (jm && (d->flags & PW_IOCTL_F_TIMELINE))
		return PW_EOPNOTSUPP;
	if (!d->fn)
		return d->err;
	if (size && !arg)
		return PW_EFAULT;
	if (size >= d->size)
		return d->fn(file, arg);

	pw_memset(&k, 0, sizeof(k));
	if (size)
		pw_memcpy(&k, arg, size);
	ret = d->fn(file, &k);
	if (size)
		pw_memcpy(arg, &k, size);
	return ret;
}

/* ------------------------------------------------------------------------------------------ */
/* mmap                                                                                       */
/* ------------------------------------------------------------------------------------------ */

/*
 * drv.c:1672 panthor_mmap + device.c:445 panthor_device_mmap_io. Offsets from 1 << 43 up are
 * the user-MMIO window; the only page in it is LATEST_FLUSH_ID, at DRM_PANTHOR_USER_MMIO_OFFSET
 * of either width (we accept both, whatever SET_USER_MMIO_OFFSET chose): one page, read-only,
 * Device memory. BO offsets are always below 1 << 43 (pw_bo.c).
 */
int pw_file_mmap_lookup(struct pw_file *file, uint64_t offset, uint64_t size,
			struct pw_mmap_target *t)
{
	uint64_t rel;
	struct pw_bo *bo;

	pw_memset(t, 0, sizeof(*t));
	if (!size || (offset & (PW_SZ_4K - 1)) || (size & (PW_SZ_4K - 1)))
		return PW_EINVAL;

	/* (jm-arch) panfrost maps no register page to userspace: BO offsets only. */
	if (offset >= DRM_PANTHOR_USER_MMIO_OFFSET_32BIT && file_is_jm(file))
		return PW_EINVAL;
	if (offset >= DRM_PANTHOR_USER_MMIO_OFFSET_32BIT) {
		if (offset != DRM_PANTHOR_USER_MMIO_OFFSET_32BIT &&
		    offset != DRM_PANTHOR_USER_MMIO_OFFSET_64BIT)
			return PW_EINVAL;
		if (size != PW_SZ_4K)
			return PW_EINVAL;
		t->kind = PW_MMAP_MMIO;
		t->mmio_offset = CSF_GPU_LATEST_FLUSH_ID;	/* fw_regs.h:24, device.c:419 */
		t->read_only = true;
		t->cached = false;
		return PW_OK;
	}

	bo = pw_file_bo_from_mmap_offset(file, offset, &rel);
	if (!bo)
		return PW_EINVAL;
	if (size > bo->size - rel) {
		pw_bo_put(bo);
		return PW_EINVAL;
	}
	t->kind = PW_MMAP_BO;
	t->bo = bo;
	t->offset_in_obj = rel;
	t->read_only = false;
	t->cached = pw_bo_cpu_cached(bo);
	return PW_OK;
}
