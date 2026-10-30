/* SPDX-License-Identifier: MIT */
/*
 * In-process fake panwin.sys, for tests and for running panwin-info without hardware
 * (-Dfake_kernel=true, then PANWIN_FAKE_KERNEL=1 or panwin_use_fake_kernel(1)).
 *
 * It answers the forwarded DRM ioctls with panthor semantics closely enough to exercise
 * libdrm-win and its callers: VERSION ("panthor" 1.5.0), GET_CAP, SET_CLIENT_CAP, DEV_QUERY
 * (GPU_INFO/CSIF_INFO/TIMESTAMP_INFO/GROUP_PRIORITIES_INFO with plausible RK3588 G610 values -
 * NOT read from hardware), syncobjs (binary + timeline: create/destroy/wait/timeline-wait/reset/
 * signal/timeline-signal/query/transfer), BO_CREATE/BO_MMAP_OFFSET/GEM_CLOSE + MMAP/MUNMAP backed
 * by pagefile sections, the user flush-id page, VM_CREATE/VM_DESTROY, and the private GET_INFO,
 * SYNCOBJ_EVENT and DEBUG_REG_READ ioctls. There is no GPU: every fence is "signalled at submit".
 *
 * ABI v2 (include/panwin/panwin_ioctl.h): errors are returned the way panwin.sys reports them,
 * PANWIN_STATUS_FROM_ERRNO(linux errno) as the request's NTSTATUS, and libdrm-win decodes them on
 * the same path as for the real transport. SYNCOBJ_EVENT references the caller-created event
 * (here: DuplicateHandle with EVENT_MODIFY_STATE, as ObReferenceObjectByHandle checks it).
 * MUNMAP releases whole mappings only (user_va and size must match), like kmd/win.
 *
 * Timeouts are absolute nanoseconds on the QueryPerformanceCounter clock, which is what Mesa's
 * os_time_get_nano() returns on Windows (CLOCK_MONOTONIC on Linux).
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "drm-uapi/drm.h"
#include "drm-uapi/panthor_drm.h"
#include "panwin/panwin_ioctl.h"
#include "libdrm_win.h"
#include "pw_internal.h"

/* Linux errno numbers (what the OS-neutral core returns, see pw_os.h). */
#define LX_ENOENT 2
#define LX_EBADF 9
#define LX_EFAULT 14
#define LX_EBUSY 16
#define LX_EINVAL 22
#define LX_ENOMEM 12
#define LX_ENOTTY 25
#define LX_ETIME 62
#define LX_EOPNOTSUPP 95

#define FAKE_DEVICES 1
/* arch 10.8 (rev 6), product major 7 = G610, r0p0, status 5: panthor on RK3588 logs
 * "mali-g610 id 0xa867 major 0x0 minor 0x0 status 0x5". */
#define FAKE_GPU_ID 0xa8670005u
#define FAKE_FLUSH_ID 0x00c0ffeeu

/* ---- objects ---- */

struct fk_event {
	struct fk_event *next;
	uint64_t point;
	HANDLE ev;
};

struct fk_syncobj {
	bool used;
	bool has_fence;   /* binary payload / timeline point 0 */
	uint64_t payload; /* last signalled timeline point */
	struct fk_event *events;
};

struct fk_bo {
	bool used;
	uint64_t size;
	HANDLE section;
};

enum fk_map_kind { FK_MAP_VIEW, FK_MAP_FLUSH_ID };

struct fk_map {
	void *va;
	uint64_t size;
	enum fk_map_kind kind;
};

struct fk_file {
	int index;
	uint64_t user_mmio_offset;
	struct fk_syncobj *syncobjs;   /* index = handle - 1 */
	uint32_t nsyncobjs;
	struct fk_bo *bos;             /* index = handle - 1 */
	uint32_t nbos;
	uint32_t vm_mask;              /* bit i = VM id i+1 */
	struct fk_map *maps;
	uint32_t nmaps, capmaps;
};

/* One lock + condvar for the whole fake kernel: simple and good enough for tests. */
static SRWLOCK fk_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE fk_cv = CONDITION_VARIABLE_INIT;

/* ---- helpers ---- */

static int64_t fk_now_ns(void)
{
	LARGE_INTEGER c, f;
	QueryPerformanceCounter(&c);
	QueryPerformanceFrequency(&f);
	return (int64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}

static struct fk_syncobj *so_get(struct fk_file *f, uint32_t h)
{
	if (h == 0 || h > f->nsyncobjs || !f->syncobjs[h - 1].used)
		return NULL;
	return &f->syncobjs[h - 1];
}

static bool so_reached(const struct fk_syncobj *s, uint64_t point)
{
	return point == 0 ? s->has_fence : s->payload >= point;
}

/* Fire one-shot events whose condition is met. Caller holds fk_lock exclusively. */
static void so_kick(struct fk_syncobj *s)
{
	struct fk_event **pp = &s->events;
	while (*pp) {
		struct fk_event *e = *pp;
		if (so_reached(s, e->point)) {
			SetEvent(e->ev);
			CloseHandle(e->ev);
			*pp = e->next;
			free(e);
		} else {
			pp = &e->next;
		}
	}
	WakeAllConditionVariable(&fk_cv);
}

static void so_signal(struct fk_syncobj *s, uint64_t point)
{
	s->has_fence = true;
	if (point > s->payload)
		s->payload = point;
	so_kick(s);
}

static void so_free_events(struct fk_syncobj *s)
{
	while (s->events) {
		struct fk_event *e = s->events;
		s->events = e->next;
		CloseHandle(e->ev);
		free(e);
	}
}

/* ---- DRM core ioctls ---- */

static void copy_field(size_t *len, char *dst, const char *src)
{
	size_t l = strlen(src);
	if (dst && *len)
		memcpy(dst, src, *len < l ? *len : l);
	*len = l;
}

static int fk_version(struct fk_file *f, void *arg)
{
	struct drm_version *v = arg;
	(void)f;
	v->version_major = 1;
	v->version_minor = 5;
	v->version_patchlevel = 0;
	copy_field(&v->name_len, v->name, "panthor");
	copy_field(&v->date_len, v->date, "20230801");
	copy_field(&v->desc_len, v->desc, "Panthor DRM driver (panwin fake kernel)");
	return 0;
}

static int fk_get_cap(struct fk_file *f, void *arg)
{
	struct drm_get_cap *c = arg;
	(void)f;
	switch (c->capability) {
	case DRM_CAP_SYNCOBJ:
	case DRM_CAP_SYNCOBJ_TIMELINE:
		c->value = 1;
		return 0;
	case DRM_CAP_PRIME:
		c->value = 0; /* no dma-buf on Windows */
		return 0;
	default:
		return -LX_EINVAL;
	}
}

static int fk_set_client_cap(struct fk_file *f, void *arg)
{
	(void)f;
	(void)arg;
	return 0;
}

static int fk_syncobj_create(struct fk_file *f, void *arg)
{
	struct drm_syncobj_create *a = arg;
	uint32_t i;

	if (a->flags & ~DRM_SYNCOBJ_CREATE_SIGNALED)
		return -LX_EINVAL;
	for (i = 0; i < f->nsyncobjs && f->syncobjs[i].used; i++)
		;
	if (i == f->nsyncobjs) {
		uint32_t n = f->nsyncobjs ? f->nsyncobjs * 2 : 16;
		struct fk_syncobj *ns = realloc(f->syncobjs, n * sizeof(*ns));
		if (!ns)
			return -LX_ENOMEM;
		memset(ns + f->nsyncobjs, 0, (n - f->nsyncobjs) * sizeof(*ns));
		f->syncobjs = ns;
		f->nsyncobjs = n;
	}
	memset(&f->syncobjs[i], 0, sizeof(f->syncobjs[i]));
	f->syncobjs[i].used = true;
	f->syncobjs[i].has_fence = !!(a->flags & DRM_SYNCOBJ_CREATE_SIGNALED);
	a->handle = i + 1;
	return 0;
}

static int fk_syncobj_destroy(struct fk_file *f, void *arg)
{
	struct drm_syncobj_destroy *a = arg;
	struct fk_syncobj *s = so_get(f, a->handle);
	if (!s || a->pad)
		return -LX_EINVAL;
	so_free_events(s);
	s->used = false;
	WakeAllConditionVariable(&fk_cv);
	return 0;
}

/* Shared by WAIT and TIMELINE_WAIT. Called with fk_lock held exclusively; may sleep on fk_cv. */
static int fk_wait_common(struct fk_file *f, const uint32_t *handles, const uint64_t *points,
			  uint32_t count, int64_t timeout_ns, uint32_t flags, uint32_t *first)
{
	const uint32_t known = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
			       DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;
	bool all = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;

	if (flags & ~known)
		return -LX_EINVAL;
	if (count == 0)
		return -LX_EINVAL;
	if (!handles)
		return -LX_EFAULT;

	for (;;) {
		uint32_t done = 0, first_idx = UINT32_MAX;

		for (uint32_t i = 0; i < count; i++) {
			struct fk_syncobj *s = so_get(f, handles[i]);
			uint64_t pt = points ? points[i] : 0;
			if (!s)
				return -LX_ENOENT;
			if (so_reached(s, pt)) {
				done++;
				if (first_idx == UINT32_MAX)
					first_idx = i;
			} else if (!(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)) {
				/* No fence (for that point) and not asked to wait for one: DRM says EINVAL. */
				return -LX_EINVAL;
			}
		}
		if (all ? done == count : done > 0) {
			if (first)
				*first = all ? 0 : first_idx;
			return 0;
		}

		int64_t now = fk_now_ns();
		if (timeout_ns <= now)
			return -LX_ETIME;
		int64_t ms = (timeout_ns - now + 999999) / 1000000;
		DWORD w = (timeout_ns == INT64_MAX || ms > 0x7fffffff) ? INFINITE : (DWORD)ms;
		SleepConditionVariableSRW(&fk_cv, &fk_lock, w, 0);
	}
}

static int fk_syncobj_wait(struct fk_file *f, void *arg)
{
	struct drm_syncobj_wait *a = arg;
	if (a->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE)
		return -LX_EINVAL; /* timeline-only flag */
	return fk_wait_common(f, (const uint32_t *)(uintptr_t)a->handles, NULL, a->count_handles,
			      a->timeout_nsec, a->flags, &a->first_signaled);
}

static int fk_syncobj_timeline_wait(struct fk_file *f, void *arg)
{
	struct drm_syncobj_timeline_wait *a = arg;
	if (!a->points)
		return -LX_EFAULT;
	return fk_wait_common(f, (const uint32_t *)(uintptr_t)a->handles,
			      (const uint64_t *)(uintptr_t)a->points, a->count_handles, a->timeout_nsec,
			      a->flags, &a->first_signaled);
}

static int fk_syncobj_array_check(struct fk_file *f, const uint32_t *h, uint32_t n)
{
	if (n == 0)
		return -LX_EINVAL;
	if (!h)
		return -LX_EFAULT;
	for (uint32_t i = 0; i < n; i++)
		if (!so_get(f, h[i]))
			return -LX_ENOENT;
	return 0;
}

static int fk_syncobj_reset(struct fk_file *f, void *arg)
{
	struct drm_syncobj_array *a = arg;
	const uint32_t *h = (const uint32_t *)(uintptr_t)a->handles;
	int r = fk_syncobj_array_check(f, h, a->count_handles);
	if (r || a->pad)
		return r ? r : -LX_EINVAL;
	for (uint32_t i = 0; i < a->count_handles; i++) {
		struct fk_syncobj *s = so_get(f, h[i]);
		s->has_fence = false;
		s->payload = 0;
	}
	return 0;
}

static int fk_syncobj_signal(struct fk_file *f, void *arg)
{
	struct drm_syncobj_array *a = arg;
	const uint32_t *h = (const uint32_t *)(uintptr_t)a->handles;
	int r = fk_syncobj_array_check(f, h, a->count_handles);
	if (r || a->pad)
		return r ? r : -LX_EINVAL;
	for (uint32_t i = 0; i < a->count_handles; i++)
		so_signal(so_get(f, h[i]), 0);
	return 0;
}

static int fk_syncobj_timeline_signal(struct fk_file *f, void *arg)
{
	struct drm_syncobj_timeline_array *a = arg;
	const uint32_t *h = (const uint32_t *)(uintptr_t)a->handles;
	const uint64_t *p = (const uint64_t *)(uintptr_t)a->points;
	int r = fk_syncobj_array_check(f, h, a->count_handles);
	if (r || a->flags)
		return r ? r : -LX_EINVAL;
	if (!p)
		return -LX_EFAULT;
	for (uint32_t i = 0; i < a->count_handles; i++)
		so_signal(so_get(f, h[i]), p[i]);
	return 0;
}

static int fk_syncobj_query(struct fk_file *f, void *arg)
{
	struct drm_syncobj_timeline_array *a = arg;
	const uint32_t *h = (const uint32_t *)(uintptr_t)a->handles;
	uint64_t *p = (uint64_t *)(uintptr_t)a->points;
	int r = fk_syncobj_array_check(f, h, a->count_handles);
	if (r)
		return r;
	if (a->flags & ~DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED)
		return -LX_EINVAL;
	if (!p)
		return -LX_EFAULT;
	/* No GPU: last submitted == last signalled. */
	for (uint32_t i = 0; i < a->count_handles; i++)
		p[i] = so_get(f, h[i])->payload;
	return 0;
}

static int fk_syncobj_transfer(struct fk_file *f, void *arg)
{
	struct drm_syncobj_transfer *a = arg;
	struct fk_syncobj *src = so_get(f, a->src_handle), *dst = so_get(f, a->dst_handle);
	if (!src || !dst)
		return -LX_ENOENT;
	if (a->flags & ~DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)
		return -LX_EINVAL;
	if (!so_reached(src, a->src_point))
		return -LX_EINVAL; /* no fence for that point (a real kernel would chain a pending one) */
	so_signal(dst, a->dst_point);
	return 0;
}

static int fk_gem_close(struct fk_file *f, void *arg)
{
	struct drm_gem_close *a = arg;
	if (a->handle == 0 || a->handle > f->nbos || !f->bos[a->handle - 1].used)
		return -LX_EINVAL;
	struct fk_bo *bo = &f->bos[a->handle - 1];
	CloseHandle(bo->section); /* live views keep the section alive */
	memset(bo, 0, sizeof(*bo));
	return 0;
}

/* ---- panthor ioctls ---- */

static void fake_gpu_info(struct drm_panthor_gpu_info *g)
{
	/* Plausible values for an RK3588 Mali-G610 MP4; NOT measured on hardware. */
	memset(g, 0, sizeof(*g));
	g->gpu_id = FAKE_GPU_ID;
	g->gpu_rev = 0;
	g->csf_id = 0x04a00412;          /* CSHW 1.10.0, MCU 1.1.2 (illustrative) */
	g->l2_features = 0x07120206;     /* 64-byte lines, 2^18 = 256 KiB (illustrative) */
	g->tiler_features = 0x00000809;
	g->mem_features = 0x00000001;
	g->mmu_features = 0x00002830;    /* VA 48 bits, PA 40 bits */
	g->thread_features = 0x04000000;
	g->max_threads = 2048;
	g->thread_max_workgroup_size = 1024;
	g->thread_max_barrier_size = 1024;
	g->coherency_features = (1u << DRM_PANTHOR_GPU_COHERENCY_ACE_LITE) |
				(1u << DRM_PANTHOR_GPU_COHERENCY_ACE);
	g->texture_features[0] = 0xc1ffff9e;
	g->as_present = 0xff;
	g->selected_coherency = DRM_PANTHOR_GPU_COHERENCY_NONE;
	g->shader_present = 0x50005;     /* 4 cores, RK3588 layout */
	g->l2_present = 0x1;
	g->tiler_present = 0x1;
}

static int fk_dev_query(struct fk_file *f, void *arg)
{
	struct drm_panthor_dev_query *q = arg;
	union {
		struct drm_panthor_gpu_info gpu;
		struct drm_panthor_csif_info csif;
		struct drm_panthor_timestamp_info ts;
		struct drm_panthor_group_priorities_info prio;
	} u;
	size_t sz;

	(void)f;
	memset(&u, 0, sizeof(u));
	switch (q->type) {
	case DRM_PANTHOR_DEV_QUERY_GPU_INFO:
		fake_gpu_info(&u.gpu);
		sz = sizeof(u.gpu);
		break;
	case DRM_PANTHOR_DEV_QUERY_CSIF_INFO:
		u.csif.csg_slot_count = 8;
		u.csif.cs_slot_count = 8;
		u.csif.cs_reg_count = 96;
		u.csif.scoreboard_slot_count = 8;
		u.csif.unpreserved_cs_reg_count = 4;
		sz = sizeof(u.csif);
		break;
	case DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO: {
		LARGE_INTEGER c, fq;
		QueryPerformanceCounter(&c);
		QueryPerformanceFrequency(&fq);
		u.ts.timestamp_frequency = 24000000; /* RK3588 system counter */
		u.ts.current_timestamp = (uint64_t)((double)c.QuadPart * 24e6 / (double)fq.QuadPart);
		sz = sizeof(u.ts);
		break;
	}
	case DRM_PANTHOR_DEV_QUERY_GROUP_PRIORITIES_INFO:
		u.prio.allowed_mask = 0x3; /* LOW | MEDIUM for unprivileged clients */
		sz = sizeof(u.prio);
		break;
	default:
		return -LX_EINVAL; /* e.g. MMU_INFO needs uAPI 1.9 */
	}

	if (!q->pointer) {
		q->size = (uint32_t)sz;
		return 0;
	}
	/* Like panthor: copy min(size, actual) and zero-extend if userspace's struct is larger. */
	void *dst = (void *)(uintptr_t)q->pointer;
	if (q->size > sz)
		memset((char *)dst + sz, 0, q->size - sz);
	memcpy(dst, &u, q->size < sz ? q->size : sz);
	q->size = (uint32_t)(q->size < sz ? q->size : sz);
	return 0;
}

static int fk_vm_create(struct fk_file *f, void *arg)
{
	struct drm_panthor_vm_create *a = arg;
	if (a->flags)
		return -LX_EINVAL;
	for (uint32_t i = 0; i < 32; i++) {
		if (!(f->vm_mask & (1u << i))) {
			f->vm_mask |= 1u << i;
			a->id = i + 1;
			return 0;
		}
	}
	return -LX_EBUSY;
}

static int fk_vm_destroy(struct fk_file *f, void *arg)
{
	struct drm_panthor_vm_destroy *a = arg;
	if (a->id == 0 || a->id > 32 || !(f->vm_mask & (1u << (a->id - 1))) || a->pad)
		return -LX_EINVAL;
	f->vm_mask &= ~(1u << (a->id - 1));
	return 0;
}

static int fk_bo_create(struct fk_file *f, void *arg)
{
	struct drm_panthor_bo_create *a = arg;
	uint32_t i;

	if (a->size == 0 || a->pad)
		return -LX_EINVAL;
	if (a->exclusive_vm_id && (a->exclusive_vm_id > 32 || !(f->vm_mask & (1u << (a->exclusive_vm_id - 1)))))
		return -LX_EINVAL;
	a->size = (a->size + 4095) & ~4095ull;
	for (i = 0; i < f->nbos && f->bos[i].used; i++)
		;
	if (i == f->nbos) {
		uint32_t n = f->nbos ? f->nbos * 2 : 16;
		struct fk_bo *nb = realloc(f->bos, n * sizeof(*nb));
		if (!nb)
			return -LX_ENOMEM;
		memset(nb + f->nbos, 0, (n - f->nbos) * sizeof(*nb));
		f->bos = nb;
		f->nbos = n;
	}
	HANDLE s = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
				      (DWORD)(a->size >> 32), (DWORD)a->size, NULL);
	if (!s)
		return -LX_ENOMEM;
	f->bos[i].used = true;
	f->bos[i].size = a->size;
	f->bos[i].section = s;
	a->handle = i + 1;
	return 0;
}

/* Fake mmap offsets: handle << 16 (page aligned, far below the 1<<43 user MMIO offset). */
static int fk_bo_mmap_offset(struct fk_file *f, void *arg)
{
	struct drm_panthor_bo_mmap_offset *a = arg;
	if (a->handle == 0 || a->handle > f->nbos || !f->bos[a->handle - 1].used || a->pad)
		return -LX_ENOENT;
	a->offset = (uint64_t)a->handle << 16;
	return 0;
}

static int fk_set_user_mmio_offset(struct fk_file *f, void *arg)
{
	struct drm_panthor_set_user_mmio_offset *a = arg;
	if (a->offset != DRM_PANTHOR_USER_MMIO_OFFSET_32BIT &&
	    a->offset != DRM_PANTHOR_USER_MMIO_OFFSET_64BIT)
		return -LX_EINVAL;
	f->user_mmio_offset = a->offset;
	return 0;
}

/* ---- private ioctls ---- */

static int fk_get_info(struct fk_file *f, void *arg)
{
	struct panwin_info *i = arg;
	memset(i, 0, sizeof(*i));
	i->abi_version = PANWIN_ABI_VERSION;
	i->flags = PANWIN_INFO_FW_BOOTED | PANWIN_INFO_DEBUG_BUILD;
	i->drm_version_major = 1;
	i->drm_version_minor = 5;
	i->gpu_phys_base = 0xfb000000;
	snprintf(i->description, sizeof(i->description),
		 "fake kernel #%d: G610 r0p0 arch10.8, no hardware", f->index);
	return 0;
}

static int fk_map_add(struct fk_file *f, void *va, uint64_t size, enum fk_map_kind kind)
{
	if (f->nmaps == f->capmaps) {
		uint32_t n = f->capmaps ? f->capmaps * 2 : 16;
		struct fk_map *nm = realloc(f->maps, n * sizeof(*nm));
		if (!nm)
			return -LX_ENOMEM;
		f->maps = nm;
		f->capmaps = n;
	}
	f->maps[f->nmaps].va = va;
	f->maps[f->nmaps].size = size;
	f->maps[f->nmaps].kind = kind;
	f->nmaps++;
	return 0;
}

static void fk_map_release(struct fk_map *m)
{
	if (m->kind == FK_MAP_VIEW)
		UnmapViewOfFile(m->va);
	else
		VirtualFree(m->va, 0, MEM_RELEASE);
}

static int fk_mmap(struct fk_file *f, void *arg)
{
	struct panwin_mmap *m = arg;
	uint64_t mmio = f->user_mmio_offset;
	enum fk_map_kind kind;
	void *va;
	int r;

	if (!m->prot || (m->prot & ~(PANWIN_PROT_READ | PANWIN_PROT_WRITE)) || (m->size & 4095) ||
	    !m->size || m->pad)
		return -LX_EINVAL;

	if (m->offset == mmio || m->offset == DRM_PANTHOR_USER_MMIO_OFFSET_32BIT ||
	    m->offset == DRM_PANTHOR_USER_MMIO_OFFSET_64BIT) {
		/* User MMIO page: LATEST_FLUSH_ID at offset 0. Read-only like panthor. */
		if (m->size != 4096 || (m->prot & PANWIN_PROT_WRITE))
			return -LX_EINVAL;
		va = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!va)
			return -LX_ENOMEM;
		*(volatile uint32_t *)va = FAKE_FLUSH_ID;
		DWORD old;
		VirtualProtect(va, 4096, PAGE_READONLY, &old);
		kind = FK_MAP_FLUSH_ID;
	} else {
		uint64_t h = m->offset >> 16;
		if ((m->offset & 0xffffull) || h == 0 || h > f->nbos || !f->bos[h - 1].used)
			return -LX_EINVAL;
		if (m->size > f->bos[h - 1].size)
			return -LX_EINVAL;
		DWORD acc = (m->prot & PANWIN_PROT_WRITE) ? FILE_MAP_WRITE : FILE_MAP_READ;
		va = MapViewOfFile(f->bos[h - 1].section, acc, 0, 0, (SIZE_T)m->size);
		if (!va)
			return -LX_ENOMEM;
		kind = FK_MAP_VIEW;
	}
	r = fk_map_add(f, va, m->size, kind);
	if (r) {
		struct fk_map tmp = { va, m->size, kind };
		fk_map_release(&tmp);
		return r;
	}
	m->user_va = (uint64_t)(uintptr_t)va;
	return 0;
}

static int fk_munmap(struct fk_file *f, void *arg)
{
	struct panwin_munmap *m = arg;
	for (uint32_t i = 0; i < f->nmaps; i++) {
		if ((uint64_t)(uintptr_t)f->maps[i].va == m->user_va && f->maps[i].size == m->size) {
			fk_map_release(&f->maps[i]);
			f->maps[i] = f->maps[--f->nmaps];
			return 0;
		}
	}
	return -LX_EINVAL;
}

static int fk_syncobj_event(struct fk_file *f, void *arg)
{
	struct panwin_syncobj_event *a = arg;
	struct fk_syncobj *s = so_get(f, a->handle);
	HANDLE mine;

	if (!s)
		return -LX_EINVAL; /* drm_syncobj_find: no such syncobj */
	if (a->flags & ~(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE))
		return -LX_EINVAL;
	/* No GPU: "has a fence for point" == "point signalled". */
	if (!so_reached(s, a->point) && !(a->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT))
		return -LX_EINVAL; /* no fence and not asked to wait for one */
	/* The caller keeps its handle; we keep our own reference to the event object. */
	if (!a->event_handle ||
	    !DuplicateHandle(GetCurrentProcess(), (HANDLE)(uintptr_t)a->event_handle,
			     GetCurrentProcess(), &mine, EVENT_MODIFY_STATE, FALSE, 0))
		return -LX_EBADF;
	struct fk_event *e = calloc(1, sizeof(*e));
	if (!e) {
		CloseHandle(mine);
		return -LX_ENOMEM;
	}
	e->point = a->point;
	e->ev = mine;
	e->next = s->events;
	s->events = e;
	so_kick(s);
	return 0;
}

static int fk_debug_reg_read(struct fk_file *f, void *arg)
{
	struct panwin_debug_reg_read *a = arg;
	struct drm_panthor_gpu_info g;
	(void)f;
	fake_gpu_info(&g);
	switch (a->offset) {
	case 0x000: a->value = g.gpu_id; break;
	case 0x004: a->value = g.l2_features; break;
	case 0x00C: a->value = g.tiler_features; break;
	case 0x010: a->value = g.mem_features; break;
	case 0x014: a->value = g.mmu_features; break;
	case 0x018: a->value = g.as_present; break;
	case 0x01C: a->value = g.csf_id; break;
	case 0x0A0: a->value = g.max_threads; break;
	case 0x100: a->value = (uint32_t)g.shader_present; break;
	case 0x104: a->value = (uint32_t)(g.shader_present >> 32); break;
	case 0x110: a->value = (uint32_t)g.tiler_present; break;
	case 0x120: a->value = (uint32_t)g.l2_present; break;
	case 0x10000: a->value = FAKE_FLUSH_ID; break; /* CSF_GPU_LATEST_FLUSH_ID */
	default:
		if (a->offset & 3)
			return -LX_EINVAL;
		a->value = 0;
		break;
	}
	return 0;
}

/* ---- dispatch ---- */

typedef int (*fk_handler)(struct fk_file *f, void *arg);

struct fk_entry {
	unsigned long ioctl;
	fk_handler fn;
};

static const struct fk_entry drm_table[] = {
	{ DRM_IOCTL_VERSION, fk_version },
	{ DRM_IOCTL_GET_CAP, fk_get_cap },
	{ DRM_IOCTL_SET_CLIENT_CAP, fk_set_client_cap },
	{ DRM_IOCTL_GEM_CLOSE, fk_gem_close },
	{ DRM_IOCTL_SYNCOBJ_CREATE, fk_syncobj_create },
	{ DRM_IOCTL_SYNCOBJ_DESTROY, fk_syncobj_destroy },
	{ DRM_IOCTL_SYNCOBJ_WAIT, fk_syncobj_wait },
	{ DRM_IOCTL_SYNCOBJ_RESET, fk_syncobj_reset },
	{ DRM_IOCTL_SYNCOBJ_SIGNAL, fk_syncobj_signal },
	{ DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, fk_syncobj_timeline_wait },
	{ DRM_IOCTL_SYNCOBJ_QUERY, fk_syncobj_query },
	{ DRM_IOCTL_SYNCOBJ_TRANSFER, fk_syncobj_transfer },
	{ DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, fk_syncobj_timeline_signal },
	{ DRM_IOCTL_PANTHOR_DEV_QUERY, fk_dev_query },
	{ DRM_IOCTL_PANTHOR_VM_CREATE, fk_vm_create },
	{ DRM_IOCTL_PANTHOR_VM_DESTROY, fk_vm_destroy },
	{ DRM_IOCTL_PANTHOR_BO_CREATE, fk_bo_create },
	{ DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, fk_bo_mmap_offset },
	{ DRM_IOCTL_PANTHOR_SET_USER_MMIO_OFFSET, fk_set_user_mmio_offset },
};

static const struct {
	uint32_t code;
	uint32_t size;
	fk_handler fn;
} priv_table[] = {
	{ PANWIN_IOCTL_GET_INFO, sizeof(struct panwin_info), fk_get_info },
	{ PANWIN_IOCTL_MMAP, sizeof(struct panwin_mmap), fk_mmap },
	{ PANWIN_IOCTL_MUNMAP, sizeof(struct panwin_munmap), fk_munmap },
	{ PANWIN_IOCTL_SYNCOBJ_EVENT, sizeof(struct panwin_syncobj_event), fk_syncobj_event },
	{ PANWIN_IOCTL_DEBUG_REG_READ, sizeof(struct panwin_debug_reg_read), fk_debug_reg_read },
};

/* The NTSTATUS panwin.sys completes the request with (ABI v2). */
static int32_t lx_to_result(int lx)
{
	return lx ? PANWIN_STATUS_FROM_ERRNO(-lx) : 0;
}

static int32_t fake_ioctl(void *file, uint32_t code, void *buf, uint32_t in_size,
			  uint32_t out_size, uint32_t *returned)
{
	struct fk_file *f = file;
	fk_handler fn = NULL;
	uint32_t want = 0;
	int r;

	if ((code >> 16) != PANWIN_FILE_DEVICE || (code & 3) != PANWIN_METHOD_BUFFERED)
		return lx_to_result(-LX_ENOTTY);

	uint32_t func = (code >> 2) & 0xFFF;
	if (func >= PANWIN_FUNC_DRM_BASE && func < PANWIN_FUNC_DRM_BASE + 0x100) {
		uint32_t nr = PANWIN_IOCTL_TO_DRM_NR(code);
		for (size_t i = 0; i < PW_ARRAY_SIZE(drm_table); i++) {
			if (_IOC_NR(drm_table[i].ioctl) == nr) {
				fn = drm_table[i].fn;
				want = _IOC_SIZE(drm_table[i].ioctl);
				break;
			}
		}
	} else {
		for (size_t i = 0; i < PW_ARRAY_SIZE(priv_table); i++) {
			if (priv_table[i].code == code) {
				fn = priv_table[i].fn;
				want = priv_table[i].size;
				break;
			}
		}
	}
	if (returned)
		*returned = 0;
	if (!fn)
		return lx_to_result(-LX_ENOTTY);
	/* METHOD_BUFFERED: input must hold the whole struct; output may be 0 (write-only). */
	if (in_size != want || (out_size && out_size != want) || !buf)
		return lx_to_result(-LX_EINVAL);

	AcquireSRWLockExclusive(&fk_lock);
	r = fn(f, buf);
	ReleaseSRWLockExclusive(&fk_lock);
	if (returned)
		*returned = r ? 0 : out_size;
	return lx_to_result(r);
}

static int fake_enumerate(void)
{
	return FAKE_DEVICES;
}

static int fake_device_path(int index, char *buf, size_t len)
{
	if (index < 0 || index >= FAKE_DEVICES)
		return -ENOENT;
	snprintf(buf, len, "fake:\\\\.\\PanWin%d", index);
	return 0;
}

static int fake_open(int index, void **file)
{
	if (index < 0 || index >= FAKE_DEVICES)
		return -ENOENT;
	struct fk_file *f = calloc(1, sizeof(*f));
	if (!f)
		return -ENOMEM;
	f->index = index;
	f->user_mmio_offset = DRM_PANTHOR_USER_MMIO_OFFSET; /* 32-bit variant on LLP64 */
	*file = f;
	return 0;
}

static void fake_close(void *file)
{
	struct fk_file *f = file;

	AcquireSRWLockExclusive(&fk_lock);
	for (uint32_t i = 0; i < f->nsyncobjs; i++)
		if (f->syncobjs[i].used)
			so_free_events(&f->syncobjs[i]);
	for (uint32_t i = 0; i < f->nbos; i++)
		if (f->bos[i].used)
			CloseHandle(f->bos[i].section);
	/* Like the real driver at IRP_MJ_CLEANUP: tear down the file's remaining mappings. */
	for (uint32_t i = 0; i < f->nmaps; i++)
		fk_map_release(&f->maps[i]);
	WakeAllConditionVariable(&fk_cv);
	ReleaseSRWLockExclusive(&fk_lock);

	free(f->syncobjs);
	free(f->bos);
	free(f->maps);
	free(f);
}

static void *fake_os_handle(void *file)
{
	return file;
}

const struct panwin_transport pw_transport_fake = {
	.version = PANWIN_TRANSPORT_VERSION,
	.name = "fake",
	.enumerate = fake_enumerate,
	.device_path = fake_device_path,
	.open = fake_open,
	.close = fake_close,
	.ioctl = fake_ioctl,
	.os_handle = fake_os_handle,
};
