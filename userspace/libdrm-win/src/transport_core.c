/* SPDX-License-Identifier: MIT */
/*
 * transport_core.c - libdrm-win transport "core": the real kmd/core in-process, on the host-test
 * mock G610 (kmd/tests/pw_os_mock.c + mock_csf.c), booting the real mali_csffw.bin, or with
 * PANWIN_CORE_MODEL=g31 on the mock Mali-G31 (job manager, mock_jm.c, no firmware), where the
 * core speaks the panfrost uAPI.
 * Built only with -Dcore_transport=true (x86_64 test builds, see include/panwin_core.h).
 *
 * This file plays the part of kmd/win/pw_win_ioctl.c (the panwin.sys IOCTL layer) and must keep
 * its ABI v2 behaviour (include/panwin/panwin_ioctl.h):
 *  - DRM ioctls: in <= 4096 bytes, out <= in, the struct is copied into a "system buffer", run
 *    through pw_file_ioctl() and copied back (out bytes) on success only; failures complete
 *    with PANWIN_STATUS_FROM_ERRNO(errno). Nested user pointers are plain pointers here (the
 *    mock's pw_os_copy_{from,to}_user is memcpy).
 *  - GET_INFO (the device's own DRM version, PANWIN_INFO_JM on the G31), MMAP/MUNMAP (whole
 *    mappings only, BO reference held per mapping, read-only LATEST_FLUSH_ID page on CSF,
 *    writable mappings of it refused; on JM the core refuses the MMIO offsets with EINVAL),
 *    SYNCOBJ_EVENT on a caller-created event (the "kernel" duplicates the handle, sets it once
 *    and closes its reference).
 *  - Close of the last handle: cancel pending notifies, tear down mappings, pw_file_close().
 * Concurrent ioctls on one file are allowed (libdrm-win keeps the file alive while one runs).
 * Not a driver and never run on hardware.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "panwin/panwin_ioctl.h"
#include "panthor_drm.h"
#include "panwin_core.h"
#include "pw_internal.h"

#include "pw_core.h"
#include "mock_gpu.h"

#ifndef PANWIN_CORE_FW_DEFAULT
#define PANWIN_CORE_FW_DEFAULT ""
#endif

#define CORE_DRM_ARG_MAX	4096u		/* = kmd/win PW_DRM_ARG_MAX */
#define CORE_PAGE		4096u
#define LINUX_EBADF		9
#define LINUX_ENODEV		19
#define STATUS_INVALID_DEVICE_REQUEST_	((int32_t)0xC0000010)
#define STATUS_BUFFER_TOO_SMALL_	((int32_t)0xC0000023)

struct core_map {
	struct core_map *next;
	uint8_t *view;		/* mock_mem_user_map() view (BO) or NULL (flush-id page) */
	void *user_va;
	uint64_t size;
	struct pw_bo *bo;	/* reference from pw_file_mmap_lookup(), NULL for MMIO */
};

struct core_notify {
	struct core_notify *next;
	HANDLE event;		/* our duplicate of the caller's event */
	void *cookie;
	volatile LONG fired;
};

struct core_file {
	struct pw_file *pwf;
	enum pw_backend backend;	/* of g.dev, fixed while the file is open */
	SRWLOCK lock;		/* maps, notifies */
	struct core_map *maps;
	struct core_notify *notifies;
};

#define CORE_NCSG 8u		/* MOCK_FW_GROUP_NUM */
#define CORE_NCS 8u		/* MOCK_FW_STREAM_NUM */

static struct {
	SRWLOCK lock;		/* everything below except the counters */
	struct pw_os *os;
	struct pw_device *dev;
	enum mock_model model;	/* of the live device (PANWIN_CORE_MODEL at bring-up) */
	uint32_t nfiles;
	void *flush_page;	/* LATEST_FLUSH_ID page: zeros, PAGE_READONLY */
	struct panwin_core_report rep;
	struct mock_cs_stats last[CORE_NCSG][CORE_NCS];
	CRITICAL_SECTION cs_lock; /* rep.cs_*, last[] */
	INIT_ONCE once;
} g = { .lock = SRWLOCK_INIT, .once = INIT_ONCE_STATIC_INIT };

static volatile LONG64 c_ioctls, c_ioctl_errors, c_mmaps, c_munmaps, c_events;

static BOOL CALLBACK core_once(PINIT_ONCE o, void *p, void **ctx)
{
	(void)o;
	(void)p;
	(void)ctx;
	InitializeCriticalSection(&g.cs_lock);
	return TRUE;
}

static bool env_set(const char *name)
{
	char v[8];
	DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));

	return n > 0 && n < sizeof(v) && v[0] != '0';
}

static int32_t st_errno(int linux_errno)
{
	return linux_errno ? PANWIN_STATUS_FROM_ERRNO(linux_errno) : 0;
}

/* PW_OK / -errno -> NTSTATUS, as pw_win_status_from_errno(). */
static int32_t st_pw(int ret)
{
	if (!ret)
		return 0;
	if (ret > 0 || ret < -4095)
		ret = PW_EIO;
	return PANWIN_STATUS_FROM_ERRNO(-ret);
}

/* ---- mock CSF executor statistics ---- */

/* Accumulate what every CS slot executed since the last sample (a slot whose counters went
 * backwards was reset/reused: count it from zero). Caller holds g.lock (device alive).
 * The G31 has no command-stream executor: nothing to sample. */
static void cs_sample(void)
{
	if (g.model != MOCK_MODEL_G610)
		return;
	EnterCriticalSection(&g.cs_lock);
	for (unsigned int i = 0; i < CORE_NCSG; i++) {
		for (unsigned int j = 0; j < CORE_NCS; j++) {
			struct mock_cs_stats st, *l = &g.last[i][j];

			mock_csf_cs_stats(g.os, i, j, &st);
			if (st.instrs < l->instrs || st.doorbells < l->doorbells)
				memset(l, 0, sizeof(*l));
			g.rep.cs_instrs += st.instrs - l->instrs;
			g.rep.cs_runs += st.runs - l->runs;
			g.rep.cs_syncs += st.syncs - l->syncs;
			g.rep.cs_waits += st.waits - l->waits;
			g.rep.cs_doorbells += st.doorbells - l->doorbells;
			if (st.fatal && !l->fatal) {
				g.rep.cs_fatal++;
				g.rep.cs_fatal_type = st.fatal_type;
			}
			*l = st;
		}
	}
	LeaveCriticalSection(&g.cs_lock);
}

/* ---- device lifetime ---- */

/* PANWIN_CORE_MODEL: "g610" (default, RK3588, CSF) or "g31" (H616, job manager). -1 if unknown. */
static int core_model(enum mock_model *m)
{
	char v[16];
	DWORD n = GetEnvironmentVariableA("PANWIN_CORE_MODEL", v, sizeof(v));

	*m = MOCK_MODEL_G610;
	if (!n)
		return 0;
	if (n >= sizeof(v))
		return -1;
	if (!_stricmp(v, "g610"))
		return 0;
	if (!_stricmp(v, "g31")) {
		*m = MOCK_MODEL_G31;
		return 0;
	}
	return -1;
}

/* Caller holds g.lock exclusive. */
static int core_device_up(void)
{
	struct mock_config cfg = { 0 };
	struct pw_device_params p = { 0 };
	static char fw[MAX_PATH];
	enum mock_model model;
	bool ok;
	DWORD n;
	int ret;

	if (core_model(&model)) {
		fprintf(stderr, "libdrm-win core: PANWIN_CORE_MODEL must be g610 or g31\n");
		g.rep.bringup_failures++;
		return -EINVAL;
	}
	if (model == MOCK_MODEL_G31) {
		fw[0] = '\0';	/* job manager: no firmware */
		goto mock;
	}
	n = GetEnvironmentVariableA("PANWIN_CORE_FW", fw, sizeof(fw));
	if (!n || n >= sizeof(fw))
		snprintf(fw, sizeof(fw), "%s", PANWIN_CORE_FW_DEFAULT);
	if (!fw[0] || GetFileAttributesA(fw) == INVALID_FILE_ATTRIBUTES) {
		fprintf(stderr, "libdrm-win core: firmware '%s' not found (set PANWIN_CORE_FW)\n", fw);
		g.rep.bringup_failures++;
		return -ENOENT;
	}
	if (!g.flush_page) {
		DWORD old;

		g.flush_page = VirtualAlloc(NULL, CORE_PAGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (!g.flush_page || !VirtualProtect(g.flush_page, CORE_PAGE, PAGE_READONLY, &old))
			return -ENOMEM;
	}

mock:
	cfg.model = model;
	cfg.fw_path = fw[0] ? fw : NULL;
	/* User BOs read as zero, as on Linux. PANWIN_CORE_POISON=1 keeps the mock's default 0xa5
	 * poison (repro for the mock ignoring PW_MEM_ZEROED, docs/status/e2e.md). */
	cfg.honor_zeroed = !env_set("PANWIN_CORE_POISON");
	g.os = mock_create(&cfg);	/* aborts on allocation failure */
	p.os = g.os;
	p.boot_firmware = true;	/* CSF; ignored by the core on JM */
	ret = pw_device_alloc(&p, &g.dev);
	if (!ret) {
		/* As kmd/win: publish the device to the "ISR" before probe unmasks interrupts. */
		mock_attach_irq(g.os, g.dev);
		ret = pw_device_probe(g.dev);
	}
	/* CSF: up means the firmware booted; JM: the probe picked the job-manager backend. */
	ok = !ret && (model == MOCK_MODEL_G31 ? pw_device_backend(g.dev) == PW_BACKEND_JM :
						 pw_device_fw_booted(g.dev));
	if (!ok) {
		fprintf(stderr, "libdrm-win core: device bring-up failed (%d, %s)\n", ret,
			model == MOCK_MODEL_G31 ? "mock G31" :
			g.dev && pw_device_fw_booted(g.dev) ? "fw booted" : "fw not booted");
		mock_detach_irq(g.os);
		if (g.dev)
			pw_device_destroy(g.dev);
		mock_destroy(g.os);
		g.dev = NULL;
		g.os = NULL;
		g.rep.bringup_failures++;
		return -ENODEV;
	}
	g.model = model;
	memset(g.last, 0, sizeof(g.last));
	g.rep.devices_created++;
	g.rep.device_live = 1;
	if (env_set("PANWIN_CORE_REPORT")) {
		char d[96];

		pw_device_describe(g.dev, d, sizeof(d));
		fprintf(stderr, "libdrm-win core: device up: %s\n", d);
	}
	return 0;
}

/* Caller holds g.lock exclusive; no file is open. */
static void core_device_down(void)
{
	long allocs, mems, sync;
	uint32_t errs, unclaimed;

	cs_sample();
	mock_detach_irq(g.os);
	pw_device_destroy(g.dev);
	allocs = mock_live_allocs(g.os);
	mems = mock_live_mems(g.os);
	sync = mock_live_sync(g.os);
	errs = mock_model_errors(g.os);
	unclaimed = mock_irq_unclaimed(g.os);
	mock_destroy(g.os);
	g.os = NULL;
	g.dev = NULL;
	g.rep.leaked_allocs += allocs;
	g.rep.leaked_mems += mems;
	g.rep.leaked_sync += sync;
	g.rep.model_errors += errs;
	g.rep.irq_unclaimed += unclaimed;
	g.rep.devices_destroyed++;
	g.rep.device_live = 0;
	if (allocs || mems || sync || errs || unclaimed || env_set("PANWIN_CORE_REPORT"))
		fprintf(stderr, "libdrm-win core: device down: leaked allocs=%ld mems=%ld sync=%ld, "
			"mock model errors=%u, unclaimed irqs=%u -> %s\n", allocs, mems, sync, errs,
			unclaimed, (allocs || mems || sync || errs || unclaimed) ? "NOT CLEAN" : "clean");
}

/* ---- transport ---- */

static int core_enumerate(void)
{
	return 1;
}

static int core_device_path(int index, char *buf, size_t len)
{
	enum mock_model m;

	if (index != 0)
		return -ENOENT;
	if (core_model(&m))
		return -EINVAL;
	snprintf(buf, len, "core:mock-%s", m == MOCK_MODEL_G31 ? "g31" : "g610");
	return 0;
}

static int core_open(int index, void **file)
{
	struct core_file *cf;
	int ret = 0;

	if (index != 0)
		return -ENOENT;
	InitOnceExecuteOnce(&g.once, core_once, NULL, NULL);
	cf = calloc(1, sizeof(*cf));
	if (!cf)
		return -ENOMEM;
	InitializeSRWLock(&cf->lock);

	AcquireSRWLockExclusive(&g.lock);
	if (!g.dev)
		ret = core_device_up();
	if (!ret) {
		cf->backend = pw_device_backend(g.dev);
		ret = pw_file_open(g.dev, &cf->pwf);
		if (ret) {
			ret = -ENOMEM;
			if (!g.nfiles)
				core_device_down();
		} else {
			g.nfiles++;
			g.rep.open_files = g.nfiles;
		}
	}
	ReleaseSRWLockExclusive(&g.lock);
	if (ret) {
		free(cf);
		return ret;
	}
	*file = cf;
	return 0;
}

static void map_release(struct core_map *m)
{
	if (m->bo) {
		mock_mem_user_unmap(g.os, pw_bo_os_mem(m->bo), m->view);
		pw_bo_put(m->bo);
	}
	free(m);
}

static void notify_fire(void *ctx)
{
	struct core_notify *n = ctx;

	SetEvent(n->event);
	CloseHandle(n->event);
	InterlockedExchange(&n->fired, 1); /* last access to n */
}

static void core_close(void *file)
{
	struct core_file *cf = file;
	uint32_t maps = 0, notifies = 0;

	/* No ioctl of this file runs any more (libdrm-win holds the file while one does). */
	while (cf->notifies) {
		struct core_notify *n = cf->notifies;

		cf->notifies = n->next;
		if (!InterlockedCompareExchange(&n->fired, 0, 0)) {
			if (pw_file_syncobj_notify_cancel(cf->pwf, n->cookie)) {
				CloseHandle(n->event);
				notifies++;
			} else {
				while (!InterlockedCompareExchange(&n->fired, 0, 0))
					Sleep(1);
			}
		}
		free(n);
	}
	while (cf->maps) {
		struct core_map *m = cf->maps;

		cf->maps = m->next;
		map_release(m);
		maps++;
	}

	AcquireSRWLockExclusive(&g.lock);
	cs_sample();
	pw_file_close(cf->pwf);
	g.rep.leaked_maps += maps;
	g.rep.leaked_notifies += notifies;
	g.nfiles--;
	g.rep.open_files = g.nfiles;
	if (!g.nfiles)
		core_device_down();
	ReleaseSRWLockExclusive(&g.lock);
	if ((maps || notifies) && env_set("PANWIN_CORE_REPORT"))
		fprintf(stderr, "libdrm-win core: file closed with %u mapping(s) and %u pending "
			"event(s)\n", maps, notifies);
	free(cf);
}

static int core_mmap(struct core_file *cf, struct panwin_mmap *a)
{
	struct pw_mmap_target t;
	struct core_map *m;
	int ret;

	if (!a->size || (a->size & (CORE_PAGE - 1)) || (a->offset & (CORE_PAGE - 1)) ||
	    a->size > 0xFFFFF000u || a->pad)
		return PW_EINVAL;
	if (!a->prot || (a->prot & ~(PANWIN_PROT_READ | PANWIN_PROT_WRITE)))
		return PW_EINVAL;
	memset(&t, 0, sizeof(t));
	ret = pw_file_mmap_lookup(cf->pwf, a->offset, a->size, &t);
	if (ret)
		return ret;
	if ((a->prot & PANWIN_PROT_WRITE) && t.read_only) {
		ret = PW_EINVAL;
		goto err_put;
	}
	m = calloc(1, sizeof(*m));
	if (!m) {
		ret = PW_ENOMEM;
		goto err_put;
	}
	if (t.kind == PW_MMAP_BO && t.bo) {
		struct pw_os_mem *mem = pw_bo_os_mem(t.bo);

		if (!mem || t.offset_in_obj > pw_os_mem_size(mem) ||
		    a->size > pw_os_mem_size(mem) - t.offset_in_obj) {
			free(m);
			ret = PW_EINVAL;
			goto err_put;
		}
		m->view = mock_mem_user_map(g.os, mem, (a->prot & PANWIN_PROT_WRITE) != 0);
		if (!m->view) {
			free(m);
			ret = PW_ENOMEM;
			goto err_put;
		}
		m->bo = t.bo;
		m->user_va = m->view + t.offset_in_obj;
	} else if (t.kind == PW_MMAP_MMIO && cf->backend == PW_BACKEND_CSF) {
		/* Only the LATEST_FLUSH_ID page (panthor_device.c:~470 panthor_device_mmap_io):
		 * a static zero page stands in for the register. panthor only: the core already
		 * refuses these offsets on JM. */
		if (a->size != CORE_PAGE) {
			free(m);
			return PW_EINVAL;
		}
		m->user_va = g.flush_page;
	} else {
		free(m);
		ret = PW_EINVAL;
		goto err_put;
	}
	m->size = a->size;
	AcquireSRWLockExclusive(&cf->lock);
	m->next = cf->maps;
	cf->maps = m;
	ReleaseSRWLockExclusive(&cf->lock);
	a->user_va = (uint64_t)(uintptr_t)m->user_va;
	InterlockedIncrement64(&c_mmaps);
	return PW_OK;

err_put:
	if (t.kind == PW_MMAP_BO && t.bo)
		pw_bo_put(t.bo);
	return ret;
}

static int core_munmap(struct core_file *cf, const struct panwin_munmap *a)
{
	struct core_map **pp, *found = NULL;

	AcquireSRWLockExclusive(&cf->lock);
	for (pp = &cf->maps; *pp; pp = &(*pp)->next) {
		if ((uint64_t)(uintptr_t)(*pp)->user_va == a->user_va && (*pp)->size == a->size) {
			found = *pp;
			*pp = found->next;
			break;
		}
	}
	ReleaseSRWLockExclusive(&cf->lock);
	if (!found)
		return PW_EINVAL;
	map_release(found);
	InterlockedIncrement64(&c_munmaps);
	return PW_OK;
}

static int core_syncobj_event(struct core_file *cf, const struct panwin_syncobj_event *a)
{
	struct core_notify *n, **pp;
	HANDLE dup;
	int ret;

	if (!a->event_handle || a->event_handle != (uint64_t)(uintptr_t)a->event_handle)
		return -LINUX_EBADF;
	/* ObReferenceObjectByHandle(EVENT_MODIFY_STATE) stand-in. */
	if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)(uintptr_t)a->event_handle,
			     GetCurrentProcess(), &dup, EVENT_MODIFY_STATE, FALSE, 0))
		return -LINUX_EBADF;
	n = calloc(1, sizeof(*n));
	if (!n) {
		CloseHandle(dup);
		return PW_ENOMEM;
	}
	n->event = dup;

	/* Free records whose callback completed. */
	AcquireSRWLockExclusive(&cf->lock);
	for (pp = &cf->notifies; *pp;) {
		struct core_notify *o = *pp;

		if (InterlockedCompareExchange(&o->fired, 0, 0)) {
			*pp = o->next;
			free(o);
		} else {
			pp = &o->next;
		}
	}
	ReleaseSRWLockExclusive(&cf->lock);

	ret = pw_file_syncobj_notify(cf->pwf, a->handle, a->point, a->flags, notify_fire, n,
				     &n->cookie);
	if (ret) {
		CloseHandle(dup);
		free(n);
		return ret;
	}
	InterlockedIncrement64(&c_events);
	if (InterlockedCompareExchange(&n->fired, 0, 0)) {
		free(n);
		return PW_OK;
	}
	AcquireSRWLockExclusive(&cf->lock);
	n->next = cf->notifies;
	cf->notifies = n;
	ReleaseSRWLockExclusive(&cf->lock);
	return PW_OK;
}

static int32_t core_get_info(void *buf, uint32_t out_size, uint32_t *returned)
{
	struct panwin_info *pi = buf;
	uint32_t maj, min, patch;

	if (out_size < sizeof(*pi))
		return STATUS_BUFFER_TOO_SMALL_;
	memset(pi, 0, sizeof(*pi));
	pi->abi_version = PANWIN_ABI_VERSION;
	AcquireSRWLockShared(&g.lock);
	/* As kmd/win pw_win_get_info(): the device's own version (panthor without one). */
	pw_device_drm_version(g.dev, &maj, &min, &patch, NULL);
	pi->drm_version_major = maj;
	pi->drm_version_minor = min;
	if (g.dev) {
		pi->flags = (pw_device_fw_booted(g.dev) ? PANWIN_INFO_FW_BOOTED : 0) |
			    (pw_device_backend(g.dev) == PW_BACKEND_JM ? PANWIN_INFO_JM : 0);
		pw_device_describe(g.dev, pi->description, sizeof(pi->description));
	} else {
		snprintf(pi->description, sizeof(pi->description), "core transport: no device");
	}
	ReleaseSRWLockShared(&g.lock);
	pi->description[sizeof(pi->description) - 1] = 0;
	*returned = sizeof(*pi);
	return 0;
}

static int32_t core_drm_ioctl(struct core_file *cf, uint32_t nr, void *buf, uint32_t in_size,
			      uint32_t out_size, uint32_t *returned)
{
	/* The METHOD_BUFFERED system buffer: the core never sees the caller's struct. */
	_Alignas(16) uint8_t sysbuf[CORE_DRM_ARG_MAX];
	int ret;

	if (in_size > CORE_DRM_ARG_MAX || out_size > in_size)
		return st_errno(EINVAL);
	memcpy(sysbuf, buf, in_size);
	/* panthor only: on JM 0x40 + 0x0a is DRM_PANFROST_JM_CTX_CREATE. */
	if (cf->backend == PW_BACKEND_CSF && nr == 0x40 + DRM_PANTHOR_GROUP_DESTROY) {
		AcquireSRWLockShared(&g.lock);
		cs_sample(); /* before the slot is torn down */
		ReleaseSRWLockShared(&g.lock);
	}
	InterlockedIncrement64(&c_ioctls);
	ret = pw_file_ioctl(cf->pwf, nr, sysbuf, in_size);
	if (ret)
		InterlockedIncrement64(&c_ioctl_errors);
	if (env_set("PANWIN_CORE_TRACE") ||
	    (ret && ret != PW_ETIME && ret != PW_EINTR && env_set("PANWIN_CORE_REPORT")))
		fprintf(stderr, "libdrm-win core: DRM ioctl 0x%02x (%u bytes) -> %d\n", nr, in_size,
			ret);
	if (ret)
		return st_pw(ret);
	memcpy(buf, sysbuf, out_size);
	*returned = out_size;
	return 0;
}

static int32_t core_ioctl(void *file, uint32_t code, void *buf, uint32_t in_size,
			  uint32_t out_size, uint32_t *returned)
{
	struct core_file *cf = file;
	uint32_t func = (code >> 2) & 0xFFFu;
	_Alignas(8) uint8_t pbuf[sizeof(struct panwin_mmap)];
	size_t size, want_out;
	int ret;

	*returned = 0;
	if (func >= PANWIN_FUNC_DRM_BASE && func <= PANWIN_FUNC_DRM_BASE + 0xFF &&
	    code == PANWIN_IOCTL_DRM(func - PANWIN_FUNC_DRM_BASE))
		return core_drm_ioctl(cf, func - PANWIN_FUNC_DRM_BASE, buf, in_size, out_size,
				      returned);
	switch (code) {
	case PANWIN_IOCTL_GET_INFO:
		return core_get_info(buf, out_size, returned);
	case PANWIN_IOCTL_MMAP:
	case PANWIN_IOCTL_MUNMAP:
	case PANWIN_IOCTL_SYNCOBJ_EVENT:
		break;
	default:
		return STATUS_INVALID_DEVICE_REQUEST_;
	}
	/* pw_win_file_private(): exact sizes; MMAP in = out, the others out 0 (or = in). */
	size = code == PANWIN_IOCTL_MMAP ? sizeof(struct panwin_mmap) :
	       code == PANWIN_IOCTL_MUNMAP ? sizeof(struct panwin_munmap) :
	       sizeof(struct panwin_syncobj_event);
	want_out = code == PANWIN_IOCTL_MMAP ? size : 0;
	if (in_size != size || (out_size != want_out && out_size != size))
		return st_errno(EINVAL);
	memcpy(pbuf, buf, size);
	switch (code) {
	case PANWIN_IOCTL_MMAP:
		ret = core_mmap(cf, (struct panwin_mmap *)pbuf);
		break;
	case PANWIN_IOCTL_MUNMAP:
		ret = core_munmap(cf, (struct panwin_munmap *)pbuf);
		break;
	default:
		ret = core_syncobj_event(cf, (struct panwin_syncobj_event *)pbuf);
		break;
	}
	if (ret)
		return st_pw(ret);
	if (code == PANWIN_IOCTL_MMAP) {
		memcpy(buf, pbuf, size);
		*returned = (uint32_t)size;
	}
	return 0;
}

static void *core_os_handle(void *file)
{
	return file;
}

const struct panwin_transport pw_transport_core = {
	.version = PANWIN_TRANSPORT_VERSION,
	.name = "core",
	.enumerate = core_enumerate,
	.device_path = core_device_path,
	.open = core_open,
	.close = core_close,
	.ioctl = core_ioctl,
	.os_handle = core_os_handle,
};

__declspec(dllexport) int panwin_core_get_report(struct panwin_core_report *r);
__declspec(dllexport) int panwin_core_get_report(struct panwin_core_report *r)
{
	if (!r || r->version != PANWIN_CORE_REPORT_VERSION)
		return -1;
	InitOnceExecuteOnce(&g.once, core_once, NULL, NULL);
	AcquireSRWLockExclusive(&g.lock);
	if (g.dev)
		cs_sample();
	EnterCriticalSection(&g.cs_lock);
	*r = g.rep;
	LeaveCriticalSection(&g.cs_lock);
	ReleaseSRWLockExclusive(&g.lock);
	r->version = PANWIN_CORE_REPORT_VERSION;
	r->drm_ioctls = (uint64_t)c_ioctls;
	r->drm_ioctl_errors = (uint64_t)c_ioctl_errors;
	r->mmaps = (uint64_t)c_mmaps;
	r->munmaps = (uint64_t)c_munmaps;
	r->syncobj_events = (uint64_t)c_events;
	return 0;
}

/*
 * Test hook (jm-e2e): the state of the fence in binary syncobj @handle of DRM fd @fd, encoded like
 * a Linux sync_file's status (SYNC_IOC_FILE_INFO: 1 signalled, 0 active, < 0 the fence error).
 * Windows has no sync_file export, and SYNCOBJ_WAIT succeeds whatever the fence error is (as on
 * Linux), so this is how a test sees a failed job's -EINVAL / -ECANCELED. See panwin_core.h.
 * The core's pw_sync.h / pw_fence.h cannot be included here: pw_internal.h defines libdrm-win's own
 * struct pw_file. cf->pwf is the core's file, passed through that name as everywhere in this file.
 */
struct pw_fence;
int pw_syncobj_find_fence(struct pw_file *file, uint32_t handle, uint64_t point, uint32_t flags,
			  uint64_t deadline_ns, struct pw_fence **out);
bool pw_fence_is_signalled(struct pw_fence *f);
int pw_fence_error(struct pw_fence *f);
void pw_fence_put(struct pw_fence *f);

__declspec(dllexport) int panwin_core_syncobj_status(int fd, uint32_t handle, int32_t *status);
__declspec(dllexport) int panwin_core_syncobj_status(int fd, uint32_t handle, int32_t *status)
{
	struct pw_file *f = pw_file_get(fd);
	struct pw_fence *fence = NULL;
	struct core_file *cf;
	int ret;

	if (!f)
		return -1;	/* errno = EBADF */
	if (f->tp != &pw_transport_core || !status) {
		pw_file_put(f);
		errno = EINVAL;
		return -1;
	}
	cf = f->tfile;
	ret = pw_syncobj_find_fence(cf->pwf, handle, 0, 0, 0, &fence);
	if (!ret) {
		*status = pw_fence_is_signalled(fence) ? (pw_fence_error(fence) ?
							   pw_fence_error(fence) : 1) : 0;
		pw_fence_put(fence);
	}
	pw_file_put(f);
	if (ret) {
		errno = -ret;	/* EINVAL: no such syncobj, or no fence */
		return -1;
	}
	return 0;
}
