/* SPDX-License-Identifier: MIT */
/*
 * libdrm-win runtime tests against the in-process fake kernel (built with -Dfake_kernel=true).
 * Exercises the public libdrm API the way Mesa's panthor_kmod.c / u_sync_provider.c do.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include <xf86drm.h>
#include "drm-uapi/panthor_drm.h"
#include "panwin/panwin_ioctl.h"
#include "libdrm_win.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #c, errno); failures++; } } while (0)

static int64_t now_ns(void)
{
	LARGE_INTEGER c, f;
	QueryPerformanceCounter(&c);
	QueryPerformanceFrequency(&f);
	return (int64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}

static void test_errno_map(void)
{
	CHECK(panwin_ntstatus_to_errno(0) == 0);
	CHECK(panwin_ntstatus_to_errno((int32_t)0xC000000Du) == EINVAL);
	CHECK(panwin_ntstatus_to_errno((int32_t)0xC0000010u) == ENOTTY);
	CHECK(panwin_ntstatus_to_errno((int32_t)0xC000009Au) == ENOMEM);
	CHECK(panwin_ntstatus_to_errno((int32_t)0xC00000B5u) == ETIME);
	CHECK(panwin_ntstatus_to_errno((int32_t)0x00000102u) == ETIME);
	CHECK(panwin_ntstatus_to_errno((int32_t)0xC00000F1u) == EINVAL);
	/* ABI v2: Linux errno carried in the customer NTSTATUS */
	CHECK((uint32_t)PANWIN_STATUS_FROM_ERRNO(62) == 0xED52003Eu);
	CHECK(PANWIN_STATUS_IS_ERRNO(PANWIN_STATUS_FROM_ERRNO(4095)) &&
	      PANWIN_STATUS_TO_ERRNO(PANWIN_STATUS_FROM_ERRNO(4095)) == 4095);
	CHECK(!PANWIN_STATUS_IS_ERRNO(0xC000000Du));
	CHECK(panwin_ntstatus_to_errno(PANWIN_STATUS_FROM_ERRNO(62)) == ETIME);
	CHECK(panwin_ntstatus_to_errno(PANWIN_STATUS_FROM_ERRNO(110)) == ETIMEDOUT); /* distinct */
	CHECK(panwin_ntstatus_to_errno(PANWIN_STATUS_FROM_ERRNO(38)) == ENOSYS);
	CHECK(panwin_ntstatus_to_errno(PANWIN_STATUS_FROM_ERRNO(95)) == EOPNOTSUPP);
	CHECK(panwin_ntstatus_to_errno(PANWIN_STATUS_FROM_ERRNO(4)) == EINTR);
	CHECK(panwin_ntstatus_to_errno(PANWIN_STATUS_FROM_ERRNO(19)) == ENODEV);
	CHECK(panwin_ntstatus_to_errno(PANWIN_STATUS_FROM_ERRNO(25)) == ENOTTY);
	CHECK(panwin_ntstatus_to_errno(PANWIN_STATUS_FROM_ERRNO(9)) == EBADF);
	/* Inverse of kmd/win/pw_win_ioctl.c:pw_win_status_from_errno(). */
	CHECK(panwin_ntstatus_to_errno((int32_t)0xC00000A3u) == ENODEV);  /* DEVICE_NOT_READY */
	CHECK(panwin_ntstatus_to_errno((int32_t)0xC0000044u) == ENOSPC);  /* QUOTA_EXCEEDED */
	CHECK(panwin_ntstatus_to_errno((int32_t)0xC0000185u) == EIO);     /* IO_DEVICE_ERROR */
	CHECK(panwin_ntstatus_to_errno((int32_t)0x80000011u) == EBUSY);   /* DEVICE_BUSY */
	CHECK(panwin_ntstatus_to_errno((int32_t)0x8000000Fu) == ENODEV);  /* DEVICE_POWERED_OFF */
	CHECK(panwin_ntstatus_to_errno((int32_t)0xC0000225u) == ENOENT);  /* NOT_FOUND */
	CHECK(panwin_ntstatus_to_errno(PANWIN_STATUS_FROM_ERRNO(35)) == EDEADLK);
	CHECK(panwin_win32_error_to_errno(ERROR_FILE_NOT_FOUND) == ENOENT);
	CHECK(panwin_win32_error_to_errno(ERROR_ACCESS_DENIED) == EACCES);
	CHECK(panwin_win32_error_to_errno(ERROR_SEM_TIMEOUT) == ETIME);
}

static void test_devices(void)
{
	drmDevicePtr devs[8];
	int n = drmGetDevices2(0, NULL, 0);
	CHECK(n == 1);
	n = drmGetDevices2(0, devs, 8);
	CHECK(n == 1);
	if (n >= 1) {
		CHECK(devs[0]->bustype == DRM_BUS_PLATFORM);
		CHECK(devs[0]->available_nodes == (1 << DRM_NODE_RENDER));
		CHECK(!strcmp(devs[0]->nodes[DRM_NODE_RENDER], "/dev/dri/renderD128"));
		CHECK(!strcmp(devs[0]->deviceinfo.platform->compatible[0], "rockchip,rk3588-mali"));
		CHECK(devs[0]->deviceinfo.platform->compatible[2] == NULL);
		uint64_t rdev = 0;
		CHECK(panwin_stat_rdev(devs[0]->nodes[DRM_NODE_RENDER], &rdev) == 0);
		CHECK(rdev == ((226u << 8) | 128u));
		drmDevicePtr d2 = NULL;
		CHECK(drmGetDeviceFromDevId((dev_t)rdev, 0, &d2) == 0);
		CHECK(d2 && drmDevicesEqual(devs[0], d2));
		CHECK(drmGetNodeTypeFromDevId((dev_t)rdev) == DRM_NODE_RENDER);
		drmFreeDevice(&d2);
		CHECK(d2 == NULL);
	}
	drmFreeDevices(devs, n);
	CHECK(panwin_stat_rdev("/dev/dri/renderD129", NULL) == -1 && errno == ENOENT);
	CHECK(panwin_open("/dev/dri/renderD129", 0) == -1 && errno == ENOENT);
	CHECK(panwin_open("C:\\foo", 0) == -1 && errno == ENOENT);
}

static void test_fds(int fd)
{
	CHECK(fd >= PANWIN_FD_BASE);
	CHECK(panwin_is_fd(fd));
	CHECK(panwin_fd_to_handle(fd) != NULL);

	int fd2 = panwin_dup(fd);
	CHECK(fd2 >= PANWIN_FD_BASE && fd2 != fd);

	/* Handles are shared across dup'ed fds (same kernel file), like Linux dup(). */
	uint32_t h = 0;
	CHECK(drmSyncobjCreate(fd, DRM_SYNCOBJ_CREATE_SIGNALED, &h) == 0);
	CHECK(drmSyncobjWait(fd2, &h, 1, 0, 0, NULL) == 0);
	CHECK(drmSyncobjDestroy(fd2, h) == 0);
	CHECK(panwin_close(fd2) == 0);
	CHECK(panwin_close(fd2) == -1 && errno == EBADF);
	CHECK(!panwin_is_fd(fd2));

	drmDevicePtr d = NULL;
	CHECK(drmGetDevice2(fd, 0, &d) == 0 && d && d->bustype == DRM_BUS_PLATFORM);
	drmFreeDevice(&d);
	char *name = drmGetDeviceNameFromFd2(fd);
	CHECK(name && !strcmp(name, "/dev/dri/renderD128"));
	free(name);
	CHECK(drmGetNodeTypeFromFd(fd) == DRM_NODE_RENDER);
}

static void test_version_caps(int fd)
{
	drmVersionPtr v = drmGetVersion(fd);
	CHECK(v != NULL);
	if (v) {
		CHECK(!strcmp(v->name, "panthor"));
		CHECK(v->name_len == 7);
		CHECK(v->version_major == 1 && v->version_minor == 5 && v->version_patchlevel == 0);
		CHECK(v->desc && strlen(v->desc) == (size_t)v->desc_len);
		drmFreeVersion(v);
	}
	uint64_t cap = 0;
	CHECK(drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) == 0 && cap == 1);
	CHECK(drmGetCap(fd, DRM_CAP_SYNCOBJ, &cap) == 0 && cap == 1);
	CHECK(drmGetCap(fd, 0xdead, &cap) == -1 && errno == EINVAL);
	CHECK(drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0);
	CHECK(drmGetVersion(0x7fff) == NULL && errno == EBADF);
}

static void test_dev_query(int fd)
{
	struct drm_panthor_dev_query q = { .type = DRM_PANTHOR_DEV_QUERY_GPU_INFO };
	struct drm_panthor_gpu_info gpu;

	/* Two-pass like panthor_kmod.c: size first, then data. */
	CHECK(drmIoctl(fd, DRM_IOCTL_PANTHOR_DEV_QUERY, &q) == 0);
	CHECK(q.size == sizeof(gpu));
	memset(&gpu, 0xab, sizeof(gpu));
	q.pointer = (uint64_t)(uintptr_t)&gpu;
	CHECK(drmIoctl(fd, DRM_IOCTL_PANTHOR_DEV_QUERY, &q) == 0);
	CHECK(DRM_PANTHOR_ARCH_MAJOR(gpu.gpu_id) == 10);
	CHECK(DRM_PANTHOR_ARCH_MINOR(gpu.gpu_id) == 8);
	CHECK(DRM_PANTHOR_PRODUCT_MAJOR(gpu.gpu_id) == 7);
	CHECK(DRM_PANTHOR_MMU_VA_BITS(gpu.mmu_features) == 48);
	CHECK(gpu.shader_present == 0x50005 && gpu.pad == 0);

	struct drm_panthor_csif_info csif;
	q.type = DRM_PANTHOR_DEV_QUERY_CSIF_INFO;
	q.size = sizeof(csif);
	q.pointer = (uint64_t)(uintptr_t)&csif;
	CHECK(drmIoctl(fd, DRM_IOCTL_PANTHOR_DEV_QUERY, &q) == 0);
	CHECK(csif.csg_slot_count == 8 && csif.cs_slot_count == 8);

	q.type = DRM_PANTHOR_DEV_QUERY_MMU_INFO; /* uAPI 1.9: not offered at 1.5 */
	CHECK(drmIoctl(fd, DRM_IOCTL_PANTHOR_DEV_QUERY, &q) == -1 && errno == EINVAL);

	/* Unknown driver ioctl number -> ENOTTY, like an unknown ioctl on Linux. */
	CHECK(drmIoctl(fd, DRM_IOWR(DRM_COMMAND_BASE + 0x3f, struct drm_panthor_dev_query), &q) == -1 &&
	      errno == ENOTTY);
	/* Size mismatch (struct from a different uAPI version) is rejected by the fake. */
	CHECK(drmIoctl(fd, DRM_IOWR(DRM_COMMAND_BASE, struct drm_panthor_csif_info), &csif) == -1 &&
	      errno == EINVAL);

	struct panwin_info info;
	CHECK(panwin_private_ioctl(fd, PANWIN_IOCTL_GET_INFO, &info, sizeof(info)) == 0);
	CHECK(info.abi_version == PANWIN_ABI_VERSION && info.drm_version_minor == 5);
	struct panwin_debug_reg_read rr = { .offset = 0 };
	CHECK(panwin_private_ioctl(fd, PANWIN_IOCTL_DEBUG_REG_READ, &rr, sizeof(rr)) == 0);
	CHECK(rr.value == gpu.gpu_id);
}

static void test_syncobj(int fd)
{
	uint32_t a = 0, b = 0, t = 0, first = 99;

	CHECK(drmSyncobjCreate(fd, 0, &a) == 0 && a != 0);
	CHECK(drmSyncobjCreate(fd, DRM_SYNCOBJ_CREATE_SIGNALED, &b) == 0 && b != a);

	/* Unsubmitted binary syncobj without WAIT_FOR_SUBMIT: EINVAL (DRM semantics). */
	CHECK(drmSyncobjWait(fd, &a, 1, 0, 0, NULL) == -EINVAL);
	/* With WAIT_FOR_SUBMIT and an expired deadline: -ETIME. */
	CHECK(drmSyncobjWait(fd, &a, 1, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL) == -ETIME);
	int64_t t0 = now_ns();
	CHECK(drmSyncobjWait(fd, &a, 1, t0 + 50000000, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL) == -ETIME);
	CHECK(now_ns() - t0 >= 40000000); /* actually waited ~50 ms */

	uint32_t both[2] = { a, b };
	CHECK(drmSyncobjWait(fd, both, 2, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, &first) == 0 && first == 1);
	CHECK(drmSyncobjWait(fd, both, 2, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
			     DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL) == -ETIME);
	CHECK(drmSyncobjSignal(fd, &a, 1) == 0);
	CHECK(drmSyncobjWait(fd, both, 2, INT64_MAX, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL) == 0);
	CHECK(drmSyncobjReset(fd, both, 2) == 0);
	CHECK(drmSyncobjWait(fd, &b, 1, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL) == -ETIME);

	/* Timeline (mandatory for panvk on CSF). */
	CHECK(drmSyncobjCreate(fd, 0, &t) == 0);
	uint64_t p = 5, q = 0;
	CHECK(drmSyncobjTimelineSignal(fd, &t, &p, 1) == 0);
	CHECK(drmSyncobjQuery(fd, &t, &q, 1) == 0 && q == 5);
	CHECK(drmSyncobjQuery2(fd, &t, &q, 1, DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) == 0 && q == 5);
	p = 3;
	CHECK(drmSyncobjTimelineWait(fd, &t, &p, 1, 0, 0, NULL) == 0);
	p = 6;
	CHECK(drmSyncobjTimelineWait(fd, &t, &p, 1, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL) == -ETIME);
	CHECK(drmSyncobjTimelineWait(fd, &t, &p, 1, 0, 0, NULL) == -EINVAL);

	/* Transfer timeline point 5 -> binary a, and binary b(signalled) -> timeline point 9. */
	CHECK(drmSyncobjTransfer(fd, a, 0, t, 5, 0) == 0);
	CHECK(drmSyncobjWait(fd, &a, 1, 0, 0, NULL) == 0);
	CHECK(drmSyncobjSignal(fd, &b, 1) == 0);
	CHECK(drmSyncobjTransfer(fd, t, 9, b, 0, 0) == 0);
	CHECK(drmSyncobjQuery(fd, &t, &q, 1) == 0 && q == 9);
	CHECK(drmSyncobjTransfer(fd, a, 0, t, 10, 0) == -1 && errno == EINVAL);

	/* Event replacement for poll()/eventfd (ABI v2: the event is created in user mode). */
	void *ev = NULL;
	p = 12;
	/* No fence for point 12 and no WAIT_FOR_SUBMIT: EINVAL, like WAIT. */
	CHECK(panwin_syncobj_event(fd, t, 12, 0, &ev) == -1 && errno == EINVAL && !ev);
	CHECK(panwin_syncobj_event(fd, 0xdead, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, &ev) == -1 &&
	      errno == EINVAL);
	/* Already signalled point: set immediately. */
	CHECK(panwin_syncobj_event(fd, t, 9, 0, &ev) == 0 && ev);
	if (ev) {
		CHECK(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0);
		CloseHandle(ev);
		ev = NULL;
	}
	CHECK(panwin_syncobj_event(fd, t, 12, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, &ev) == 0 && ev);
	if (ev) {
		CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT);
		CHECK(drmSyncobjTimelineSignal(fd, &t, &p, 1) == 0);
		CHECK(WaitForSingleObject(ev, 1000) == WAIT_OBJECT_0);
		CloseHandle(ev);
	}

	/* No fd-based sharing on Windows yet. */
	int ofd = -1;
	CHECK(drmSyncobjHandleToFD(fd, t, &ofd) == -1 && errno == ENOSYS);
	CHECK(drmSyncobjExportSyncFile(fd, t, &ofd) == -1 && errno == ENOSYS);
	CHECK(drmSyncobjImportSyncFile(fd, t, 3) == -1 && errno == ENOSYS);

	CHECK(drmSyncobjDestroy(fd, a) == 0);
	CHECK(drmSyncobjDestroy(fd, a) == -1 && errno == EINVAL);
	CHECK(drmSyncobjDestroy(fd, b) == 0);
	CHECK(drmSyncobjDestroy(fd, t) == 0);
	CHECK(drmSyncobjWait(fd, &a, 1, 0, 0, NULL) == -ENOENT);
}

struct waiter_ctx {
	int fd;
	uint32_t h;
	int result;
};

static DWORD WINAPI waiter_thread(void *p)
{
	struct waiter_ctx *w = p;
	uint64_t pt = 2;
	w->result = drmSyncobjTimelineWait(w->fd, &w->h, &pt, 1, now_ns() + 5000000000ll,
					   DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL);
	return 0;
}

static void test_threads(int fd)
{
	struct waiter_ctx w = { .fd = fd, .result = 1 };
	HANDLE th[4];
	CHECK(drmSyncobjCreate(fd, 0, &w.h) == 0);
	struct waiter_ctx ws[4];
	for (int i = 0; i < 4; i++) {
		ws[i] = w;
		th[i] = CreateThread(NULL, 0, waiter_thread, &ws[i], 0, NULL);
	}
	Sleep(50);
	uint64_t p1 = 1, p2 = 2;
	CHECK(drmSyncobjTimelineSignal(fd, &w.h, &p1, 1) == 0);
	Sleep(20);
	CHECK(WaitForMultipleObjects(4, th, TRUE, 0) == WAIT_TIMEOUT); /* point 1 < 2: still waiting */
	CHECK(drmSyncobjTimelineSignal(fd, &w.h, &p2, 1) == 0);
	CHECK(WaitForMultipleObjects(4, th, TRUE, 5000) == WAIT_OBJECT_0);
	for (int i = 0; i < 4; i++) {
		CHECK(ws[i].result == 0);
		CloseHandle(th[i]);
	}
	CHECK(drmSyncobjDestroy(fd, w.h) == 0);
}

static void test_bo_mmap(int fd)
{
	struct drm_panthor_vm_create vc = { 0 };
	CHECK(drmIoctl(fd, DRM_IOCTL_PANTHOR_VM_CREATE, &vc) == 0 && vc.id != 0);

	struct drm_panthor_bo_create bc = { .size = 10000, .exclusive_vm_id = vc.id };
	CHECK(drmIoctl(fd, DRM_IOCTL_PANTHOR_BO_CREATE, &bc) == 0);
	CHECK(bc.handle != 0 && bc.size == 12288);

	struct drm_panthor_bo_mmap_offset mo = { .handle = bc.handle };
	CHECK(drmIoctl(fd, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &mo) == 0);

	uint32_t *m1 = panwin_mmap(fd, mo.offset, (size_t)bc.size, 3);
	uint32_t *m2 = panwin_mmap(fd, mo.offset, (size_t)bc.size, 1);
	CHECK(m1 && m2 && m1 != m2);
	if (m1 && m2) {
		CHECK(panwin_is_mapping(m1) && panwin_is_mapping(m2));
		m1[0] = 0x12345678;
		m1[3000] = 0xcafe;
		CHECK(m2[0] == 0x12345678 && m2[3000] == 0xcafe); /* same pages */
	}
	CHECK(drmCloseBufferHandle(fd, bc.handle) == 0);
	CHECK(drmCloseBufferHandle(fd, bc.handle) == -1 && errno == EINVAL);
	if (m1)
		CHECK(m1[3000] == 0xcafe); /* mapping outlives the GEM handle, as on Linux */
	CHECK(panwin_munmap(m1, (size_t)bc.size) == 0);
	CHECK(panwin_munmap(m1, (size_t)bc.size) == -1 && errno == EINVAL);
	CHECK(panwin_munmap(m2, (size_t)bc.size) == 0);
	CHECK(!panwin_is_mapping(m1));

	/* User MMIO flush-id page (panthor_kmod.c maps it unconditionally). */
	struct drm_panthor_set_user_mmio_offset so = { .offset = DRM_PANTHOR_USER_MMIO_OFFSET };
	CHECK(drmIoctl(fd, DRM_IOCTL_PANTHOR_SET_USER_MMIO_OFFSET, &so) == 0);
	CHECK(DRM_PANTHOR_USER_MMIO_OFFSET == DRM_PANTHOR_USER_MMIO_OFFSET_32BIT); /* LLP64! */
	volatile uint32_t *flush = panwin_mmap(fd, DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET, 4096, 1);
	CHECK(flush && *flush == 0x00c0ffee);
	CHECK(panwin_mmap(fd, DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET, 4096, 3) == NULL && errno == EINVAL);
	CHECK(panwin_munmap((void *)flush, 4096) == 0);
	CHECK(panwin_mmap(fd, 0x123, 4096, 1) == NULL && errno == EINVAL);

	struct drm_panthor_vm_destroy vd = { .id = vc.id };
	CHECK(drmIoctl(fd, DRM_IOCTL_PANTHOR_VM_DESTROY, &vd) == 0);
}

/* A mapping keeps the kernel file alive after the last fd is closed (Linux semantics). */
static void test_mapping_lifetime(void)
{
	int fd = panwin_open("/dev/dri/renderD128", 0);
	CHECK(fd >= 0);
	struct drm_panthor_bo_create bc = { .size = 4096 };
	CHECK(drmIoctl(fd, DRM_IOCTL_PANTHOR_BO_CREATE, &bc) == 0);
	struct drm_panthor_bo_mmap_offset mo = { .handle = bc.handle };
	CHECK(drmIoctl(fd, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &mo) == 0);
	uint32_t *m = panwin_mmap(fd, mo.offset, 4096, 3);
	CHECK(m != NULL);
	CHECK(panwin_close(fd) == 0);
	if (m) {
		m[1] = 42; /* still mapped */
		CHECK(m[1] == 42);
		CHECK(panwin_munmap(m, 4096) == 0);
	}
}

static void test_prime(int fd)
{
	int pfd;
	uint32_t h;
	CHECK(drmPrimeHandleToFD(fd, 1, 0 /* DRM_CLOEXEC: O_CLOEXEC is undefined on mingw */, &pfd) == -1 && errno == ENOSYS);
	CHECK(drmPrimeFDToHandle(fd, 5, &h) == -1 && errno == ENOSYS);
	CHECK(drmPrimeHandleToFD(3, 1, 0, &pfd) == -1 && errno == EBADF);
}

static void test_fd_exhaustion(void)
{
	static int fds[PANWIN_FD_MAX + 1];
	int n = 0;
	int base = panwin_open("/dev/dri/renderD128", 0);
	CHECK(base >= 0);
	for (; n < PANWIN_FD_MAX + 1; n++) {
		fds[n] = panwin_dup(base);
		if (fds[n] < 0)
			break;
	}
	CHECK(n == PANWIN_FD_MAX - 1 && errno == EMFILE);
	for (int i = 0; i < n; i++)
		panwin_close(fds[i]);
	CHECK(panwin_close(base) == 0);
}

/* ---- transport injection (what the later in-process real-core transport plugs into) ---- */

static int tt_ioctls, tt_abi = 1;

static int tt_enumerate(void) { return 1; }
static int tt_device_path(int index, char *buf, size_t len)
{
	if (index != 0)
		return -ENOENT;
	snprintf(buf, len, "test:%d", index);
	return 0;
}
static int tt_open(int index, void **file)
{
	*file = (void *)(uintptr_t)(0x1000 + index);
	return 0;
}
static void tt_close(void *file) { (void)file; }
static int32_t tt_ioctl(void *file, uint32_t code, void *buf, uint32_t in_size, uint32_t out_size,
			uint32_t *returned)
{
	(void)file;
	(void)in_size;
	tt_ioctls++;
	*returned = 0;
	if (code == PANWIN_IOCTL_GET_INFO && out_size == sizeof(struct panwin_info)) {
		struct panwin_info *i = buf;
		memset(i, 0, sizeof(*i));
		i->abi_version = (uint32_t)tt_abi;
		*returned = sizeof(*i);
		return 0;
	}
	if (code == PANWIN_IOCTL_DRM(_IOC_NR(DRM_IOCTL_SYNCOBJ_WAIT)))
		return PANWIN_STATUS_FROM_ERRNO(110); /* ETIMEDOUT, not ETIME */
	return (int32_t)0xC0000010u; /* STATUS_INVALID_DEVICE_REQUEST: unknown ioctl */
}
static void *tt_os_handle(void *file) { return file; }

static const struct panwin_transport tt_transport = {
	.version = PANWIN_TRANSPORT_VERSION, .name = "test", .enumerate = tt_enumerate,
	.device_path = tt_device_path, .open = tt_open, .close = tt_close, .ioctl = tt_ioctl,
	.os_handle = tt_os_handle,
};

static void test_transports(void)
{
	struct panwin_transport bad = tt_transport;
	char path[32];

	bad.version = 99;
	CHECK(panwin_register_transport(&bad) == -1 && errno == EINVAL);
	CHECK(panwin_register_transport(&tt_transport) == 0);
	CHECK(panwin_register_transport(&tt_transport) == -1 && errno == EEXIST);
	CHECK(panwin_select_transport("nope") == -1 && errno == ENOENT);
	CHECK(panwin_select_transport("dll:Z:\\nonexistent\\x.dll") == -1 && errno == ENOENT);
	CHECK(panwin_select_transport("test") == 0);
	CHECK(!strcmp(panwin_transport_name(), "test"));
	CHECK(panwin_device_path(0, path, sizeof(path)) == 0 && !strcmp(path, "test:0"));

	/* ABI handshake: an ABI 1 kernel is refused unless the caller opts out. */
	tt_abi = 1;
	CHECK(panwin_open("/dev/dri/renderD128", 0) == -1 && errno == EPROTO);
	int fd = panwin_open("/dev/dri/renderD128", PANWIN_O_NOABICHECK);
	CHECK(fd >= 0);
	CHECK(panwin_select_transport("fake") == -1 && errno == EBUSY);
	CHECK(panwin_close(fd) == 0);
	tt_abi = PANWIN_ABI_VERSION;
	fd = panwin_open("/dev/dri/renderD128", 0);
	CHECK(fd >= 0);
	if (fd >= 0) {
		uint32_t h = 1;
		struct drm_get_cap cap = { .capability = DRM_CAP_SYNCOBJ };
		int before = tt_ioctls;
		CHECK(drmSyncobjWait(fd, &h, 1, 0, 0, NULL) == -ETIMEDOUT);
		CHECK(drmIoctl(fd, DRM_IOCTL_GET_CAP, &cap) == -1 && errno == ENOTTY);
		CHECK(tt_ioctls == before + 2);
		CHECK(panwin_close(fd) == 0);
	}
	/* PANWIN_TRANSPORT=dll:<path> loader (meson passes the test DLL's path). */
	char dll[600], sel[620];
	DWORD n = GetEnvironmentVariableA("PANWIN_TEST_TRANSPORT_DLL", dll, sizeof(dll));
	if (n > 0 && n < sizeof(dll)) {
		snprintf(sel, sizeof(sel), "dll:%s", dll);
		CHECK(panwin_select_transport(sel) == 0);
		CHECK(!strcmp(panwin_transport_name(), "testdll"));
		fd = panwin_open("/dev/dri/renderD128", 0); /* ABI 2 handshake through the DLL */
		CHECK(fd >= 0);
		if (fd >= 0) {
			struct drm_get_cap cap = { .capability = DRM_CAP_SYNCOBJ };
			CHECK(drmIoctl(fd, DRM_IOCTL_GET_CAP, &cap) == -1 && errno == ENOSYS);
			CHECK(panwin_close(fd) == 0);
		}
		CHECK(panwin_select_transport(sel) == 0); /* already registered: reused */
		CHECK(panwin_select_transport("testdll") == 0);
	} else {
		printf("note: PANWIN_TEST_TRANSPORT_DLL not set, dll: loader not tested\n");
	}
	CHECK(panwin_select_transport("fake") == 0);
	CHECK(!strcmp(panwin_transport_name(), "fake"));
}

int main(void)
{
	printf("transport: %s\n", panwin_transport_name());
	if (strcmp(panwin_transport_name(), "fake")) {
		CHECK(panwin_use_fake_kernel(1) == 0);
	}

	test_errno_map();
	test_devices();

	int fd = drmOpenWithType("panthor", NULL, DRM_NODE_RENDER);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK(panwin_use_fake_kernel(0) == -1 && errno == EBUSY);
		test_fds(fd);
		test_version_caps(fd);
		test_dev_query(fd);
		test_syncobj(fd);
		test_threads(fd);
		test_bo_mmap(fd);
		test_prime(fd);
		CHECK(drmClose(fd) == 0);
	}
	test_mapping_lifetime();
	test_fd_exhaustion();
	test_transports();

	printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "OK", checks, failures);
	return failures ? 1 : 0;
}
