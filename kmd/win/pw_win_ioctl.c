// SPDX-License-Identifier: MIT
/*
 * pw_win_ioctl.c - file objects and IOCTL dispatch for panwin.sys.
 *
 * ABI: include/panwin/panwin_ioctl.h. A Linux DRM ioctl `nr` arrives as PANWIN_IOCTL_DRM(nr)
 * (METHOD_BUFFERED) carrying the Linux struct verbatim. Like Linux drm_ioctl(), the struct size
 * is taken from the Linux ioctl number (_IOC_SIZE), a shorter user struct is zero-extended and
 * only min(user, kernel) bytes are copied back, and only for _IOC_READ ioctls.
 *
 * Two stages:
 *  - pw_win_io_in_caller_context() runs in the requesting thread. Ioctls whose structs contain
 *    nested user pointers are prepared here: DRM_IOCTL_VERSION is answered directly (constant
 *    data, copied with ProbeForWrite under __try); DEV_QUERY's output buffer is probed and
 *    locked (WdfRequestProbeAndLockUserBufferForWrite) so the queue stage can fill it from
 *    any thread. Everything else is forwarded unchanged.
 *  - pw_win_io_device_control() runs from a sequential, passive-level queue and does the work.
 *
 * Errors use Linux errno semantics internally (PW_E*), mapped to NTSTATUS on completion by
 * pw_win_status_from_errno(); the table is documented in docs/status/kmd-win.md so libdrm-win
 * can invert it. UNTESTED ON HARDWARE.
 */
#include "pw_win.h"
#include "pw_fmt.h"

#include "panwin/panwin_ioctl.h"
#include "drm.h"
#include "panthor_drm.h"

#define PW_WIN_ENODEV  (-19)
#define PW_UOBJ_MAX    (64u * 1024) /* largest nested object we lock for a query */
#define PW_ARG_MAX     256          /* largest DRM ioctl struct handled here */

#define PW_OFFSETOFEND(type, field) (offsetof(type, field) + sizeof(((type *)0)->field))

/* The ABI is the Linux aarch64 layout; catch header/compiler drift at build time. */
_Static_assert(sizeof(struct drm_version) == 64, "drm_version layout");
_Static_assert(sizeof(struct drm_get_cap) == 16, "drm_get_cap layout");
_Static_assert(sizeof(struct drm_panthor_dev_query) == 16, "dev_query layout");
_Static_assert(sizeof(struct drm_panthor_gpu_info) == 112, "gpu_info layout");
_Static_assert(sizeof(struct drm_panthor_csif_info) == 24, "csif_info layout");
_Static_assert(sizeof(struct panwin_info) == 120, "panwin_info layout");
_Static_assert(_IOC_SIZE(DRM_IOCTL_VERSION) == sizeof(struct drm_version), "ioctl size");
_Static_assert(_IOC_SIZE(DRM_IOCTL_GET_CAP) <= PW_ARG_MAX &&
	       _IOC_SIZE(DRM_IOCTL_PANTHOR_DEV_QUERY) <= PW_ARG_MAX, "PW_ARG_MAX too small");

static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL pw_win_io_device_control;

NTSTATUS pw_win_status_from_errno(int err)
{
	switch (err) {
	case PW_OK:          return STATUS_SUCCESS;
	case PW_EPERM:       return STATUS_ACCESS_DENIED;
	case PW_ENOENT:      return STATUS_NOT_FOUND;
	case PW_EIO:         return STATUS_IO_DEVICE_ERROR;
	case PW_ENOMEM:      return STATUS_INSUFFICIENT_RESOURCES;
	case PW_EFAULT:      return STATUS_ACCESS_VIOLATION;
	case PW_EBUSY:       return STATUS_DEVICE_BUSY;
	case PW_WIN_ENODEV:  return STATUS_DEVICE_NOT_READY;
	case PW_EINVAL:      return STATUS_INVALID_PARAMETER;
	case PW_ENOSPC:      return STATUS_QUOTA_EXCEEDED;
	case PW_ERANGE:      return STATUS_INTEGER_OVERFLOW;
	case PW_ENOSYS:      return STATUS_NOT_IMPLEMENTED;
	case PW_EOPNOTSUPP:  return STATUS_NOT_SUPPORTED;
	case PW_ETIMEDOUT:   return STATUS_IO_TIMEOUT;
	default:             return STATUS_UNSUCCESSFUL;
	}
}

/* ------------------------------------------------------------------- file objects */

VOID pw_win_file_create(WDFDEVICE device, WDFREQUEST request, WDFFILEOBJECT file)
{
	PW_FILE_CTX *fc = PwFileCtx(file);

	fc->dev = PwDeviceCtx(device);
	fc->pid = PsGetCurrentProcessId();
	WdfRequestComplete(request, STATUS_SUCCESS);
}

VOID pw_win_file_cleanup(WDFFILEOBJECT file)
{
	/* Last handle closed, still in the owning process: later milestones unmap user BO
	 * mappings and release GEM/VM/group/syncobj handles here. */
	(void)file;
}

VOID pw_win_file_close(WDFFILEOBJECT file)
{
	(void)file;
}

/* ------------------------------------------------------------- DRM ioctl table */

typedef int (*pw_drm_fn)(PW_DEVICE_CTX *ctx, WDFREQUEST req, void *arg);

struct pw_drm_ioctl {
	ULONG cmd;      /* Linux DRM_IOCTL_* number: nr, size and direction */
	pw_drm_fn fn;
	const char *name;
};

static int pw_drm_get_cap(PW_DEVICE_CTX *ctx, WDFREQUEST req, void *arg);
static int pw_drm_dev_query(PW_DEVICE_CTX *ctx, WDFREQUEST req, void *arg);

/* DRM_IOCTL_VERSION is answered in caller context (pw_drm_version) and never reaches here. */
static const struct pw_drm_ioctl pw_drm_ioctls[] = {
	{ (ULONG)DRM_IOCTL_GET_CAP, pw_drm_get_cap, "GET_CAP" },
	{ (ULONG)DRM_IOCTL_PANTHOR_DEV_QUERY, pw_drm_dev_query, "PANTHOR_DEV_QUERY" },
};

static const struct pw_drm_ioctl *pw_drm_lookup(ULONG nr)
{
	for (size_t i = 0; i < sizeof(pw_drm_ioctls) / sizeof(pw_drm_ioctls[0]); i++)
		if (_IOC_NR(pw_drm_ioctls[i].cmd) == nr)
			return &pw_drm_ioctls[i];
	return NULL;
}

/* Copy the ioctl struct from the METHOD_BUFFERED system buffer, zero-extended to ksize. */
static NTSTATUS pw_drm_read_args(WDFREQUEST req, size_t in_len, void *karg, size_t ksize)
{
	void *in;
	size_t n;
	NTSTATUS st;

	RtlZeroMemory(karg, ksize);
	if (!in_len)
		return STATUS_SUCCESS;
	st = WdfRequestRetrieveInputBuffer(req, 1, &in, &n);
	if (!NT_SUCCESS(st))
		return st;
	RtlCopyMemory(karg, in, n < ksize ? n : ksize);
	return STATUS_SUCCESS;
}

/* Copy the (updated) struct back; returns the byte count for the request's Information. */
static size_t pw_drm_write_args(WDFREQUEST req, size_t out_len, const void *karg, size_t ksize)
{
	void *out;
	size_t n;

	if (!out_len || !NT_SUCCESS(WdfRequestRetrieveOutputBuffer(req, 1, &out, &n)))
		return 0;
	n = n < ksize ? n : ksize;
	RtlCopyMemory(out, karg, n);
	return n;
}

static void pw_complete_errno(WDFREQUEST req, int err, size_t info)
{
	WdfRequestCompleteWithInformation(req, pw_win_status_from_errno(err), err ? 0 : info);
}

/* ------------------------------------------------- caller-context stage (nested ptrs) */

/* drm_copy_field(): copy min(len, *buf_len) bytes (no NUL), report the full length. */
static NTSTATUS pw_copy_field(KPROCESSOR_MODE mode, char *ubuf, size_t *ulen, const char *value)
{
	size_t len = strlen(value);
	size_t n = len < *ulen ? len : *ulen;

	*ulen = len;
	if (n && ubuf)
		return pw_win_copy_to_user(mode, ubuf, value, n);
	return STATUS_SUCCESS;
}

static void pw_drm_version(WDFREQUEST req, size_t in_len, size_t out_len)
{
	struct drm_version v;
	KPROCESSOR_MODE mode = WdfRequestGetRequestorMode(req);
	NTSTATUS st;
	size_t info;

	/* struct drm_version holds size_t and char *: only the 64-bit layout is supported. */
	if (WdfRequestIsFrom32BitProcess(req)) {
		WdfRequestComplete(req, STATUS_NOT_SUPPORTED);
		return;
	}
	st = pw_drm_read_args(req, in_len, &v, sizeof(v));
	if (!NT_SUCCESS(st)) {
		WdfRequestComplete(req, st);
		return;
	}
	v.version_major = PW_DRM_VERSION_MAJOR;
	v.version_minor = PW_DRM_VERSION_MINOR;
	v.version_patchlevel = PW_DRM_VERSION_PATCH;
	st = pw_copy_field(mode, v.name, &v.name_len, PW_DRM_NAME);
	if (NT_SUCCESS(st))
		st = pw_copy_field(mode, v.date, &v.date_len, PW_DRM_DATE);
	if (NT_SUCCESS(st))
		st = pw_copy_field(mode, v.desc, &v.desc_len, PW_DRM_DESC);
	if (!NT_SUCCESS(st)) {
		WdfRequestComplete(req, st);
		return;
	}
	info = pw_drm_write_args(req, out_len, &v, sizeof(v));
	WdfRequestCompleteWithInformation(req, STATUS_SUCCESS, info);
}

/* Lock DEV_QUERY's output object (pointer/size) for the queue stage. */
static NTSTATUS pw_dev_query_prepare(WDFREQUEST req, size_t in_len)
{
	struct drm_panthor_dev_query q;
	PW_REQ_CTX *rc = PwReqCtx(req);
	WDF_OBJECT_ATTRIBUTES attr;
	NTSTATUS st;

	st = pw_drm_read_args(req, in_len, &q, sizeof(q));
	if (!NT_SUCCESS(st))
		return st;
	if (!q.pointer || !q.size)
		return STATUS_SUCCESS; /* size query, or size 0 (rejected by the queue stage) */
	if (q.size > PW_UOBJ_MAX)
		return STATUS_INVALID_PARAMETER;
	if (WdfRequestGetRequestorMode(req) == UserMode) {
		st = WdfRequestProbeAndLockUserBufferForWrite(req, (PVOID)(ULONG_PTR)q.pointer,
							      q.size, &rc->user_out);
	} else {
		WDF_OBJECT_ATTRIBUTES_INIT(&attr);
		attr.ParentObject = req;
		st = WdfMemoryCreatePreallocated(&attr, (PVOID)(ULONG_PTR)q.pointer, q.size,
						 &rc->user_out);
	}
	if (!NT_SUCCESS(st)) {
		rc->user_out = NULL;
		return STATUS_ACCESS_VIOLATION; /* -EFAULT */
	}
	rc->user_out_len = q.size;
	return STATUS_SUCCESS;
}

VOID pw_win_io_in_caller_context(WDFDEVICE device, WDFREQUEST req)
{
	WDF_REQUEST_PARAMETERS p;
	NTSTATUS st;

	WDF_REQUEST_PARAMETERS_INIT(&p);
	WdfRequestGetParameters(req, &p);
	if (p.Type == WdfRequestTypeDeviceControl) {
		ULONG code = p.Parameters.DeviceIoControl.IoControlCode;
		size_t in_len = p.Parameters.DeviceIoControl.InputBufferLength;
		size_t out_len = p.Parameters.DeviceIoControl.OutputBufferLength;

		if (code == PANWIN_IOCTL_DRM(_IOC_NR(DRM_IOCTL_VERSION))) {
			pw_drm_version(req, in_len, out_len);
			return;
		}
		if (code == PANWIN_IOCTL_DRM(_IOC_NR(DRM_IOCTL_PANTHOR_DEV_QUERY))) {
			st = pw_dev_query_prepare(req, in_len);
			if (!NT_SUCCESS(st)) {
				WdfRequestComplete(req, st);
				return;
			}
		}
	}
	st = WdfDeviceEnqueueRequest(device, req);
	if (!NT_SUCCESS(st))
		WdfRequestComplete(req, st);
}

/* --------------------------------------------------------------- queue stage */

static int pw_drm_get_cap(PW_DEVICE_CTX *ctx, WDFREQUEST req, void *arg)
{
	struct drm_get_cap *c = arg;

	(void)ctx;
	(void)req;
	/* Linux drm_getcap() for a render-only driver; no PRIME (dma-buf) on Windows and no
	 * syncobjs yet, so those report 0. */
	switch (c->capability) {
	case DRM_CAP_TIMESTAMP_MONOTONIC:
		c->value = 1;
		return PW_OK;
	case DRM_CAP_PRIME:
	case DRM_CAP_SYNCOBJ:
	case DRM_CAP_SYNCOBJ_TIMELINE:
		c->value = 0;
		return PW_OK;
	default:
		c->value = 0;
		return PW_EOPNOTSUPP; /* "other caps only work with KMS drivers" */
	}
}

/* panthor_set_uobj(): user size >= minimal (uAPI 1.0) size; copy min(user, kernel) and zero
 * the rest of a larger user object. */
static int pw_set_uobj(WDFREQUEST req, uint32_t usize, size_t min_size, const void *src,
		       size_t ksize)
{
	PW_REQ_CTX *rc = PwReqCtx(req);
	size_t len;
	UCHAR *dst;

	if (usize < min_size)
		return PW_EINVAL;
	if (!rc->user_out || rc->user_out_len != usize)
		return PW_EFAULT;
	dst = WdfMemoryGetBuffer(rc->user_out, &len);
	if (!dst || len < usize)
		return PW_EFAULT;
	RtlCopyMemory(dst, src, usize < ksize ? usize : ksize);
	if (usize > ksize)
		RtlZeroMemory(dst + ksize, usize - ksize);
	return PW_OK;
}

static int pw_drm_dev_query(PW_DEVICE_CTX *ctx, WDFREQUEST req, void *arg)
{
	struct drm_panthor_dev_query *q = arg;
	struct pw_device *pw = pw_win_core(ctx);

	/* Types beyond GPU_INFO/CSIF_INFO are gated behind DRM minor >= 1 in Mesa and are
	 * rejected like panthor 1.0 did. */
	if (!q->pointer) {
		switch (q->type) {
		case DRM_PANTHOR_DEV_QUERY_GPU_INFO:
			q->size = sizeof(struct drm_panthor_gpu_info);
			return PW_OK;
		case DRM_PANTHOR_DEV_QUERY_CSIF_INFO:
			q->size = sizeof(struct drm_panthor_csif_info);
			return PW_OK;
		default:
			return PW_EINVAL;
		}
	}
	if (!pw)
		return PW_WIN_ENODEV;
	switch (q->type) {
	case DRM_PANTHOR_DEV_QUERY_GPU_INFO: {
		struct drm_panthor_gpu_info info;

		RtlZeroMemory(&info, sizeof(info));
		pw_device_get_gpu_info(pw, &info);
		return pw_set_uobj(req, q->size,
				   PW_OFFSETOFEND(struct drm_panthor_gpu_info, tiler_present),
				   &info, sizeof(info));
	}
	case DRM_PANTHOR_DEV_QUERY_CSIF_INFO: {
		struct drm_panthor_csif_info info;
		int ret;

		RtlZeroMemory(&info, sizeof(info));
		ret = pw_device_get_csif_info(pw, &info);
		if (ret)
			return ret;
		return pw_set_uobj(req, q->size, PW_OFFSETOFEND(struct drm_panthor_csif_info, pad),
				   &info, sizeof(info));
	}
	default:
		return PW_EINVAL;
	}
}

static void pw_win_drm_ioctl(PW_DEVICE_CTX *ctx, WDFREQUEST req, ULONG nr, size_t in_len,
			     size_t out_len)
{
	const struct pw_drm_ioctl *ioc = pw_drm_lookup(nr);
	ULONG64 karg[PW_ARG_MAX / sizeof(ULONG64)];
	size_t ksize, info = 0;
	NTSTATUS st;
	int ret;

	if (!ioc) {
		WdfRequestComplete(req, STATUS_NOT_SUPPORTED);
		return;
	}
	ksize = _IOC_SIZE(ioc->cmd);
	st = pw_drm_read_args(req, (_IOC_DIR(ioc->cmd) & _IOC_WRITE) ? in_len : 0, karg, ksize);
	if (!NT_SUCCESS(st)) {
		WdfRequestComplete(req, st);
		return;
	}
	ret = ioc->fn(ctx, req, karg);
	if (!ret && (_IOC_DIR(ioc->cmd) & _IOC_READ))
		info = pw_drm_write_args(req, out_len, karg, ksize);
	if (ret)
		PW_WLOG(PW_LOG_DBG, "DRM %s: %d", ioc->name, ret);
	pw_complete_errno(req, ret, info);
}

/* ---------------------------------------------------------------- private ioctls */

static void pw_win_get_info(PW_DEVICE_CTX *ctx, WDFREQUEST req)
{
	struct panwin_info *info;
	struct pw_device *pw = pw_win_core(ctx);
	NTSTATUS st;

	st = WdfRequestRetrieveOutputBuffer(req, sizeof(*info), (PVOID *)&info, NULL);
	if (!NT_SUCCESS(st)) {
		WdfRequestComplete(req, st);
		return;
	}
	RtlZeroMemory(info, sizeof(*info));
	info->abi_version = PANWIN_ABI_VERSION;
	info->flags = (PANWIN_DEBUG_IOCTLS ? PANWIN_INFO_DEBUG_BUILD : 0) |
		      ((pw && pw_device_fw_booted(pw)) ? PANWIN_INFO_FW_BOOTED : 0);
	info->drm_version_major = PW_DRM_VERSION_MAJOR;
	info->drm_version_minor = PW_DRM_VERSION_MINOR;
	info->gpu_phys_base = (uint64_t)ctx->os.mmio_pa.QuadPart;
	if (pw)
		pw_device_describe(pw, info->description, sizeof(info->description));
	else
		pw_win_snprintf(info->description, sizeof(info->description),
			    "no core device: %s, core status %d",
			    ctx->gpu_powered ? "GPU powered" : "GPU power domain OFF",
			    ctx->core_status);
	info->description[sizeof(info->description) - 1] = 0;
	WdfRequestCompleteWithInformation(req, STATUS_SUCCESS, sizeof(*info));
}

#if PANWIN_DEBUG_IOCTLS
static void pw_win_debug_reg_read(PW_DEVICE_CTX *ctx, WDFREQUEST req)
{
	struct panwin_debug_reg_read *in, *out;
	uint32_t off;
	NTSTATUS st;

	st = WdfRequestRetrieveInputBuffer(req, sizeof(*in), (PVOID *)&in, NULL);
	if (NT_SUCCESS(st))
		st = WdfRequestRetrieveOutputBuffer(req, sizeof(*out), (PVOID *)&out, NULL);
	if (!NT_SUCCESS(st)) {
		WdfRequestComplete(req, st);
		return;
	}
	off = in->offset; /* in and out share the system buffer: read before writing */
	if (!ctx->os.regs_enabled) {
		WdfRequestComplete(req, STATUS_DEVICE_NOT_READY); /* -ENODEV */
		return;
	}
	if ((off & 3) || off > ctx->os.mmio_len - 4) {
		WdfRequestComplete(req, STATUS_INVALID_PARAMETER);
		return;
	}
	out->offset = off;
	out->value = pw_os_reg_read32(&ctx->os, off);
	WdfRequestCompleteWithInformation(req, STATUS_SUCCESS, sizeof(*out));
}
#endif

static VOID pw_win_io_device_control(WDFQUEUE queue, WDFREQUEST req, size_t out_len,
				     size_t in_len, ULONG code)
{
	PW_DEVICE_CTX *ctx = PwDeviceCtx(WdfIoQueueGetDevice(queue));
	ULONG func = (code >> 2) & 0xFFF;

	if (func >= PANWIN_FUNC_DRM_BASE && func <= PANWIN_FUNC_DRM_BASE + 0xFF &&
	    code == PANWIN_IOCTL_DRM(func - PANWIN_FUNC_DRM_BASE)) {
		pw_win_drm_ioctl(ctx, req, func - PANWIN_FUNC_DRM_BASE, in_len, out_len);
		return;
	}
	switch (code) {
	case PANWIN_IOCTL_GET_INFO:
		pw_win_get_info(ctx, req);
		return;
#if PANWIN_DEBUG_IOCTLS
	case PANWIN_IOCTL_DEBUG_REG_READ:
		pw_win_debug_reg_read(ctx, req);
		return;
#endif
	default:
		/* MMAP/MUNMAP/SYNCOBJ_EVENT arrive with later milestones. */
		WdfRequestComplete(req, STATUS_NOT_SUPPORTED);
		return;
	}
}

NTSTATUS pw_win_queue_init(PW_DEVICE_CTX *ctx)
{
	WDF_IO_QUEUE_CONFIG qcfg;
	WDF_OBJECT_ATTRIBUTES attr;
	NTSTATUS st;

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchSequential);
	qcfg.EvtIoDeviceControl = pw_win_io_device_control;
	WDF_OBJECT_ATTRIBUTES_INIT(&attr);
	attr.ExecutionLevel = WdfExecutionLevelPassive; /* the core takes mutexes and waits */
	st = WdfIoQueueCreate(ctx->device, &qcfg, &attr, WDF_NO_HANDLE);
	if (!NT_SUCCESS(st))
		PW_WLOG(PW_LOG_ERR, "WdfIoQueueCreate failed: 0x%08x", (unsigned)st);
	return st;
}
