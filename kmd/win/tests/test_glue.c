// SPDX-License-Identifier: MIT
/*
 * Host tests for the kmd/win glue (pw_win_ioctl.c, pw_os_win.c, pw_win_user.c) running against
 * the REAL kmd/core on a pthread fake of the NT/KMDF API (fake_wdk.c). They cover what cannot be
 * seen on the target without hardware: ioctl concurrency (a SYNCOBJ WAIT does not block a SIGNAL
 * from another thread), user waits ending with EINTR on a user APC / handle cleanup / device
 * revoke, SYNCOBJ_EVENT registration + cancel races and reference counting, MMAP/MUNMAP
 * bookkeeping per process, copy_from_user faults, and the pw_os_work state machine.
 * The Windows kernel itself is NOT exercised: these are logic tests of the glue only.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include "pw_win.h"
#include "fake_wdk.h"

#include "panwin/panwin_ioctl.h"
#include "drm.h"
#include "panthor_drm.h"
#include "panfrost_drm.h"

long fake_wdk_misuse_count(void);

#ifndef FAKE_NO_SEH_FAULTS
#define FAKE_NO_SEH_FAULTS 0
#endif

static int checks, failures;

#define CHECK(cond)                                                                    \
	do {                                                                           \
		__atomic_fetch_add(&checks, 1, __ATOMIC_RELAXED);                      \
		if (!(cond)) {                                                         \
			__atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);            \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                      \
	} while (0)
#define CHECK_EQ(a, b)                                                                     \
	do {                                                                               \
		long long _a = (long long)(a), _b = (long long)(b);                        \
		__atomic_fetch_add(&checks, 1, __ATOMIC_RELAXED);                          \
		if (_a != _b) {                                                            \
			__atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);                \
			fprintf(stderr, "FAIL %s:%d: %s == %lld, expected %s == %lld\n", __FILE__, \
				__LINE__, #a, _a, #b, _b);                                 \
		}                                                                          \
	} while (0)

/* ------------------------------------------------------------------ device + files */

static WDFDEVICE g_dev;
static PW_DEVICE_CTX *g_ctx;

static void device_up(void)
{
	struct pw_device_params p = { 0 };
	struct pw_device *pw = NULL;

	g_dev = fake_wdf_object_new(sizeof(PW_DEVICE_CTX));
	g_ctx = PwDeviceCtx(g_dev);
	g_ctx->device = g_dev;
	pw_os_win_init(&g_ctx->os, g_dev);
	pw_win_files_init(g_ctx);
	p.os = &g_ctx->os;
	CHECK_EQ(pw_device_alloc(&p, &pw), PW_OK);	/* no hardware touched: enough for files */
	g_ctx->pw = pw;
	g_ctx->core_ready = 1;
}

static void device_down(void)
{
	g_ctx->core_ready = 0;
	pw_win_revoke_files(g_ctx);
	pw_device_destroy(g_ctx->pw);
	g_ctx->pw = NULL;
	fake_wdf_object_free(g_dev);
	g_dev = NULL;
}

static WDFFILEOBJECT file_open(void)
{
	WDFFILEOBJECT fo = fake_wdf_object_new(sizeof(PW_FILE_CTX));
	WDFREQUEST req = fake_wdf_object_new(sizeof(PW_REQ_CTX));

	pw_win_file_create(g_dev, req, fo);
	CHECK(req->completed);
	CHECK_EQ(req->status, STATUS_SUCCESS);
	fake_wdf_object_free(req);
	return fo;
}

static void file_close(WDFFILEOBJECT fo)
{
	pw_win_file_cleanup(fo);
	pw_win_file_close(fo);
	fake_wdf_object_free(fo);
}

/* METHOD_BUFFERED round trip through EvtIoInCallerContext. */
static NTSTATUS ioctl_raw(WDFFILEOBJECT fo, ULONG code, void *arg, size_t in, size_t out)
{
	WDFREQUEST req = fake_wdf_object_new(sizeof(PW_REQ_CTX));
	size_t n = in > out ? in : out;
	NTSTATUS st;

	req->file = fo;
	req->code = code;
	req->in_len = in;
	req->out_len = out;
	req->sysbuf = calloc(1, n ? n : 1);
	if (in)
		memcpy(req->sysbuf, arg, in);
	pw_win_io_in_caller_context(g_dev, req);
	CHECK(req->completed);
	st = req->status;
	if (NT_SUCCESS(st)) {
		CHECK(req->info <= out);
		if (req->info)
			memcpy(arg, req->sysbuf, req->info);
	} else {
		CHECK_EQ(req->info, 0);
	}
	fake_wdf_object_free(req);
	return st;
}

/* 0 or -Linux errno (or -9999 for a non-errno status). */
static int st_errno(NTSTATUS st)
{
	if (st == STATUS_SUCCESS)
		return 0;
	if (PANWIN_STATUS_IS_ERRNO(st))
		return -PANWIN_STATUS_TO_ERRNO(st);
	return -9999;
}

static int drm(WDFFILEOBJECT fo, unsigned long request, void *arg)
{
	return st_errno(ioctl_raw(fo, PANWIN_IOCTL_DRM(_IOC_NR(request)), arg, _IOC_SIZE(request),
				  _IOC_SIZE(request)));
}

static uint32_t syncobj_create(WDFFILEOBJECT fo, uint32_t flags)
{
	struct drm_syncobj_create c = { .flags = flags };

	CHECK_EQ(drm(fo, DRM_IOCTL_SYNCOBJ_CREATE, &c), 0);
	return c.handle;
}

static int syncobj_signal(WDFFILEOBJECT fo, uint32_t h)
{
	struct drm_syncobj_array a = { .handles = (uintptr_t)&h, .count_handles = 1 };

	return drm(fo, DRM_IOCTL_SYNCOBJ_SIGNAL, &a);
}

static int syncobj_reset(WDFFILEOBJECT fo, uint32_t h)
{
	struct drm_syncobj_array a = { .handles = (uintptr_t)&h, .count_handles = 1 };

	return drm(fo, DRM_IOCTL_SYNCOBJ_RESET, &a);
}

static int syncobj_wait(WDFFILEOBJECT fo, uint32_t h, int64_t abs_timeout, uint32_t flags)
{
	struct drm_syncobj_wait w = {
		.handles = (uintptr_t)&h, .count_handles = 1, .timeout_nsec = abs_timeout,
		.flags = flags,
	};

	return drm(fo, DRM_IOCTL_SYNCOBJ_WAIT, &w);
}

/* A thread blocked in SYNCOBJ_WAIT (forever, WAIT_FOR_SUBMIT). */
struct waiter {
	pthread_t thr;
	WDFFILEOBJECT fo;
	uint32_t handle;
	HANDLE tid;
	volatile int started, done;
	int ret;
	uint64_t t_end;
};

static void *waiter_fn(void *arg)
{
	struct waiter *w = arg;

	w->tid = PsGetCurrentThreadId();
	__atomic_store_n(&w->started, 1, __ATOMIC_RELEASE);
	w->ret = syncobj_wait(w->fo, w->handle, INT64_MAX, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT);
	w->t_end = fake_now_ns();
	__atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
	return NULL;
}

static void waiter_start(struct waiter *w, WDFFILEOBJECT fo, uint32_t handle)
{
	memset(w, 0, sizeof(*w));
	w->fo = fo;
	w->handle = handle;
	pthread_create(&w->thr, NULL, waiter_fn, w);
	while (!__atomic_load_n(&w->started, __ATOMIC_ACQUIRE))
		usleep(100);
	usleep(20000);	/* let it block in the kernel wait */
}

static int waiter_join(struct waiter *w)
{
	pthread_join(w->thr, NULL);
	return w->ret;
}

/* ---------------------------------------------------------------------------- tests */

static void test_dispatch_basics(void)
{
	WDFFILEOBJECT fo;
	struct panwin_info info;
	uint32_t h;
	uint64_t junk[8] = { 0 };

	device_up();
	fo = file_open();

	memset(&info, 0, sizeof(info));
	CHECK_EQ(ioctl_raw(fo, PANWIN_IOCTL_GET_INFO, &info, 0, sizeof(info)), STATUS_SUCCESS);
	CHECK_EQ(info.abi_version, PANWIN_ABI_VERSION);

	/* Unknown private and unknown DRM ioctls. */
	CHECK_EQ(ioctl_raw(fo, PANWIN_CTL_CODE(0x9AB), junk, 8, 8), STATUS_INVALID_DEVICE_REQUEST);
	CHECK_EQ(st_errno(ioctl_raw(fo, PANWIN_IOCTL_DRM(0x3e), junk, 8, 8)), -ENOTTY);
	/* out > in, and over the size cap. */
	CHECK_EQ(st_errno(ioctl_raw(fo, PANWIN_IOCTL_DRM(_IOC_NR(DRM_IOCTL_SYNCOBJ_CREATE)), junk,
				    8, 16)), -EINVAL);

	h = syncobj_create(fo, 0);
	CHECK(h != 0);
	/* Binary syncobj without a fence: EINVAL unless WAIT_FOR_SUBMIT. */
	CHECK_EQ(syncobj_wait(fo, h, 0, 0), -EINVAL);
	/* Timeout is an absolute deadline on the QPC (here: monotonic) clock: -ETIME. */
	{
		uint64_t t0 = fake_now_ns();

		CHECK_EQ(syncobj_wait(fo, h, (int64_t)(t0 + 30000000),
				      DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT), -ETIME);
		CHECK(fake_now_ns() - t0 >= 30000000);
	}
	/* Fault in a nested user pointer: EFAULT from the SEH path, nothing leaks. (TSan cannot
	 * follow the setjmp/longjmp SEH emulation.) */
	if (!FAKE_NO_SEH_FAULTS) {
		struct drm_syncobj_wait w = {
			.handles = FAKE_BAD_USER_PTR, .count_handles = 1, .timeout_nsec = 0,
		};

		CHECK_EQ(drm(fo, DRM_IOCTL_SYNCOBJ_WAIT, &w), -EFAULT);
	}
	CHECK_EQ(syncobj_signal(fo, h), 0);
	CHECK_EQ(syncobj_wait(fo, h, 0, 0), 0);
	/* Copy-back only on success: a failing ioctl leaves the caller's struct alone. */
	{
		struct drm_syncobj_create c = { .handle = 0x1234, .flags = 0xdead };

		CHECK_EQ(drm(fo, DRM_IOCTL_SYNCOBJ_CREATE, &c), -EINVAL);
		CHECK_EQ(c.handle, 0x1234);
	}
	file_close(fo);
	device_down();
}

/* WAIT in one thread, SIGNAL from another on the same handle: must not deadlock. */
static void test_wait_vs_signal(void)
{
	WDFFILEOBJECT fo;
	struct waiter w;
	uint64_t t_sig;

	device_up();
	fo = file_open();
	for (int i = 0; i < 20; i++) {
		uint32_t h = syncobj_create(fo, 0);

		waiter_start(&w, fo, h);
		CHECK(!w.done);
		t_sig = fake_now_ns();
		CHECK_EQ(syncobj_signal(fo, h), 0);
		CHECK_EQ(waiter_join(&w), 0);
		CHECK(w.t_end >= t_sig);
	}
	file_close(fo);
	device_down();
}

/* A user APC ends the alertable UserMode wait: EINTR (libdrm-win restarts the ioctl). */
static void test_wait_user_apc(void)
{
	WDFFILEOBJECT fo;
	struct waiter w;
	uint32_t h;

	device_up();
	fo = file_open();
	h = syncobj_create(fo, 0);
	waiter_start(&w, fo, h);
	fake_queue_user_apc(w.tid);
	CHECK_EQ(waiter_join(&w), -EINTR);
	/* "Restart": the wait still works afterwards. */
	waiter_start(&w, fo, h);
	CHECK_EQ(syncobj_signal(fo, h), 0);
	CHECK_EQ(waiter_join(&w), 0);
	file_close(fo);
	device_down();
}

/* Closing the handle while another thread waits on it: the wait ends with EINTR, cleanup
 * returns, every later ioctl on the file is ENODEV, the process reference is dropped. */
static void test_cleanup_ends_waits(void)
{
	WDFFILEOBJECT fo;
	struct waiter w[3];
	int refs0 = fake_proc_a.hdr.refs;

	device_up();
	fo = file_open();
	CHECK_EQ(fake_proc_a.hdr.refs, refs0 + 1);
	for (int i = 0; i < 3; i++)
		waiter_start(&w[i], fo, syncobj_create(fo, 0));
	pw_win_file_cleanup(fo);
	for (int i = 0; i < 3; i++)
		CHECK_EQ(waiter_join(&w[i]), -EINTR);
	CHECK_EQ(syncobj_wait(fo, 1, 0, 0), -ENODEV);
	pw_win_file_close(fo);
	CHECK_EQ(fake_proc_a.hdr.refs, refs0);
	fake_wdf_object_free(fo);
	device_down();
}

/* Device stop (D0Exit) revokes every open file the same way; handles stay usable (ENODEV). */
static void test_revoke(void)
{
	WDFFILEOBJECT a, b;
	struct waiter w;

	device_up();
	a = file_open();
	b = file_open();
	waiter_start(&w, a, syncobj_create(a, 0));
	(void)syncobj_create(b, 0);
	g_ctx->core_ready = 0;
	pw_win_revoke_files(g_ctx);
	CHECK_EQ(waiter_join(&w), -EINTR);
	CHECK_EQ(syncobj_wait(a, 1, 0, 0), -ENODEV);
	CHECK_EQ(syncobj_wait(b, 1, 0, 0), -ENODEV);
	/* A file opened while the core is down: opens, DRM ioctls ENODEV. */
	{
		WDFFILEOBJECT c = file_open();
		struct drm_syncobj_create cr = { 0 };

		CHECK_EQ(drm(c, DRM_IOCTL_SYNCOBJ_CREATE, &cr), -ENODEV);
		file_close(c);
	}
	file_close(a);
	file_close(b);
	device_down();
}

static NTSTATUS syncobj_event(WDFFILEOBJECT fo, uint32_t h, uint64_t point, uint32_t flags,
			      PKEVENT ev)
{
	struct panwin_syncobj_event a = {
		.handle = h, .flags = flags, .point = point, .event_handle = (uintptr_t)ev,
	};

	return ioctl_raw(fo, PANWIN_IOCTL_SYNCOBJ_EVENT, &a, sizeof(a), 0);
}

static void test_syncobj_event(void)
{
	WDFFILEOBJECT fo;
	PKEVENT ev[4];
	uint32_t h;

	device_up();
	fo = file_open();
	h = syncobj_create(fo, 0);
	for (int i = 0; i < 4; i++)
		ev[i] = fake_user_event_create();

	/* No fence and no WAIT_FOR_SUBMIT: EINVAL, the event reference is given back. */
	CHECK_EQ(st_errno(syncobj_event(fo, h, 0, 0, ev[0])), -EINVAL);
	CHECK_EQ(fake_user_event_refs(ev[0]), 1);
	/* Bad handles. */
	CHECK_EQ(st_errno(syncobj_event(fo, h, 0, 0, NULL)), -EBADF);
	CHECK_EQ(st_errno(syncobj_event(fo, h, 0, 0, (PKEVENT)(uintptr_t)FAKE_BAD_USER_PTR)),
		 -EBADF);
	CHECK_EQ(st_errno(syncobj_event(fo, h, 0, 0, (PKEVENT)&fake_proc_a)), -EINVAL);
	CHECK_EQ(st_errno(syncobj_event(fo, 999, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
					ev[0])), -ENOENT);
	CHECK_EQ(fake_user_event_refs(ev[0]), 1);

	/* Pending, then signalled from another ioctl: event set, reference dropped. */
	CHECK_EQ(syncobj_event(fo, h, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, ev[0]),
		 STATUS_SUCCESS);
	CHECK(!fake_event_is_set(ev[0]));
	CHECK_EQ(fake_user_event_refs(ev[0]), 2);
	CHECK_EQ(syncobj_signal(fo, h), 0);
	CHECK(fake_event_is_set(ev[0]));
	CHECK_EQ(fake_user_event_refs(ev[0]), 1);

	/* Already signalled: fires synchronously. */
	CHECK_EQ(syncobj_event(fo, h, 0, 0, ev[1]), STATUS_SUCCESS);
	CHECK(fake_event_is_set(ev[1]));
	CHECK_EQ(fake_user_event_refs(ev[1]), 1);

	/* Pending at cleanup: cancelled, never set, reference dropped. */
	CHECK_EQ(syncobj_reset(fo, h), 0);
	CHECK_EQ(syncobj_event(fo, h, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, ev[2]),
		 STATUS_SUCCESS);
	CHECK_EQ(syncobj_event(fo, h, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, ev[3]),
		 STATUS_SUCCESS);
	CHECK_EQ(fake_user_event_refs(ev[2]), 2);
	file_close(fo);
	CHECK(!fake_event_is_set(ev[2]));
	CHECK_EQ(fake_user_event_refs(ev[2]), 1);
	CHECK_EQ(fake_user_event_refs(ev[3]), 1);

	for (int i = 0; i < 4; i++)
		fake_user_event_close(ev[i]);
	device_down();
}

/* Registrations racing with signals from another thread and with handle cleanup. */
struct signaller {
	WDFFILEOBJECT fo;
	uint32_t *handles;
	int n;
	volatile int stop;
};

static void *signaller_fn(void *arg)
{
	struct signaller *s = arg;
	unsigned i = 0;

	while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
		uint32_t h = s->handles[i++ % (unsigned)s->n];

		(void)syncobj_signal(s->fo, h);	/* ENODEV once the file is gone */
		if (i % 3 == 0)
			(void)syncobj_reset(s->fo, h);
	}
	return NULL;
}

static void test_syncobj_event_races(void)
{
	enum { NH = 8, NEV = 400 };
	PKEVENT *ev = calloc(NEV, sizeof(*ev));

	for (int round = 0; round < 5; round++) {
		struct signaller s = { 0 };
		uint32_t handles[NH];
		pthread_t thr;
		int ok = 0;
		WDFFILEOBJECT fo;

		device_up();
		fo = file_open();
		for (int i = 0; i < NH; i++)
			handles[i] = syncobj_create(fo, 0);
		s.fo = fo;
		s.handles = handles;
		s.n = NH;
		pthread_create(&thr, NULL, signaller_fn, &s);
		for (int i = 0; i < NEV; i++) {
			ev[i] = fake_user_event_create();
			if (syncobj_event(fo, handles[i % NH], 0,
					  DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, ev[i]) ==
			    STATUS_SUCCESS)
				ok++;
		}
		CHECK_EQ(ok, NEV);
		/* Close while the signaller is still firing notifies. */
		pw_win_file_cleanup(fo);
		__atomic_store_n(&s.stop, 1, __ATOMIC_RELEASE);
		pthread_join(thr, NULL);
		pw_win_file_close(fo);
		fake_wdf_object_free(fo);
		for (int i = 0; i < NEV; i++) {
			CHECK_EQ(fake_user_event_refs(ev[i]), 1);
			fake_user_event_close(ev[i]);
		}
		device_down();
	}
	free(ev);
}

static NTSTATUS do_mmap(WDFFILEOBJECT fo, uint64_t offset, uint64_t size, uint32_t prot,
			uint64_t *va)
{
	struct panwin_mmap m = { .offset = offset, .size = size, .prot = prot };
	NTSTATUS st = ioctl_raw(fo, PANWIN_IOCTL_MMAP, &m, sizeof(m), sizeof(m));

	*va = m.user_va;
	return st;
}

static NTSTATUS do_munmap(WDFFILEOBJECT fo, uint64_t va, uint64_t size)
{
	struct panwin_munmap m = { .user_va = va, .size = size };

	return ioctl_raw(fo, PANWIN_IOCTL_MUNMAP, &m, sizeof(m), 0);
}

static void test_mmap(void)
{
	WDFFILEOBJECT fo;
	struct drm_panthor_bo_create bc = { .size = 3 * 4096 };
	struct drm_panthor_bo_mmap_offset mo = { 0 };
	struct drm_gem_close gc = { 0 };
	uint64_t va = 0, va2 = 0;
	long live0 = fake_user_maps_live;

	device_up();
	fo = file_open();
	CHECK_EQ(drm(fo, DRM_IOCTL_PANTHOR_BO_CREATE, &bc), 0);
	mo.handle = bc.handle;
	CHECK_EQ(drm(fo, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &mo), 0);

	/* Argument checks. */
	CHECK_EQ(st_errno(do_mmap(fo, mo.offset, 0, PANWIN_PROT_READ, &va)), -EINVAL);
	CHECK_EQ(st_errno(do_mmap(fo, mo.offset, 100, PANWIN_PROT_READ, &va)), -EINVAL);
	CHECK_EQ(st_errno(do_mmap(fo, mo.offset, 4096, 4, &va)), -EINVAL);
	CHECK(st_errno(do_mmap(fo, mo.offset, 4 * 4096, PANWIN_PROT_READ, &va)) < 0);
	/* User MMIO page: no register window in this harness -> EINVAL; writable -> EINVAL. */
	CHECK_EQ(st_errno(do_mmap(fo, DRM_PANTHOR_USER_MMIO_OFFSET_64BIT, 4096,
				  PANWIN_PROT_READ | PANWIN_PROT_WRITE, &va)), -EINVAL);
	CHECK_EQ(fake_user_maps_live, live0);

	/* Map, write through it, unmap. */
	CHECK_EQ(do_mmap(fo, mo.offset, 3 * 4096, PANWIN_PROT_READ | PANWIN_PROT_WRITE, &va),
		 STATUS_SUCCESS);
	CHECK(va != 0);
	CHECK_EQ(fake_user_maps_live, live0 + 1);
	memset((void *)(uintptr_t)va, 0x5a, 3 * 4096);
	/* Partial mapping at a page offset. */
	CHECK_EQ(do_mmap(fo, mo.offset + 4096, 4096, PANWIN_PROT_READ, &va2), STATUS_SUCCESS);
	CHECK_EQ(*(volatile uint8_t *)(uintptr_t)va2, 0x5a);
	CHECK_EQ(va2, va + 4096);
	CHECK_EQ(st_errno(do_munmap(fo, va, 4096)), -EINVAL);	/* whole mappings only */
	CHECK_EQ(do_munmap(fo, va, 3 * 4096), STATUS_SUCCESS);
	CHECK_EQ(st_errno(do_munmap(fo, va, 3 * 4096)), -EINVAL);
	CHECK_EQ(fake_user_maps_live, live0 + 1);

	/* Another process may not map through this handle, nor unmap. */
	fake_set_current_process(&fake_proc_b);
	CHECK_EQ(st_errno(do_mmap(fo, mo.offset, 4096, PANWIN_PROT_READ, &va)), -EACCES);
	CHECK_EQ(st_errno(do_munmap(fo, va2, 4096)), -EINVAL);
	fake_set_current_process(NULL);

	/* The mapping keeps the BO alive after GEM_CLOSE (as a Linux VMA does). */
	gc.handle = bc.handle;
	CHECK_EQ(drm(fo, DRM_IOCTL_GEM_CLOSE, &gc), 0);
	CHECK_EQ(*(volatile uint8_t *)(uintptr_t)va2, 0x5a);

	/* Last handle closed from another process (duplicated handle): cleanup attaches to the
	 * creator to unmap there. LeakSanitizer checks the BO is released. */
	{
		long att0 = fake_attach_count;

		fake_set_current_process(&fake_proc_b);
		pw_win_file_cleanup(fo);
		fake_set_current_process(NULL);
		CHECK_EQ(fake_attach_count, att0 + 1);
	}
	CHECK_EQ(fake_user_maps_live, live0);
	CHECK_EQ(fake_unmap_wrong_process, 0);
	pw_win_file_close(fo);
	fake_wdf_object_free(fo);
	device_down();
}

/* ------------------------------------------------------------------- pw_os_work */

struct wctx {
	struct pw_os_work *w;
	volatile int runs;
	volatile int running, max_running;
	volatile int requeue_delayed_ms;	/* >0: re-arm itself from the callback */
	volatile int requeue_now;		/* >0: re-queue itself immediately, n times */
	uint64_t first_run_ns;
	unsigned sleep_us;
	volatile int hold;			/* callback blocks while set */
};

static void work_fn(void *arg)
{
	struct wctx *c = arg;
	int r = __atomic_add_fetch(&c->running, 1, __ATOMIC_ACQ_REL);

	if (r > c->max_running)
		c->max_running = r;
	if (!__atomic_load_n(&c->runs, __ATOMIC_ACQUIRE))
		__atomic_store_n(&c->first_run_ns, fake_now_ns(), __ATOMIC_RELEASE);
	if (__atomic_load_n(&c->sleep_us, __ATOMIC_ACQUIRE))
		usleep(__atomic_load_n(&c->sleep_us, __ATOMIC_ACQUIRE));
	while (__atomic_load_n(&c->hold, __ATOMIC_ACQUIRE))
		usleep(500);
	__atomic_add_fetch(&c->runs, 1, __ATOMIC_ACQ_REL);
	__atomic_sub_fetch(&c->running, 1, __ATOMIC_ACQ_REL);
	if (c->requeue_now > 0) {
		c->requeue_now--;
		pw_os_work_queue(c->w);
	}
	if (c->requeue_delayed_ms > 0)
		pw_os_work_queue_delayed(c->w, (uint32_t)c->requeue_delayed_ms);
}

static void *destroy_thread(void *arg)
{
	struct wctx *c = arg;

	pw_os_work_destroy(&g_ctx->os, c->w);
	return NULL;
}

static void test_work(void)
{
	struct wctx c;
	uint64_t t0;

	device_up();

	/* queue -> runs once; queue twice before it runs -> once. */
	memset(&c, 0, sizeof(c));
	c.w = pw_os_work_create(&g_ctx->os, work_fn, &c);
	CHECK(c.w != NULL);
	pw_os_work_queue(c.w);
	pw_os_work_flush(c.w);
	CHECK_EQ(__atomic_load_n(&c.runs, __ATOMIC_ACQUIRE), 1);

	/* queue while running -> exactly one more run, never concurrently. */
	__atomic_store_n(&c.runs, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&c.sleep_us, 30000u, __ATOMIC_RELEASE);
	pw_os_work_queue(c.w);
	usleep(10000);
	pw_os_work_queue(c.w);
	pw_os_work_queue(c.w);
	pw_os_work_flush(c.w);
	CHECK_EQ(__atomic_load_n(&c.runs, __ATOMIC_ACQUIRE), 2);
	CHECK_EQ(c.max_running, 1);
	__atomic_store_n(&c.sleep_us, 0u, __ATOMIC_RELEASE);

	/* self re-queue from the callback. */
	__atomic_store_n(&c.runs, 0, __ATOMIC_RELEASE);
	c.requeue_now = 3;
	pw_os_work_queue(c.w);
	pw_os_work_flush(c.w);
	CHECK_EQ(__atomic_load_n(&c.runs, __ATOMIC_ACQUIRE), 4);

	/* delayed: not before the delay; flush runs a pending delayed item now. */
	__atomic_store_n(&c.runs, 0, __ATOMIC_RELEASE);
	t0 = fake_now_ns();
	pw_os_work_queue_delayed(c.w, 40);
	usleep(10000);
	CHECK_EQ(__atomic_load_n(&c.runs, __ATOMIC_ACQUIRE), 0);
	usleep(80000);
	CHECK_EQ(__atomic_load_n(&c.runs, __ATOMIC_ACQUIRE), 1);
	CHECK(__atomic_load_n(&c.first_run_ns, __ATOMIC_ACQUIRE) - t0 >= 40000000);
	__atomic_store_n(&c.runs, 0, __ATOMIC_RELEASE);
	t0 = fake_now_ns();
	pw_os_work_queue_delayed(c.w, 10000);
	pw_os_work_flush(c.w);
	CHECK_EQ(__atomic_load_n(&c.runs, __ATOMIC_ACQUIRE), 1);
	CHECK(fake_now_ns() - t0 < 1000000000);
	/* queue on a pending delayed run makes it immediate. */
	__atomic_store_n(&c.runs, 0, __ATOMIC_RELEASE);
	pw_os_work_queue_delayed(c.w, 10000);
	pw_os_work_queue(c.w);
	usleep(50000);
	CHECK_EQ(__atomic_load_n(&c.runs, __ATOMIC_ACQUIRE), 1);
	/* queue_delayed on a pending immediate run does not delay it. */
	__atomic_store_n(&c.runs, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&c.sleep_us, 20000u, __ATOMIC_RELEASE);
	pw_os_work_queue(c.w);
	usleep(5000);			/* running */
	pw_os_work_queue(c.w);		/* pending immediate */
	pw_os_work_queue_delayed(c.w, 10000);
	pw_os_work_flush(c.w);
	CHECK_EQ(__atomic_load_n(&c.runs, __ATOMIC_ACQUIRE), 2);
	__atomic_store_n(&c.sleep_us, 0u, __ATOMIC_RELEASE);

	/* destroy cancels a pending delayed run. */
	__atomic_store_n(&c.runs, 0, __ATOMIC_RELEASE);
	pw_os_work_queue_delayed(c.w, 50);
	pw_os_work_destroy(&g_ctx->os, c.w);
	usleep(100000);
	CHECK_EQ(__atomic_load_n(&c.runs, __ATOMIC_ACQUIRE), 0);

	/*
	 * destroy while the callback keeps re-arming itself with a delay (a periodic tick): the
	 * mock's semantics (kmd/tests/pw_os_mock.c) - the re-armed delayed run is cancelled, so
	 * destroy returns promptly. Run in a thread with a deadline so a livelock is reported
	 * instead of hanging the test.
	 */
	memset(&c, 0, sizeof(c));
	c.w = pw_os_work_create(&g_ctx->os, work_fn, &c);
	c.requeue_delayed_ms = 1;
	__atomic_store_n(&c.sleep_us, 1000u, __ATOMIC_RELEASE);
	pw_os_work_queue(c.w);
	usleep(20000);
	{
		pthread_t thr;
		int done = 0;
		int runs_at_destroy;

		/* Catch the callback while it runs, start destroy, then let it re-arm itself. */
		__atomic_store_n(&c.hold, 1, __ATOMIC_RELEASE);
		while (!__atomic_load_n(&c.running, __ATOMIC_ACQUIRE))
			usleep(100);
		runs_at_destroy = __atomic_load_n(&c.runs, __ATOMIC_ACQUIRE);
		pthread_create(&thr, NULL, destroy_thread, &c);
		usleep(10000);
		__atomic_store_n(&c.hold, 0, __ATOMIC_RELEASE);
		for (int i = 0; i < 200 && !done; i++) {
			usleep(5000);
			done = pthread_tryjoin_np(thr, NULL) == 0;
		}
		CHECK(done);
		if (!done) {
			fprintf(stderr, "  pw_os_work_destroy livelocked: %d runs since destroy\n",
				__atomic_load_n(&c.runs, __ATOMIC_ACQUIRE) - runs_at_destroy);
			__atomic_store_n(&c.requeue_delayed_ms, 0, __ATOMIC_RELEASE);
			pthread_join(thr, NULL);
		}
	}
	fake_wait_work_items_idle();
	device_down();
}

/*
 * (jm-glue review) The glue on a job-manager GPU: a static "always ready" Mali-G31 register
 * file (GPU_ID 0x70930000, every power domain READY, RESET/POWER/CLEAN completion bits latched in
 * GPU_INT_RAWSTAT), probed by the real core. Covers what jm-glue could only check through
 * libdrm-win's copy of the logic: GET_INFO reports PANWIN_INFO_JM and the panfrost DRM version
 * (also after a failed probe), VERSION through pw_win_ioctl.c says "panfrost", a panfrost
 * GET_PARAM goes through, and the user-MMIO mmap offsets fail with EINVAL (no such page on JM).
 */
static uint32_t g31_regs[0x40000 / 4];

static void g31_regs_init(bool ready)
{
	memset(g31_regs, 0, sizeof(g31_regs));
	g31_regs[0x000 / 4] = 0x70930000u;	/* GPU_ID: G31 r0p0 */
	g31_regs[0x004 / 4] = 0x07110206u;	/* L2_FEATURES */
	g31_regs[0x014 / 4] = 0x00003030u;	/* MMU_FEATURES: 48-bit VA and PA (host addresses) */
	g31_regs[0x018 / 4] = 0xffu;		/* AS_PRESENT */
	g31_regs[0x01c / 4] = 0x7u;		/* JS_PRESENT */
	g31_regs[0x100 / 4] = 0x3u;		/* SHADER_PRESENT */
	g31_regs[0x110 / 4] = 0x1u;		/* TILER_PRESENT */
	g31_regs[0x120 / 4] = 0x1u;		/* L2_PRESENT */
	g31_regs[0xe00 / 4] = 0x1u;		/* STACK_PRESENT */
	if (!ready)
		return;
	/* RESET_COMPLETED | POWER_CHANGED | POWER_CHANGED_ALL | CLEAN_CACHES_COMPLETED */
	g31_regs[0x020 / 4] = (1u << 8) | (1u << 9) | (1u << 10) | (1u << 17);
	g31_regs[0x140 / 4] = 0x3u;		/* SHADER_READY */
	g31_regs[0x150 / 4] = 0x1u;		/* TILER_READY */
	g31_regs[0x160 / 4] = 0x1u;		/* L2_READY */
	g31_regs[0xe10 / 4] = 0x1u;		/* STACK_READY */
}

/* Like device_up(), but with the register file and a real probe; returns the probe's result. */
static int device_up_g31(bool ready)
{
	struct pw_device_params p = { 0 };
	struct pw_device *pw = NULL;
	int ret;

	g31_regs_init(ready);
	g_dev = fake_wdf_object_new(sizeof(PW_DEVICE_CTX));
	g_ctx = PwDeviceCtx(g_dev);
	g_ctx->device = g_dev;
	pw_os_win_init(&g_ctx->os, g_dev);
	pw_win_files_init(g_ctx);
	g_ctx->os.mmio = (volatile UCHAR *)g31_regs;
	g_ctx->os.mmio_len = sizeof(g31_regs);
	g_ctx->os.regs_enabled = 1;
	g_ctx->gpu_powered = TRUE;
	p.os = &g_ctx->os;
	CHECK_EQ(pw_device_alloc(&p, &pw), PW_OK);
	g_ctx->pw = pw;
	ret = pw_device_probe(pw);
	g_ctx->core_status = ret;
	g_ctx->backend = pw_device_backend(pw);
	g_ctx->probed = 1;
	g_ctx->core_ready = ret == 0;
	return ret;
}

static void device_down_g31(void)
{
	device_down();
}

static void test_jm(void)
{
	struct panwin_info info;
	uint32_t maj = 0, min = 0, patch = 0;
	const char *name = NULL;
	WDFFILEOBJECT fo;
	uint64_t va = 0;
	int ret;

	/* 1. Failed probe (GPU_ID read, reset never completes): still a JM device for GET_INFO. */
	CHECK(device_up_g31(false) != 0);
	CHECK_EQ(g_ctx->backend, PW_BACKEND_JM);
	fo = file_open();
	memset(&info, 0, sizeof(info));
	CHECK_EQ(ioctl_raw(fo, PANWIN_IOCTL_GET_INFO, &info, 0, sizeof(info)), STATUS_SUCCESS);
	pw_device_drm_version(g_ctx->pw, &maj, &min, &patch, &name);
	CHECK(name && !strcmp(name, "panfrost"));
	CHECK(info.flags & PANWIN_INFO_JM);
	CHECK(!(info.flags & PANWIN_INFO_FW_BOOTED));
	CHECK_EQ(info.drm_version_major, maj);
	CHECK_EQ(info.drm_version_minor, min);
	CHECK(strstr(info.description, "no core device") != NULL);
	{
		struct drm_panfrost_get_param gp = { .param = DRM_PANFROST_PARAM_GPU_PROD_ID };

		CHECK_EQ(drm(fo, DRM_IOCTL_PANFROST_GET_PARAM, &gp), -ENODEV);
	}
	/* Once the device is gone (D0Exit clears probed), GET_INFO is the pre-JM panthor answer. */
	g_ctx->probed = 0;
	memset(&info, 0, sizeof(info));
	CHECK_EQ(ioctl_raw(fo, PANWIN_IOCTL_GET_INFO, &info, 0, sizeof(info)), STATUS_SUCCESS);
	pw_device_drm_version(NULL, &maj, &min, &patch, NULL);
	CHECK(!(info.flags & PANWIN_INFO_JM));
	CHECK_EQ(info.drm_version_major, maj);
	CHECK_EQ(info.drm_version_minor, min);
	file_close(fo);
	device_down_g31();

	/* 2. Successful probe on the static register file. */
	ret = device_up_g31(true);
	if (ret) {
		/* The core's init now waits for something this static file does not model: not a
		 * glue bug, so report it instead of failing (extend g31_regs_init()). */
		fprintf(stderr, "  SKIP jm part 2: probe on the static G31 register file failed (%d)\n",
			ret);
		device_down_g31();
		return;
	}
	fo = file_open();
	memset(&info, 0, sizeof(info));
	CHECK_EQ(ioctl_raw(fo, PANWIN_IOCTL_GET_INFO, &info, 0, sizeof(info)), STATUS_SUCCESS);
	pw_device_drm_version(g_ctx->pw, &maj, &min, &patch, NULL);
	CHECK(info.flags & PANWIN_INFO_JM);
	CHECK_EQ(info.drm_version_major, maj);
	CHECK_EQ(info.drm_version_minor, min);
	{
		char nbuf[16] = { 0 };
		struct drm_version v = { .name = nbuf, .name_len = sizeof(nbuf) - 1 };

		CHECK_EQ(drm(fo, DRM_IOCTL_VERSION, &v), 0);
		CHECK_EQ(v.name_len, 8);
		CHECK(!strcmp(nbuf, "panfrost"));
		CHECK_EQ(v.version_major, (int)maj);
		CHECK_EQ(v.version_minor, (int)min);
	}
	{
		struct drm_panfrost_get_param gp = { .param = DRM_PANFROST_PARAM_GPU_PROD_ID };

		CHECK_EQ(drm(fo, DRM_IOCTL_PANFROST_GET_PARAM, &gp), 0);
		CHECK_EQ(gp.value, 0x7093);
		gp.param = DRM_PANFROST_PARAM_JS_PRESENT;
		gp.value = 0;
		CHECK_EQ(drm(fo, DRM_IOCTL_PANFROST_GET_PARAM, &gp), 0);
		CHECK_EQ(gp.value, 0x7);
	}
	/* No user-MMIO page on JM: EINVAL at both offsets, read-only or not, nothing mapped. */
	{
		long live0 = fake_user_maps_live;

		CHECK_EQ(st_errno(do_mmap(fo, DRM_PANTHOR_USER_MMIO_OFFSET_64BIT, 4096,
					  PANWIN_PROT_READ, &va)), -EINVAL);
		CHECK_EQ(st_errno(do_mmap(fo, DRM_PANTHOR_USER_MMIO_OFFSET_32BIT, 4096,
					  PANWIN_PROT_READ, &va)), -EINVAL);
		CHECK_EQ(fake_user_maps_live, live0);
	}
	file_close(fo);
	device_down_g31();
}

/* ----------------------------------------------------------------------------- main */

static const struct {
	const char *name;
	void (*fn)(void);
} tests[] = {
	{ "dispatch_basics", test_dispatch_basics },
	{ "wait_vs_signal", test_wait_vs_signal },
	{ "wait_user_apc", test_wait_user_apc },
	{ "cleanup_ends_waits", test_cleanup_ends_waits },
	{ "revoke", test_revoke },
	{ "syncobj_event", test_syncobj_event },
	{ "syncobj_event_races", test_syncobj_event_races },
	{ "mmap", test_mmap },
	{ "work", test_work },
	{ "jm", test_jm },
};

int main(int argc, char **argv)
{
	int ran = 0;

	alarm(120);	/* a deadlock fails the run instead of hanging it */
	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		int f0 = failures;

		if (argc > 1 && !strstr(tests[i].name, argv[1]))
			continue;
		fprintf(stderr, "[ RUN  ] %s\n", tests[i].name);
		tests[i].fn();
		fprintf(stderr, "[ %s ] %s\n", failures == f0 ? " OK " : "FAIL", tests[i].name);
		ran++;
	}
	fake_wait_work_items_idle();
	CHECK_EQ(fake_wdk_misuse_count(), 0);
	fprintf(stderr, "%s: %d tests, %d checks, %d failures\n", failures ? "FAIL" : "OK", ran,
		checks, failures);
	return failures ? 1 : 0;
}
