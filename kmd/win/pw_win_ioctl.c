// SPDX-License-Identifier: MIT
/*
 * pw_win_ioctl.c - file objects and IOCTL dispatch for panwin.sys (ABI v2,
 * include/panwin/panwin_ioctl.h).
 *
 * Every request is executed and completed in EvtIoInCallerContext, i.e. in the requesting
 * thread at PASSIVE_LEVEL, and is never put on a queue:
 *  - nested user pointers inside the Linux uAPI structs are read/written by the core with
 *    pw_os_copy_{from,to}_user (ProbeFor* + SEH in the caller's address space), and user
 *    mappings (MMAP) are created in the caller's process;
 *  - syncobj waits block only the calling thread. A sequential queue would deadlock a WAIT on
 *    one thread against the SIGNAL from another; a parallel queue does not promise caller
 *    context. User waits are UserMode/alertable (see pw_os_event_wait_user) so thread
 *    termination and user APCs end them with EINTR (libdrm-win restarts on EINTR).
 *  - libdrm-win opens the device FILE_FLAG_OVERLAPPED: on a synchronous handle the I/O manager
 *    itself serializes all requests of the file object, which would re-create the deadlock.
 *
 * DRM ioctls: PANWIN_IOCTL_DRM(nr), METHOD_BUFFERED, in = the Linux struct (size _IOC_SIZE),
 * out <= in. The system buffer is handed to pw_file_ioctl(nr, buf, in_len) unchanged and
 * copied back (out_len bytes) on success only. Errors complete with
 * PANWIN_STATUS_FROM_ERRNO(errno) so libdrm-win recovers the exact Linux errno.
 *
 * Lifetime: each file holds a core pw_file (opened in EvtDeviceFileCreate). In-flight ioctls
 * of a file hold its rundown reference. EvtFileCleanup (and D0Exit, for every open file) sets
 * the file's abort event (ends its user waits), waits for the rundown, unmaps the file's user
 * mappings in the owning process, cancels its pending SYNCOBJ_EVENT notifies and only then
 * calls pw_file_close(), as pw_core.h requires.
 * UNTESTED ON HARDWARE.
 */
#include "pw_win.h"
#include "pw_fmt.h"

#include "panwin/panwin_ioctl.h"
#include "drm.h"
#include "panthor_drm.h"

#define PW_WIN_EBADF      (-9)   /* Linux errno values the core has no PW_E* name for */
#define PW_DRM_ARG_MAX    4096u  /* sanity limit for a forwarded DRM struct */
#define PW_MAX_NOTIFIES   16384u /* pending SYNCOBJ_EVENT registrations per file */
#define PW_MAX_MAPS       65536u /* live user mappings per file */

_Static_assert(sizeof(struct panwin_info) == 120, "panwin_info layout");
_Static_assert(sizeof(struct panwin_mmap) == 32, "panwin_mmap layout");
_Static_assert(sizeof(struct panwin_munmap) == 16, "panwin_munmap layout");
_Static_assert(sizeof(struct panwin_syncobj_event) == 24, "panwin_syncobj_event layout");
_Static_assert(sizeof(struct drm_version) == 64, "drm_version layout");

static EVT_WDF_IO_QUEUE_IO_DEFAULT pw_win_io_default;

/* PW_OK -> STATUS_SUCCESS; -errno -> the errno-carrying customer status (ABI v2). */
NTSTATUS pw_win_status_from_errno(int err)
{
	if (!err)
		return STATUS_SUCCESS;
	if (err > 0 || err < -4095)
		err = PW_EIO;
	return (NTSTATUS)PANWIN_STATUS_FROM_ERRNO(-err);
}

/* ------------------------------------------------------------ user mappings */

struct pw_win_map {
	LIST_ENTRY link;          /* fc->maps */
	void *user_va;
	uint64_t size;
	PMDL mdl;                 /* partial MDL of the BO's pages, or an MDL over the MMIO page */
	struct pw_bo *bo;         /* reference from pw_file_mmap_lookup(); NULL for MMIO */
};

/* Unmap in the owning process (attaching if we are elsewhere, e.g. D0Exit in a system thread or
 * cleanup of a handle that was duplicated into another process), then drop the BO reference. */
static void pw_win_map_release(PW_FILE_CTX *fc, struct pw_win_map *m)
{
	PW_KAPC_STATE apc;
	BOOLEAN attach = IoGetCurrentProcess() != fc->process;

	if (attach)
		KeStackAttachProcess(fc->process, &apc);
	MmUnmapLockedPages(m->user_va, m->mdl);
	if (attach)
		KeUnstackDetachProcess(&apc);
	IoFreeMdl(m->mdl);
	if (m->bo)
		pw_bo_put(m->bo);
	pw_os_free(&fc->dev->os, m);
}

static int pw_win_mmap(PW_FILE_CTX *fc, struct panwin_mmap *a)
{
	struct pw_os *os = &fc->dev->os;
	struct pw_mmap_target t;
	struct pw_win_map *m;
	MEMORY_CACHING_TYPE cache;
	ULONG prio = NormalPagePriority | MdlMappingNoExecute;
	PMDL mdl;
	int ret;

	if (!a->size || (a->size & (PAGE_SIZE - 1)) || (a->offset & (PAGE_SIZE - 1)) ||
	    a->size > 0xFFFFF000u || a->pad)
		return PW_EINVAL;
	if (!a->prot || (a->prot & ~(PANWIN_PROT_READ | PANWIN_PROT_WRITE)))
		return PW_EINVAL;
	/* Mappings live in the creator's address space only: cleanup must be able to find the
	 * process again (and libdrm-win never passes device handles between processes). */
	if (IoGetCurrentProcess() != fc->process)
		return PW_EACCES;
	if (fc->nmaps >= PW_MAX_MAPS)
		return PW_ENOMEM;

	RtlZeroMemory(&t, sizeof(t));
	ret = pw_file_mmap_lookup(fc->pwf, a->offset, a->size, &t);
	if (ret)
		return ret;
	/* panthor_device_mmap_io(): a writable mapping of the user MMIO page is -EINVAL. */
	if ((a->prot & PANWIN_PROT_WRITE) && t.read_only) {
		ret = PW_EINVAL;
		goto err_put;
	}
	if (!(a->prot & PANWIN_PROT_WRITE))
		prio |= MdlMappingNoWrite;

	if (t.kind == PW_MMAP_BO) {
		struct pw_os_mem *mem = t.bo ? pw_bo_os_mem(t.bo) : NULL;
		PMDL src = mem ? pw_os_win_mem_mdl(mem) : NULL;
		char *va;

		if (!src || t.offset_in_obj > pw_os_mem_size(mem) ||
		    a->size > pw_os_mem_size(mem) - t.offset_in_obj) {
			ret = PW_EINVAL;
			goto err_put;
		}
		/* Every mapping of a page must use the attribute it was allocated with (ARM64:
		 * no mismatched aliases), whatever the core asked for. */
		cache = pw_os_win_mem_cache(mem);
		if ((cache == MmCached) != t.cached)
			PW_WLOG(PW_LOG_WARN, "mmap: core asked for a %s mapping of a %s BO; using the "
				"BO's attribute", t.cached ? "cached" : "WC", t.cached ? "WC" : "cached");
		/* MDLs from MmAllocatePagesForMdlEx have a NULL StartVa: compute the "address"
		 * IoBuildPartialMdl wants as an integer (NULL + offset is undefined behaviour in C,
		 * found by UBSan in kmd/win/tests). */
		va = (char *)((ULONG_PTR)MmGetMdlVirtualAddress(src) + (ULONG_PTR)t.offset_in_obj);
		mdl = IoAllocateMdl(va, (ULONG)a->size, FALSE, FALSE, NULL);
		if (!mdl) {
			ret = PW_ENOMEM;
			goto err_put;
		}
		IoBuildPartialMdl(src, mdl, va, (ULONG)a->size);
	} else if (t.kind == PW_MMAP_MMIO) {
		/*
		 * User view of a GPU register page: an MDL over our own MmMapIoSpace(Ex) mapping of
		 * the window (MmBuildMdlForNonPagedPool fills in the PFNs of the I/O pages), mapped
		 * with MmNonCached - the same Device attribute as the kernel mapping - and
		 * MdlMappingNoWrite. No \Device\PhysicalMemory section is involved.
		 */
		if ((t.mmio_offset & (PAGE_SIZE - 1)) || !os->mmio || !os->regs_enabled ||
		    t.mmio_offset > os->mmio_len || a->size > os->mmio_len - t.mmio_offset) {
			ret = PW_EINVAL;
			goto err_put;
		}
		cache = MmNonCached;
		mdl = IoAllocateMdl((PVOID)(os->mmio + t.mmio_offset), (ULONG)a->size, FALSE, FALSE,
				    NULL);
		if (!mdl) {
			ret = PW_ENOMEM;
			goto err_put;
		}
		MmBuildMdlForNonPagedPool(mdl);
	} else {
		ret = PW_EINVAL;
		goto err_put;
	}

	m = pw_os_zalloc(os, sizeof(*m));
	if (!m) {
		ret = PW_ENOMEM;
		goto err_mdl;
	}
	m->user_va = pw_win_map_user(mdl, cache, prio);
	if (!m->user_va) {
		pw_os_free(os, m);
		ret = PW_ENOMEM;
		goto err_mdl;
	}
	m->size = a->size;
	m->mdl = mdl;
	m->bo = t.kind == PW_MMAP_BO ? t.bo : NULL;
	pw_win_lock(&fc->lock);
	InsertTailList(&fc->maps, &m->link);
	fc->nmaps++;
	pw_win_unlock(&fc->lock);
	a->user_va = (uint64_t)(ULONG_PTR)m->user_va;
	return PW_OK;

err_mdl:
	IoFreeMdl(mdl);
err_put:
	if (t.kind == PW_MMAP_BO && t.bo)
		pw_bo_put(t.bo);
	return ret;
}

static int pw_win_munmap(PW_FILE_CTX *fc, const struct panwin_munmap *a)
{
	struct pw_win_map *found = NULL;
	PLIST_ENTRY le;

	if (IoGetCurrentProcess() != fc->process)
		return PW_EINVAL;
	pw_win_lock(&fc->lock);
	for (le = fc->maps.Flink; le != &fc->maps; le = le->Flink) {
		struct pw_win_map *m = CONTAINING_RECORD(le, struct pw_win_map, link);

		/* Whole mappings only (libdrm-win never splits one). */
		if ((uint64_t)(ULONG_PTR)m->user_va == a->user_va && m->size == a->size) {
			RemoveEntryList(&m->link);
			fc->nmaps--;
			found = m;
			break;
		}
	}
	pw_win_unlock(&fc->lock);
	if (!found)
		return PW_EINVAL;
	pw_win_map_release(fc, found);
	return PW_OK;
}

static void pw_win_unmap_all(PW_FILE_CTX *fc)
{
	for (;;) {
		struct pw_win_map *m = NULL;

		pw_win_lock(&fc->lock);
		if (!IsListEmpty(&fc->maps)) {
			m = CONTAINING_RECORD(RemoveHeadList(&fc->maps), struct pw_win_map, link);
			fc->nmaps--;
		}
		pw_win_unlock(&fc->lock);
		if (!m)
			return;
		pw_win_map_release(fc, m);
	}
}

/* -------------------------------------------------------- SYNCOBJ_EVENT notifies */

/*
 * One registration. The core calls pw_win_notify_fire() exactly once (unless cancelled), maybe
 * from an IRQ bottom half and maybe synchronously from pw_file_syncobj_notify(). The callback
 * touches only the record and the event, never the file: its last access is the store to
 * `fired`, after which whoever sees fired == 1 (the next SYNCOBJ_EVENT on the file, or
 * cleanup) frees the record. That keeps the callback independent of the file's lifetime.
 */
struct pw_win_notify {
	LIST_ENTRY link;          /* fc->notifies, under fc->lock */
	PKEVENT event;            /* referenced; dereferenced by fire, or by a successful cancel */
	void *cookie;
	volatile LONG fired;
};

static void pw_win_notify_fire(void *ctx)
{
	struct pw_win_notify *n = ctx;
	PKEVENT ev = n->event;

	KeSetEvent(ev, IO_NO_INCREMENT, FALSE);
	ObDereferenceObject(ev);
	__atomic_store_n(&n->fired, 1, __ATOMIC_RELEASE); /* last access to n */
}

static BOOLEAN pw_win_notify_fired(struct pw_win_notify *n)
{
	return __atomic_load_n(&n->fired, __ATOMIC_ACQUIRE) != 0;
}

/* Free records whose callback has completed. Caller holds fc->lock. */
static void pw_win_notify_sweep_locked(PW_FILE_CTX *fc)
{
	PLIST_ENTRY le = fc->notifies.Flink;

	while (le != &fc->notifies) {
		struct pw_win_notify *n = CONTAINING_RECORD(le, struct pw_win_notify, link);

		le = le->Flink;
		if (pw_win_notify_fired(n)) {
			RemoveEntryList(&n->link);
			fc->nnotifies--;
			pw_os_free(&fc->dev->os, n);
		}
	}
}

static int pw_win_syncobj_event(PW_FILE_CTX *fc, WDFREQUEST req,
				const struct panwin_syncobj_event *a)
{
	struct pw_os *os = &fc->dev->os;
	struct pw_win_notify *n;
	PKEVENT ev = NULL;
	NTSTATUS st;
	int ret;

	if (a->event_handle != (uint64_t)(ULONG_PTR)a->event_handle || !a->event_handle)
		return PW_WIN_EBADF;
	pw_win_lock(&fc->lock);
	pw_win_notify_sweep_locked(fc);
	ret = fc->nnotifies >= PW_MAX_NOTIFIES ? PW_ENOSPC : PW_OK;
	pw_win_unlock(&fc->lock);
	if (ret)
		return ret;

	/* The caller's handle, checked against the caller's handle table and access rights
	 * (RequestorMode UserMode: kernel handles are rejected). */
	st = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)a->event_handle, EVENT_MODIFY_STATE,
				       *ExEventObjectType, WdfRequestGetRequestorMode(req),
				       (PVOID *)&ev, NULL);
	if (!NT_SUCCESS(st))
		return st == STATUS_ACCESS_DENIED ? PW_EACCES :
		       st == STATUS_OBJECT_TYPE_MISMATCH ? PW_EINVAL : PW_WIN_EBADF;

	n = pw_os_zalloc(os, sizeof(*n));
	if (!n) {
		ObDereferenceObject(ev);
		return PW_ENOMEM;
	}
	n->event = ev;
	ret = pw_file_syncobj_notify(fc->pwf, a->handle, a->point, a->flags, pw_win_notify_fire, n,
				     &n->cookie);
	if (ret) {
		/* Assumes the core never calls fn when it returns an error. */
		ObDereferenceObject(ev);
		pw_os_free(os, n);
		return ret;
	}
	if (pw_win_notify_fired(n)) { /* fired synchronously (already signalled) */
		pw_os_free(os, n);
		return PW_OK;
	}
	pw_win_lock(&fc->lock);
	InsertTailList(&fc->notifies, &n->link);
	fc->nnotifies++;
	pw_win_unlock(&fc->lock);
	return PW_OK;
}

/* Cleanup: cancel every pending registration (the core must tolerate a cancel that races with
 * the notification firing: it then returns false and we wait for the callback to finish). */
static void pw_win_notify_cancel_all(PW_FILE_CTX *fc)
{
	for (;;) {
		struct pw_win_notify *n = NULL;

		pw_win_lock(&fc->lock);
		if (!IsListEmpty(&fc->notifies)) {
			n = CONTAINING_RECORD(RemoveHeadList(&fc->notifies), struct pw_win_notify, link);
			fc->nnotifies--;
		}
		pw_win_unlock(&fc->lock);
		if (!n)
			return;
		if (!pw_win_notify_fired(n)) {
			if (fc->pwf && pw_file_syncobj_notify_cancel(fc->pwf, n->cookie)) {
				ObDereferenceObject(n->event); /* fn will never run */
			} else {
				LARGE_INTEGER d;

				d.QuadPart = -10000; /* 1 ms: the callback is a KeSetEvent away */
				while (!pw_win_notify_fired(n))
					KeDelayExecutionThread(KernelMode, FALSE, &d);
			}
		}
		pw_os_free(&fc->dev->os, n);
	}
}

/* ------------------------------------------------------------------- file objects */

void pw_win_files_init(PW_DEVICE_CTX *ctx)
{
	ExInitializeRundownProtection(&ctx->core_rundown);
	ExInitializeFastMutex(&ctx->files_lock);
	InitializeListHead(&ctx->files);
}

VOID pw_win_file_create(WDFDEVICE device, WDFREQUEST request, WDFFILEOBJECT file)
{
	PW_DEVICE_CTX *ctx = PwDeviceCtx(device);
	PW_FILE_CTX *fc = PwFileCtx(file);
	struct pw_device *pw;
	int ret = PW_OK;

	fc->dev = ctx;
	fc->process = IoGetCurrentProcess();
	ObReferenceObject(fc->process);
	ExInitializeRundownProtection(&fc->rundown);
	KeInitializeEvent(&fc->abort, NotificationEvent, FALSE);
	ExInitializeFastMutex(&fc->lock);
	InitializeListHead(&fc->maps);
	InitializeListHead(&fc->notifies);

	/* Under files_lock so D0Exit either sees this file on the list or we see no core. */
	pw_win_lock(&ctx->files_lock);
	pw = pw_win_core(ctx);
	if (pw)
		ret = pw_file_open(pw, &fc->pwf);
	if (!ret)
		InsertTailList(&ctx->files, &fc->link);
	pw_win_unlock(&ctx->files_lock);

	if (ret) {
		/* No cleanup/close callbacks for a failed create. */
		PW_WLOG(PW_LOG_ERR, "pw_file_open failed: %d", ret);
		ObDereferenceObject(fc->process);
		fc->process = NULL;
		fc->pwf = NULL;
		WdfRequestComplete(request, pw_win_status_from_errno(ret));
		return;
	}
	/* Without a core device the handle still opens (GET_INFO reports why); every DRM ioctl on
	 * it fails with ENODEV. */
	WdfRequestComplete(request, STATUS_SUCCESS);
}

/* Caller holds ctx->files_lock. Idempotent. */
static void pw_win_file_teardown(PW_FILE_CTX *fc)
{
	if (fc->torn_down)
		return;
	fc->torn_down = TRUE;
	KeSetEvent(&fc->abort, IO_NO_INCREMENT, FALSE);    /* ends user waits with EINTR */
	ExWaitForRundownProtectionRelease(&fc->rundown); /* no ioctl of this file runs now */
	pw_win_notify_cancel_all(fc);
	pw_win_unmap_all(fc);
	if (fc->pwf) {
		pw_file_close(fc->pwf);
		fc->pwf = NULL;
	}
}

VOID pw_win_file_cleanup(WDFFILEOBJECT file)
{
	PW_FILE_CTX *fc = PwFileCtx(file);
	PW_DEVICE_CTX *ctx = fc->dev;

	/* Last handle closed; normally in the owning process (pw_win_map_release attaches
	 * otherwise). In-flight ioctls of other threads may still be running: teardown waits. */
	pw_win_lock(&ctx->files_lock);
	pw_win_file_teardown(fc);
	RemoveEntryList(&fc->link);
	InitializeListHead(&fc->link);
	pw_win_unlock(&ctx->files_lock);
}

VOID pw_win_file_close(WDFFILEOBJECT file)
{
	PW_FILE_CTX *fc = PwFileCtx(file);

	if (fc->process) {
		ObDereferenceObject(fc->process);
		fc->process = NULL;
	}
}

void pw_win_revoke_files(PW_DEVICE_CTX *ctx)
{
	PLIST_ENTRY le;
	ULONG n = 0;

	pw_win_lock(&ctx->files_lock);
	for (le = ctx->files.Flink; le != &ctx->files; le = le->Flink) {
		PW_FILE_CTX *fc = CONTAINING_RECORD(le, PW_FILE_CTX, link);

		if (!fc->torn_down) {
			pw_win_file_teardown(fc);
			n++;
		}
	}
	pw_win_unlock(&ctx->files_lock);
	if (n)
		PW_WLOG(PW_LOG_WARN, "device stopping: revoked %lu open file(s); their ioctls now "
			"fail with ENODEV", (unsigned long)n);
}

/* Enter/leave an ioctl on a file: rundown reference + in-flight registration. */
static BOOLEAN pw_win_file_enter(PW_FILE_CTX *fc, WDFREQUEST req)
{
	if (!ExAcquireRundownProtection(&fc->rundown))
		return FALSE;
	if (!fc->pwf) {
		ExReleaseRundownProtection(&fc->rundown);
		return FALSE;
	}
	pw_os_win_active_enter(&fc->dev->os, &PwReqCtx(req)->active, &fc->abort);
	return TRUE;
}

static void pw_win_file_leave(PW_FILE_CTX *fc, WDFREQUEST req)
{
	pw_os_win_active_leave(&fc->dev->os, &PwReqCtx(req)->active);
	ExReleaseRundownProtection(&fc->rundown);
}

/* ------------------------------------------------------------------- DRM ioctls */

static NTSTATUS pw_win_drm_ioctl(PW_FILE_CTX *fc, WDFREQUEST req, ULONG nr, size_t in_len,
				 size_t out_len, size_t *info)
{
	static uint64_t empty; /* argument for size-0 ioctls (never written: size is 0) */
	void *buf = &empty;
	NTSTATUS st;
	int ret;

	if (in_len > PW_DRM_ARG_MAX || out_len > in_len)
		return pw_win_status_from_errno(PW_EINVAL);
	if (in_len) {
		st = WdfRequestRetrieveInputBuffer(req, in_len, &buf, NULL);
		if (!NT_SUCCESS(st))
			return pw_win_status_from_errno(PW_EFAULT);
	}
	/* struct drm_version holds size_t and char *: only the 64-bit layout is supported. */
	if (nr == _IOC_NR(DRM_IOCTL_VERSION) && WdfRequestIsFrom32BitProcess(req))
		return pw_win_status_from_errno(PW_EINVAL);
	if (!pw_win_file_enter(fc, req))
		return pw_win_status_from_errno(PW_WIN_ENODEV);
	ret = pw_file_ioctl(fc->pwf, nr, buf, (uint32_t)in_len);
	pw_win_file_leave(fc, req);
	if (ret) {
		if (ret != PW_EINTR && ret != PW_ETIME && ret != PW_ENOTTY)
			PW_WLOG(PW_LOG_DBG, "DRM ioctl 0x%02lx: %d", (unsigned long)nr, ret);
		return pw_win_status_from_errno(ret);
	}
	*info = out_len; /* the (updated) struct goes back on success only */
	return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- private ioctls */

static NTSTATUS pw_win_get_info(PW_DEVICE_CTX *ctx, WDFREQUEST req, size_t *info)
{
	struct panwin_info *pi;
	struct pw_device *pw = NULL;
	uint32_t maj, min, patch;
	BOOLEAN held;
	NTSTATUS st;

	st = WdfRequestRetrieveOutputBuffer(req, sizeof(*pi), (PVOID *)&pi, NULL);
	if (!NT_SUCCESS(st))
		return st;
	RtlZeroMemory(pi, sizeof(*pi));
	pw_core_drm_version(&maj, &min, &patch);
	pi->abi_version = PANWIN_ABI_VERSION;
	pi->drm_version_major = maj;
	pi->drm_version_minor = min;
	pi->gpu_phys_base = (uint64_t)ctx->os.mmio_pa.QuadPart;
	held = ExAcquireRundownProtection(&ctx->core_rundown);
	if (held)
		pw = pw_win_core(ctx);
	pi->flags = (PANWIN_DEBUG_IOCTLS ? PANWIN_INFO_DEBUG_BUILD : 0) |
		    ((pw && pw_device_fw_booted(pw)) ? PANWIN_INFO_FW_BOOTED : 0);
	if (pw)
		pw_device_describe(pw, pi->description, sizeof(pi->description));
	else
		pw_win_snprintf(pi->description, sizeof(pi->description),
				"no core device: %s, core status %d",
				!held ? "device stopping" :
				ctx->gpu_powered ? "GPU powered" : "GPU power domain OFF",
				ctx->core_status);
	if (held)
		ExReleaseRundownProtection(&ctx->core_rundown);
	pi->description[sizeof(pi->description) - 1] = 0;
	*info = sizeof(*pi);
	return STATUS_SUCCESS;
}

#if PANWIN_DEBUG_IOCTLS
static NTSTATUS pw_win_debug_reg_read(PW_DEVICE_CTX *ctx, WDFREQUEST req, size_t *info)
{
	struct panwin_debug_reg_read *in, *out;
	uint32_t off;
	NTSTATUS st;

	st = WdfRequestRetrieveInputBuffer(req, sizeof(*in), (PVOID *)&in, NULL);
	if (NT_SUCCESS(st))
		st = WdfRequestRetrieveOutputBuffer(req, sizeof(*out), (PVOID *)&out, NULL);
	if (!NT_SUCCESS(st))
		return st;
	off = in->offset; /* in and out share the system buffer: read before writing */
	if (!ExAcquireRundownProtection(&ctx->core_rundown))
		return pw_win_status_from_errno(PW_WIN_ENODEV);
	if (!ctx->os.regs_enabled) {
		st = pw_win_status_from_errno(PW_WIN_ENODEV);
	} else if ((off & 3) || off > ctx->os.mmio_len - 4) {
		st = pw_win_status_from_errno(PW_EINVAL);
	} else {
		out->offset = off;
		out->value = pw_os_reg_read32(&ctx->os, off);
		*info = sizeof(*out);
		st = STATUS_SUCCESS;
	}
	ExReleaseRundownProtection(&ctx->core_rundown);
	return st;
}
#endif

/* Fixed-size private ioctl on a file (MMAP, MUNMAP, SYNCOBJ_EVENT). */
static NTSTATUS pw_win_file_private(PW_FILE_CTX *fc, WDFREQUEST req, ULONG code, size_t in_len,
				    size_t out_len, size_t *info)
{
	size_t size = code == PANWIN_IOCTL_MMAP ? sizeof(struct panwin_mmap) :
		      code == PANWIN_IOCTL_MUNMAP ? sizeof(struct panwin_munmap) :
		      sizeof(struct panwin_syncobj_event);
	size_t want_out = code == PANWIN_IOCTL_MMAP ? size : 0;
	void *buf;
	NTSTATUS st;
	int ret;

	/* libdrm-win: MMAP in = out = size; MUNMAP out 0; SYNCOBJ_EVENT out 0 or size. */
	if (in_len != size || (out_len != want_out && out_len != size))
		return pw_win_status_from_errno(PW_EINVAL);
	if (code == PANWIN_IOCTL_MMAP && out_len != size)
		return pw_win_status_from_errno(PW_EINVAL);
	st = WdfRequestRetrieveInputBuffer(req, size, &buf, NULL);
	if (!NT_SUCCESS(st))
		return pw_win_status_from_errno(PW_EFAULT);
	if (!pw_win_file_enter(fc, req))
		return pw_win_status_from_errno(PW_WIN_ENODEV);
	switch (code) {
	case PANWIN_IOCTL_MMAP:
		ret = pw_win_mmap(fc, buf);
		break;
	case PANWIN_IOCTL_MUNMAP:
		ret = pw_win_munmap(fc, buf);
		break;
	default:
		ret = pw_win_syncobj_event(fc, req, buf);
		break;
	}
	pw_win_file_leave(fc, req);
	if (ret)
		return pw_win_status_from_errno(ret);
	*info = code == PANWIN_IOCTL_MMAP ? size : 0;
	return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------- dispatch */

VOID pw_win_io_in_caller_context(WDFDEVICE device, WDFREQUEST req)
{
	PW_DEVICE_CTX *ctx = PwDeviceCtx(device);
	WDF_REQUEST_PARAMETERS p;
	WDFFILEOBJECT fo;
	PW_FILE_CTX *fc;
	ULONG code, func;
	size_t in_len, out_len, info = 0;
	NTSTATUS st;

	WDF_REQUEST_PARAMETERS_INIT(&p);
	WdfRequestGetParameters(req, &p);
	if (p.Type != WdfRequestTypeDeviceControl) {
		/* Read/write/internal ioctls: rejected by the default queue. */
		st = WdfDeviceEnqueueRequest(device, req);
		if (!NT_SUCCESS(st))
			WdfRequestComplete(req, st);
		return;
	}
	code = p.Parameters.DeviceIoControl.IoControlCode;
	in_len = p.Parameters.DeviceIoControl.InputBufferLength;
	out_len = p.Parameters.DeviceIoControl.OutputBufferLength;
	func = (code >> 2) & 0xFFF;
	fo = WdfRequestGetFileObject(req);
	fc = fo ? PwFileCtx(fo) : NULL;

	if (func >= PANWIN_FUNC_DRM_BASE && func <= PANWIN_FUNC_DRM_BASE + 0xFF &&
	    code == PANWIN_IOCTL_DRM(func - PANWIN_FUNC_DRM_BASE)) {
		st = fc ? pw_win_drm_ioctl(fc, req, func - PANWIN_FUNC_DRM_BASE, in_len, out_len,
					   &info) :
			  STATUS_INVALID_DEVICE_REQUEST;
		WdfRequestCompleteWithInformation(req, st, NT_SUCCESS(st) ? info : 0);
		return;
	}
	switch (code) {
	case PANWIN_IOCTL_GET_INFO:
		st = pw_win_get_info(ctx, req, &info);
		break;
	case PANWIN_IOCTL_MMAP:
	case PANWIN_IOCTL_MUNMAP:
	case PANWIN_IOCTL_SYNCOBJ_EVENT:
		st = fc ? pw_win_file_private(fc, req, code, in_len, out_len, &info) :
			  STATUS_INVALID_DEVICE_REQUEST;
		break;
#if PANWIN_DEBUG_IOCTLS
	case PANWIN_IOCTL_DEBUG_REG_READ:
		st = pw_win_debug_reg_read(ctx, req, &info);
		break;
#endif
	default:
		st = STATUS_INVALID_DEVICE_REQUEST; /* libdrm-win: ENOTTY */
		break;
	}
	WdfRequestCompleteWithInformation(req, st, NT_SUCCESS(st) ? info : 0);
}

static VOID pw_win_io_default(WDFQUEUE queue, WDFREQUEST req)
{
	(void)queue;
	WdfRequestComplete(req, STATUS_INVALID_DEVICE_REQUEST);
}

/* Default queue: only receives what pw_win_io_in_caller_context() does not handle itself. Not
 * power managed (it never touches the hardware). */
NTSTATUS pw_win_queue_init(PW_DEVICE_CTX *ctx)
{
	WDF_IO_QUEUE_CONFIG qcfg;
	NTSTATUS st;

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchParallel);
	qcfg.EvtIoDefault = pw_win_io_default;
	qcfg.PowerManaged = WdfFalse;
	st = WdfIoQueueCreate(ctx->device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
	if (!NT_SUCCESS(st))
		PW_WLOG(PW_LOG_ERR, "WdfIoQueueCreate failed: 0x%08x", (unsigned)st);
	return st;
}
