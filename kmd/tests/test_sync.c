// SPDX-License-Identifier: MIT
/*
 * test_sync.c - fences (pw_fence.h) and DRM syncobjs (pw_sync.h): every SYNCOBJ ioctl through
 * pw_file_ioctl() with the real drm.h structs (error paths included), timeline ordering / query /
 * garbage collection, WAIT_ALL vs any + first_signaled, measured timeouts, WAIT_FOR_SUBMIT with a
 * late attacher, concurrent waiters/signallers, notify fire/cancel races, destroy with pending
 * waiters, and leak checks. Uses a device probed without firmware (sync needs no GPU).
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mock_gpu.h"
#include "pw_sync.h"
#include "test.h"

#define MS 1000000ull

struct env {
	struct pw_os *os;
	struct pw_device *dev;
	struct pw_file *file;
};

static bool env_init(struct env *e)
{
	struct mock_config cfg = { 0 };
	struct pw_device_params p = { 0 };

	memset(e, 0, sizeof(*e));
	cfg.instant_completion = true;
	e->os = mock_create(&cfg);
	if (!e->os)
		return false;
	p.os = e->os;
	p.boot_firmware = false;
	if (pw_device_alloc(&p, &e->dev))
		return false;
	mock_attach_irq(e->os, e->dev);
	if (pw_device_probe(e->dev))
		return false;
	return pw_file_open(e->dev, &e->file) == PW_OK;
}

static void env_fini(struct env *e)
{
	pw_file_close(e->file);
	mock_detach_irq(e->os);
	pw_device_destroy(e->dev);
	check_no_leaks(e->os);
	mock_destroy(e->os);
}

#define ENV_INIT(e) do { if (!env_init(e)) { CHECK(!"env_init"); return; } } while (0)

static uint64_t uptr(const void *p)
{
	return (uint64_t)(uintptr_t)p;
}

static uint64_t tnow(struct env *e)
{
	return pw_os_time_ns(e->os);
}

/* ---- ioctl wrappers ---- */

#define IOC(e, req, a) pw_file_ioctl((e)->file, _IOC_NR(req), (a), sizeof(*(a)))

static int sc_create(struct env *e, uint32_t flags, uint32_t *h)
{
	struct drm_syncobj_create a = { .flags = flags };
	int ret = IOC(e, DRM_IOCTL_SYNCOBJ_CREATE, &a);

	*h = a.handle;
	return ret;
}

static uint32_t mk(struct env *e, uint32_t flags)
{
	uint32_t h = 0;

	CHECK_EQ(sc_create(e, flags, &h), 0);
	return h;
}

static int sc_destroy(struct env *e, uint32_t h)
{
	struct drm_syncobj_destroy a = { .handle = h };

	return IOC(e, DRM_IOCTL_SYNCOBJ_DESTROY, &a);
}

static int sc_wait(struct env *e, const uint32_t *h, uint32_t n, uint32_t flags, int64_t timeout,
		   uint32_t *first)
{
	struct drm_syncobj_wait a = {
		.handles = uptr(h), .count_handles = n, .flags = flags, .timeout_nsec = timeout,
		.first_signaled = 0xdeadbeef,
	};
	int ret = IOC(e, DRM_IOCTL_SYNCOBJ_WAIT, &a);

	if (first)
		*first = a.first_signaled;
	return ret;
}

static int sc_twait(struct env *e, const uint32_t *h, const uint64_t *pts, uint32_t n,
		    uint32_t flags, int64_t timeout, uint32_t *first)
{
	struct drm_syncobj_timeline_wait a = {
		.handles = uptr(h), .points = uptr(pts), .count_handles = n, .flags = flags,
		.timeout_nsec = timeout, .first_signaled = 0xdeadbeef,
	};
	int ret = IOC(e, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &a);

	if (first)
		*first = a.first_signaled;
	return ret;
}

static int sc_array(struct env *e, unsigned long req, const uint32_t *h, uint32_t n, uint32_t pad)
{
	struct drm_syncobj_array a = { .handles = uptr(h), .count_handles = n, .pad = pad };

	if (req == DRM_IOCTL_SYNCOBJ_RESET)
		return IOC(e, DRM_IOCTL_SYNCOBJ_RESET, &a);
	return IOC(e, DRM_IOCTL_SYNCOBJ_SIGNAL, &a);
}

static int sc_tsignal(struct env *e, const uint32_t *h, const uint64_t *pts, uint32_t n,
		      uint32_t flags)
{
	struct drm_syncobj_timeline_array a = {
		.handles = uptr(h), .points = uptr(pts), .count_handles = n, .flags = flags,
	};

	return IOC(e, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &a);
}

static int sc_query(struct env *e, const uint32_t *h, uint64_t *pts, uint32_t n, uint32_t flags)
{
	struct drm_syncobj_timeline_array a = {
		.handles = uptr(h), .points = uptr(pts), .count_handles = n, .flags = flags,
	};

	return IOC(e, DRM_IOCTL_SYNCOBJ_QUERY, &a);
}

static uint64_t q1(struct env *e, uint32_t h, uint32_t flags)
{
	uint64_t p = 0xdead;

	CHECK_EQ(sc_query(e, &h, &p, 1, flags), 0);
	return p;
}

static int sc_transfer(struct env *e, uint32_t dst, uint64_t dpt, uint32_t src, uint64_t spt,
		       uint32_t flags, uint32_t pad)
{
	struct drm_syncobj_transfer a = {
		.src_handle = src, .dst_handle = dst, .src_point = spt, .dst_point = dpt,
		.flags = flags, .pad = pad,
	};

	return IOC(e, DRM_IOCTL_SYNCOBJ_TRANSFER, &a);
}

static int tsig1(struct env *e, uint32_t h, uint64_t pt)
{
	return sc_tsignal(e, &h, &pt, 1, 0);
}

static int twait1(struct env *e, uint32_t h, uint64_t pt, uint32_t flags, int64_t timeout)
{
	return sc_twait(e, &h, &pt, 1, flags, timeout, NULL);
}

/* ------------------------------------------------------------------------------------------ */
/* fences                                                                                     */
/* ------------------------------------------------------------------------------------------ */

struct cb_rec {
	struct pw_fence_cb cb;
	int calls;		/* __atomic */
	int order;
	int *counter;
};

static void rec_cb(struct pw_fence *f, struct pw_fence_cb *cb)
{
	struct cb_rec *r = (struct cb_rec *)cb;

	CHECK(pw_fence_is_signalled(f));
	__atomic_fetch_add(&r->calls, 1, __ATOMIC_SEQ_CST);
	if (r->counter)
		r->order = __atomic_add_fetch(r->counter, 1, __ATOMIC_SEQ_CST);
}

static void test_fence_basic(void)
{
	struct env e;
	struct pw_fence *f, *s;
	uint64_t t0;

	ENV_INIT(&e);
	f = pw_fence_create(e.dev, 42);
	REQUIRE(f);
	CHECK_EQ(pw_fence_seqno(f), 42);
	CHECK(!pw_fence_is_signalled(f));
	CHECK_EQ(pw_fence_error(f), 0);
	CHECK_EQ(pw_fence_signal_time_ns(f), 0);
	CHECK(pw_fence_get(f) == f);
	CHECK(pw_fence_get(NULL) == NULL);
	pw_fence_put(f);
	pw_fence_put(NULL);
	t0 = tnow(&e);
	CHECK(pw_fence_signal(f, PW_EIO));
	CHECK(pw_fence_is_signalled(f));
	CHECK_EQ(pw_fence_error(f), PW_EIO);
	CHECK(pw_fence_signal_time_ns(f) >= t0 && pw_fence_signal_time_ns(f) <= tnow(&e));
	/* signal once: later calls ignored, error kept */
	CHECK(!pw_fence_signal(f, 0));
	CHECK(!pw_fence_signal(f, PW_ENOMEM));
	CHECK_EQ(pw_fence_error(f), PW_EIO);
	CHECK_EQ(pw_fence_wait(f, 0), PW_OK);
	CHECK(!pw_fence_signal(NULL, 0));
	pw_fence_put(f);

	s = pw_fence_create_signalled(e.dev, PW_ECANCELED);
	REQUIRE(s);
	CHECK(pw_fence_is_signalled(s));
	CHECK_EQ(pw_fence_error(s), PW_ECANCELED);
	CHECK(pw_fence_signal_time_ns(s) != 0);
	CHECK(!pw_fence_signal(s, 0));
	CHECK_EQ(pw_fence_wait(s, UINT64_MAX), PW_OK);
	{
		struct cb_rec r = { 0 };

		CHECK_EQ(pw_fence_add_callback(s, &r.cb, rec_cb), PW_ENOENT);
		CHECK(!pw_fence_remove_callback(s, &r.cb));
		CHECK_EQ(r.calls, 0);
	}
	pw_fence_put(s);
	env_fini(&e);
}

static void test_fence_callbacks(void)
{
	struct env e;
	struct pw_fence *f;
	struct cb_rec r[4];
	int counter = 0;

	ENV_INIT(&e);
	memset(r, 0, sizeof(r));
	f = pw_fence_create(e.dev, 1);
	REQUIRE(f);
	for (int i = 0; i < 4; i++) {
		r[i].counter = &counter;
		CHECK_EQ(pw_fence_add_callback(f, &r[i].cb, rec_cb), PW_OK);
	}
	CHECK(pw_fence_remove_callback(f, &r[1].cb));
	CHECK(!pw_fence_remove_callback(f, &r[1].cb));	/* already removed */
	CHECK(pw_fence_signal(f, 0));
	CHECK_EQ(r[0].calls, 1);
	CHECK_EQ(r[1].calls, 0);
	CHECK_EQ(r[2].calls, 1);
	CHECK_EQ(r[3].calls, 1);
	/* registration order */
	CHECK_EQ(r[0].order, 1);
	CHECK_EQ(r[2].order, 2);
	CHECK_EQ(r[3].order, 3);
	CHECK(!pw_fence_remove_callback(f, &r[0].cb));	/* already ran */
	CHECK_EQ(pw_fence_add_callback(f, &r[1].cb, rec_cb), PW_ENOENT);
	CHECK_EQ(r[1].calls, 0);
	pw_fence_put(f);
	env_fini(&e);
}

/* remove_callback() while the callback is running must return false (pw_fence.h contract). */
struct slow_cb {
	struct pw_fence_cb cb;
	int entered, release, done;	/* __atomic */
};

static void slow_fn(struct pw_fence *f, struct pw_fence_cb *cb)
{
	struct slow_cb *s = (struct slow_cb *)cb;

	(void)f;
	__atomic_store_n(&s->entered, 1, __ATOMIC_SEQ_CST);
	while (!__atomic_load_n(&s->release, __ATOMIC_SEQ_CST))
		usleep(100);
	__atomic_store_n(&s->done, 1, __ATOMIC_SEQ_CST);
}

static void *signal_thread(void *arg)
{
	pw_fence_signal(arg, 0);
	return NULL;
}

static bool flag_set(void *arg)
{
	return __atomic_load_n((int *)arg, __ATOMIC_SEQ_CST) != 0;
}

static void test_fence_remove_while_running(void)
{
	struct env e;
	struct slow_cb s = { 0 };
	struct pw_fence *f;
	pthread_t t;

	ENV_INIT(&e);
	f = pw_fence_create(e.dev, 1);
	REQUIRE(f);
	CHECK_EQ(pw_fence_add_callback(f, &s.cb, slow_fn), PW_OK);
	pthread_create(&t, NULL, signal_thread, f);
	CHECK(wait_until(flag_set, &s.entered, 2000));
	CHECK(!pw_fence_remove_callback(f, &s.cb));	/* running: caller must synchronise */
	CHECK(!s.done);
	__atomic_store_n(&s.release, 1, __ATOMIC_SEQ_CST);
	pthread_join(t, NULL);
	CHECK(s.done);
	pw_fence_put(f);
	env_fini(&e);
}

struct delayed_signal {
	struct pw_fence *f;
	unsigned int delay_ms;
	int error;
};

static void *delayed_signal_thread(void *arg)
{
	struct delayed_signal *d = arg;

	usleep(d->delay_ms * 1000u);
	pw_fence_signal(d->f, d->error);
	return NULL;
}

static void test_fence_wait(void)
{
	struct env e;
	struct pw_fence *f;
	struct delayed_signal d;
	uint64_t t0, dt;
	pthread_t t;

	ENV_INIT(&e);
	f = pw_fence_create(e.dev, 7);
	REQUIRE(f);
	/* deadline in the past / poll */
	CHECK_EQ(pw_fence_wait(f, 0), PW_ETIME);
	CHECK_EQ(pw_fence_wait(f, tnow(&e)), PW_ETIME);
	/* measured short timeout */
	t0 = tnow(&e);
	CHECK_EQ(pw_fence_wait(f, t0 + 30 * MS), PW_ETIME);
	dt = tnow(&e) - t0;
	CHECK(dt >= 30 * MS);
	CHECK(dt < 1000 * MS);
	/* signalled by another thread while waiting forever; error does not change the result */
	d = (struct delayed_signal){ f, 20, PW_EIO };
	t0 = tnow(&e);
	pthread_create(&t, NULL, delayed_signal_thread, &d);
	CHECK_EQ(pw_fence_wait(f, UINT64_MAX), PW_OK);
	CHECK(tnow(&e) - t0 >= 15 * MS);
	pthread_join(t, NULL);
	CHECK_EQ(pw_fence_error(f), PW_EIO);
	CHECK_EQ(pw_fence_wait(f, 0), PW_OK);
	pw_fence_put(f);
	env_fini(&e);
}

/* Many waiters + racing add/remove/signal on a set of fences. */
#define NF 64

struct fence_stress {
	struct env *e;
	struct pw_fence *f[NF];
	int ok;		/* __atomic */
	int cbs;	/* __atomic */
};

static void count_cb(struct pw_fence *f, struct pw_fence_cb *cb)
{
	(void)f;
	__atomic_fetch_add((int *)((struct cb_rec *)cb)->counter, 1, __ATOMIC_SEQ_CST);
}

static void *stress_waiter(void *arg)
{
	struct fence_stress *s = arg;

	for (int i = 0; i < NF; i++) {
		if (pw_fence_wait(s->f[i], UINT64_MAX) == PW_OK && pw_fence_is_signalled(s->f[i]))
			__atomic_fetch_add(&s->ok, 1, __ATOMIC_SEQ_CST);
	}
	return NULL;
}

static void *stress_cb(void *arg)
{
	struct fence_stress *s = arg;
	struct cb_rec r[NF];
	int mine = 0;	/* __atomic: callbacks of this thread that ran / were accounted */

	memset(r, 0, sizeof(r));
	for (int i = 0; i < NF; i++) {
		r[i].counter = &mine;
		if (pw_fence_add_callback(s->f[i], &r[i].cb, count_cb) == PW_OK) {
			/* false: ran or running; count_cb accounts for it */
			if (pw_fence_remove_callback(s->f[i], &r[i].cb))
				__atomic_fetch_add(&mine, 1, __ATOMIC_SEQ_CST);
		} else {
			__atomic_fetch_add(&mine, 1, __ATOMIC_SEQ_CST);
		}
	}
	/* r[] lives on this stack: wait for callbacks still running elsewhere */
	while (__atomic_load_n(&mine, __ATOMIC_SEQ_CST) != NF)
		usleep(100);
	__atomic_fetch_add(&s->cbs, NF, __ATOMIC_SEQ_CST);
	return NULL;
}

static void *stress_signaller(void *arg)
{
	struct fence_stress *s = arg;

	for (int i = 0; i < NF; i++) {
		pw_fence_signal(s->f[i], 0);
		if (i % 8 == 0)
			usleep(200);
	}
	return NULL;
}

static bool cbs_done(void *arg)
{
	struct fence_stress *s = arg;

	return __atomic_load_n(&s->cbs, __ATOMIC_SEQ_CST) == 4 * NF;
}

static void test_fence_concurrent(void)
{
	struct env e;
	struct fence_stress s = { 0 };
	pthread_t w[4], c[4], sig[2];

	ENV_INIT(&e);
	s.e = &e;
	for (int i = 0; i < NF; i++)
		s.f[i] = pw_fence_create(e.dev, (uint64_t)i);
	for (int i = 0; i < 4; i++) {
		pthread_create(&w[i], NULL, stress_waiter, &s);
		pthread_create(&c[i], NULL, stress_cb, &s);
	}
	for (int i = 0; i < 2; i++)
		pthread_create(&sig[i], NULL, stress_signaller, &s);
	for (int i = 0; i < 2; i++)
		pthread_join(sig[i], NULL);
	for (int i = 0; i < 4; i++) {
		pthread_join(w[i], NULL);
		pthread_join(c[i], NULL);
	}
	CHECK_EQ(s.ok, 4 * NF);
	/* each (thread, fence) pair accounted exactly once: removed, ran, or ENOENT */
	CHECK(wait_until(cbs_done, &s, 2000));
	for (int i = 0; i < NF; i++)
		pw_fence_put(s.f[i]);
	env_fini(&e);
}

/* ------------------------------------------------------------------------------------------ */
/* ioctls                                                                                     */
/* ------------------------------------------------------------------------------------------ */

static void test_create_destroy(void)
{
	struct env e;
	uint32_t h1, h2, h3, first;

	ENV_INIT(&e);
	CHECK_EQ(sc_create(&e, 2, &h1), PW_EINVAL);
	CHECK_EQ(sc_create(&e, 0, &h1), 0);
	CHECK_EQ(sc_create(&e, DRM_SYNCOBJ_CREATE_SIGNALED, &h2), 0);
	CHECK(h1 >= 1 && h2 >= 1 && h1 != h2);
	/* unsignalled-without-fence vs signalled */
	CHECK_EQ(sc_wait(&e, &h1, 1, 0, 0, NULL), PW_EINVAL);
	CHECK_EQ(sc_wait(&e, &h2, 1, 0, 0, &first), 0);
	CHECK_EQ(first, 0);
	CHECK_EQ(q1(&e, h2, 0), 0);	/* binary: query reports 0 */
	{
		struct drm_syncobj_destroy a = { .handle = h1, .pad = 1 };

		CHECK_EQ(IOC(&e, DRM_IOCTL_SYNCOBJ_DESTROY, &a), PW_EINVAL);
	}
	CHECK_EQ(sc_destroy(&e, h1), 0);
	CHECK_EQ(sc_destroy(&e, h1), PW_EINVAL);
	CHECK_EQ(sc_destroy(&e, 0), PW_EINVAL);
	CHECK_EQ(sc_destroy(&e, 9999), PW_EINVAL);
	CHECK_EQ(sc_wait(&e, &h1, 1, 0, 0, NULL), PW_ENOENT);
	h3 = mk(&e, 0);
	CHECK_EQ(h3, h1);	/* lowest free handle reused */
	/* handles left open are released by pw_file_close() */
	env_fini(&e);
}

static void test_wait_errors(void)
{
	struct env e;
	uint32_t h[2], first = 7, bad[2];

	ENV_INIT(&e);
	h[0] = mk(&e, DRM_SYNCOBJ_CREATE_SIGNALED);
	h[1] = mk(&e, 0);
	bad[0] = h[0];
	bad[1] = 1234;
	/* count 0: success, nothing written */
	{
		struct drm_syncobj_wait a = { .handles = 0, .count_handles = 0,
					      .first_signaled = 77 };

		CHECK_EQ(IOC(&e, DRM_IOCTL_SYNCOBJ_WAIT, &a), 0);
		CHECK_EQ(a.first_signaled, 77);
	}
	CHECK_EQ(sc_twait(&e, h, NULL, 0, 0, 0, NULL), 0);
	/* invalid flags: WAIT_AVAILABLE only exists for TIMELINE_WAIT */
	CHECK_EQ(sc_wait(&e, h, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, 0, NULL), PW_EINVAL);
	CHECK_EQ(sc_wait(&e, h, 1, 1u << 7, 0, NULL), PW_EINVAL);
	CHECK_EQ(sc_twait(&e, h, NULL, 1, 1u << 7, 0, NULL), PW_EINVAL);
	CHECK_EQ(sc_wait(&e, bad, 2, 0, 0, NULL), PW_ENOENT);
	CHECK_EQ(sc_wait(&e, NULL, 2, 0, 0, NULL), PW_EFAULT);
	CHECK_EQ(sc_wait(&e, h, PW_SYNC_MAX_HANDLES + 1, 0, 0, NULL), PW_EINVAL);
	/* missing fence: EINVAL without WAIT_FOR_SUBMIT even if another one is signalled */
	CHECK_EQ(sc_wait(&e, h, 2, 0, 0, NULL), PW_EINVAL);
	CHECK_EQ(sc_wait(&e, h, 2, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, 0, NULL), PW_EINVAL);
	/* with WAIT_FOR_SUBMIT: any -> the signalled one; all -> timeout */
	CHECK_EQ(sc_wait(&e, h, 2, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, 0, &first), 0);
	CHECK_EQ(first, 0);
	CHECK_EQ(sc_wait(&e, h, 2, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
			 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, 0, &first), PW_ETIME);
	/* WAIT_DEADLINE is accepted and ignored */
	CHECK_EQ(sc_wait(&e, h, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE, 0, &first), 0);
	/* timeline wait with a NULL points array == all points 0 */
	CHECK_EQ(sc_twait(&e, h, NULL, 1, 0, 0, &first), 0);
	CHECK_EQ(first, 0);
	/* a point on a binary syncobj has not been "submitted" */
	CHECK_EQ(twait1(&e, h[0], 1, 0, 0), PW_EINVAL);
	CHECK_EQ(twait1(&e, h[0], 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, 0), PW_ETIME);
	env_fini(&e);
}

static void test_wait_any_all(void)
{
	struct env e;
	struct pw_fence *f[3];
	uint32_t h[3], first;

	ENV_INIT(&e);
	for (int i = 0; i < 3; i++) {
		h[i] = mk(&e, 0);
		f[i] = pw_fence_create(e.dev, (uint64_t)i + 1);
		REQUIRE(f[i]);
		CHECK_EQ(pw_syncobj_attach_fence(e.file, h[i], 0, f[i]), 0);
	}
	CHECK_EQ(sc_wait(&e, h, 3, 0, 0, &first), PW_ETIME);
	CHECK_EQ(first, 0xdeadbeef);	/* untouched on error */
	pw_fence_signal(f[2], 0);
	CHECK_EQ(sc_wait(&e, h, 3, 0, 0, &first), 0);
	CHECK_EQ(first, 2);
	CHECK_EQ(sc_wait(&e, h, 3, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, 0, &first), PW_ETIME);
	pw_fence_signal(f[1], PW_EIO);	/* fence errors don't fail waits */
	CHECK_EQ(sc_wait(&e, h, 3, 0, 0, &first), 0);
	CHECK_EQ(first, 1);
	pw_fence_signal(f[0], 0);
	CHECK_EQ(sc_wait(&e, h, 3, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, 0, &first), 0);
	CHECK_EQ(first, 0);	/* WAIT_ALL: first signalled in the pre-pass */
	for (int i = 0; i < 3; i++)
		pw_fence_put(f[i]);
	env_fini(&e);
}

struct ioctl_waiter {
	struct env *e;
	uint32_t handles[4];
	uint64_t points[4];
	uint32_t count;
	uint32_t flags;
	int64_t timeout;
	bool timeline;
	int ret;
	uint32_t first;
	uint64_t elapsed;
	int done;	/* __atomic */
};

static void *ioctl_waiter_thread(void *arg)
{
	struct ioctl_waiter *w = arg;
	uint64_t t0 = tnow(w->e);

	if (w->timeline)
		w->ret = sc_twait(w->e, w->handles, w->points, w->count, w->flags, w->timeout,
				  &w->first);
	else
		w->ret = sc_wait(w->e, w->handles, w->count, w->flags, w->timeout, &w->first);
	w->elapsed = tnow(w->e) - t0;
	__atomic_store_n(&w->done, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

static void test_wait_blocking_any(void)
{
	struct env e;
	struct pw_fence *f[3];
	struct ioctl_waiter w = { 0 };
	pthread_t t;

	ENV_INIT(&e);
	w.e = &e;
	w.count = 3;
	w.timeout = INT64_MAX;
	for (int i = 0; i < 3; i++) {
		w.handles[i] = mk(&e, 0);
		f[i] = pw_fence_create(e.dev, 0);
		pw_syncobj_attach_fence(e.file, w.handles[i], 0, f[i]);
	}
	pthread_create(&t, NULL, ioctl_waiter_thread, &w);
	usleep(20000);
	CHECK(!__atomic_load_n(&w.done, __ATOMIC_SEQ_CST));
	pw_fence_signal(f[1], 0);
	pthread_join(t, NULL);
	CHECK_EQ(w.ret, 0);
	CHECK_EQ(w.first, 1);
	CHECK(w.elapsed >= 15 * MS);

	/* WAIT_ALL blocks until the last one */
	memset(&w.done, 0, sizeof(w.done));
	w.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	pthread_create(&t, NULL, ioctl_waiter_thread, &w);
	usleep(10000);
	pw_fence_signal(f[0], 0);
	usleep(10000);
	CHECK(!__atomic_load_n(&w.done, __ATOMIC_SEQ_CST));
	pw_fence_signal(f[2], 0);
	pthread_join(t, NULL);
	CHECK_EQ(w.ret, 0);
	CHECK_EQ(w.first, 1);	/* f[1] was already signalled at the pre-pass */
	for (int i = 0; i < 3; i++)
		pw_fence_put(f[i]);
	env_fini(&e);
}

static void test_wait_timeouts(void)
{
	struct env e;
	struct pw_fence *f;
	uint32_t h, first;
	uint64_t t0, dt;

	ENV_INIT(&e);
	h = mk(&e, 0);
	f = pw_fence_create(e.dev, 0);
	pw_syncobj_attach_fence(e.file, h, 0, f);
	/* 0 = poll, negative / past = poll */
	t0 = tnow(&e);
	CHECK_EQ(sc_wait(&e, &h, 1, 0, 0, &first), PW_ETIME);
	CHECK_EQ(sc_wait(&e, &h, 1, 0, -5, &first), PW_ETIME);
	CHECK_EQ(sc_wait(&e, &h, 1, 0, (int64_t)tnow(&e) - (int64_t)MS, &first), PW_ETIME);
	CHECK(tnow(&e) - t0 < 100 * MS);
	/* absolute deadline 50 ms ahead (binary + timeline) */
	t0 = tnow(&e);
	CHECK_EQ(sc_wait(&e, &h, 1, 0, (int64_t)(t0 + 50 * MS), &first), PW_ETIME);
	dt = tnow(&e) - t0;
	CHECK(dt >= 50 * MS);
	CHECK(dt < 1000 * MS);
	t0 = tnow(&e);
	CHECK_EQ(twait1(&e, h, 0, 0, (int64_t)(t0 + 30 * MS)), PW_ETIME);
	dt = tnow(&e) - t0;
	CHECK(dt >= 30 * MS);
	CHECK(dt < 1000 * MS);
	pw_fence_signal(f, 0);
	CHECK_EQ(sc_wait(&e, &h, 1, 0, (int64_t)(tnow(&e) + 1000 * MS), &first), 0);
	pw_fence_put(f);
	env_fini(&e);
}

static void test_wait_for_submit(void)
{
	struct env e;
	struct ioctl_waiter w = { 0 };
	struct pw_fence *f;
	uint64_t t0, dt;
	pthread_t t;
	uint32_t h;

	ENV_INIT(&e);
	h = mk(&e, 0);
	/* measured timeout with no fence ever attached */
	t0 = tnow(&e);
	CHECK_EQ(sc_wait(&e, &h, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
			 (int64_t)(t0 + 40 * MS), NULL), PW_ETIME);
	dt = tnow(&e) - t0;
	CHECK(dt >= 40 * MS && dt < 1000 * MS);

	/* waiter blocks until a fence is attached AND signalled */
	w.e = &e;
	w.count = 1;
	w.handles[0] = h;
	w.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
	w.timeout = (int64_t)(tnow(&e) + 5000 * MS);
	pthread_create(&t, NULL, ioctl_waiter_thread, &w);
	usleep(20000);
	CHECK(!__atomic_load_n(&w.done, __ATOMIC_SEQ_CST));
	f = pw_fence_create(e.dev, 1);
	CHECK_EQ(pw_syncobj_attach_fence(e.file, h, 0, f), 0);
	usleep(20000);
	CHECK(!__atomic_load_n(&w.done, __ATOMIC_SEQ_CST));	/* attached, not signalled */
	pw_fence_signal(f, 0);
	pthread_join(t, NULL);
	CHECK_EQ(w.ret, 0);
	CHECK_EQ(w.first, 0);
	CHECK(w.elapsed >= 35 * MS);
	pw_fence_put(f);

	/* timeline: WAIT_AVAILABLE returns as soon as point 5 is submitted (unsignalled) */
	memset(&w, 0, sizeof(w));
	w.e = &e;
	w.count = 1;
	w.timeline = true;
	w.handles[0] = mk(&e, 0);
	w.points[0] = 5;
	w.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;
	w.timeout = INT64_MAX;
	pthread_create(&t, NULL, ioctl_waiter_thread, &w);
	usleep(10000);
	f = pw_fence_create(e.dev, 3);
	CHECK_EQ(pw_syncobj_attach_fence(e.file, w.handles[0], 3, f), 0);	/* too low */
	usleep(10000);
	CHECK(!__atomic_load_n(&w.done, __ATOMIC_SEQ_CST));
	pw_fence_put(f);
	f = pw_fence_create(e.dev, 5);
	CHECK_EQ(pw_syncobj_attach_fence(e.file, w.handles[0], 5, f), 0);
	pthread_join(t, NULL);
	CHECK_EQ(w.ret, 0);
	CHECK(!pw_fence_is_signalled(f));
	/* ... while a plain WAIT_FOR_SUBMIT wait on it still times out */
	CHECK_EQ(twait1(&e, w.handles[0], 5, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, 0),
		 PW_ETIME);
	pw_fence_signal(f, 0);
	pw_fence_put(f);
	/* point 5 needs point 3 too */
	CHECK_EQ(twait1(&e, w.handles[0], 5, 0, 0), PW_ETIME);
	{
		struct pw_fence *f3 = NULL;

		CHECK_EQ(pw_syncobj_find_fence(e.file, w.handles[0], 3, 0, 0, &f3), 0);
		REQUIRE(f3);
		pw_fence_signal(f3, 0);	/* point 3's payload is the first node: f3 itself */
		pw_fence_put(f3);
	}
	CHECK_EQ(twait1(&e, w.handles[0], 5, 0, 0), 0);
	env_fini(&e);
}

static void test_reset_signal(void)
{
	struct env e;
	uint32_t h[2], bad[2];

	ENV_INIT(&e);
	h[0] = mk(&e, 0);
	h[1] = mk(&e, 0);
	bad[0] = h[0];
	bad[1] = 77;
	CHECK_EQ(sc_array(&e, DRM_IOCTL_SYNCOBJ_SIGNAL, h, 2, 1), PW_EINVAL);	/* pad */
	CHECK_EQ(sc_array(&e, DRM_IOCTL_SYNCOBJ_SIGNAL, h, 0, 0), PW_EINVAL);	/* count */
	CHECK_EQ(sc_array(&e, DRM_IOCTL_SYNCOBJ_SIGNAL, bad, 2, 0), PW_ENOENT);
	CHECK_EQ(sc_array(&e, DRM_IOCTL_SYNCOBJ_SIGNAL, NULL, 2, 0), PW_EFAULT);
	CHECK_EQ(sc_wait(&e, h, 1, 0, 0, NULL), PW_EINVAL);	/* ENOENT left it alone */
	CHECK_EQ(sc_array(&e, DRM_IOCTL_SYNCOBJ_SIGNAL, h, 2, 0), 0);
	CHECK_EQ(sc_wait(&e, h, 2, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, 0, NULL), 0);
	CHECK_EQ(sc_array(&e, DRM_IOCTL_SYNCOBJ_RESET, h, 2, 1), PW_EINVAL);
	CHECK_EQ(sc_array(&e, DRM_IOCTL_SYNCOBJ_RESET, h, 0, 0), PW_EINVAL);
	CHECK_EQ(sc_array(&e, DRM_IOCTL_SYNCOBJ_RESET, bad, 2, 0), PW_ENOENT);
	CHECK_EQ(sc_array(&e, DRM_IOCTL_SYNCOBJ_RESET, &h[1], 1, 0), 0);
	CHECK_EQ(sc_wait(&e, &h[1], 1, 0, 0, NULL), PW_EINVAL);
	CHECK_EQ(sc_wait(&e, &h[0], 1, 0, 0, NULL), 0);
	/* reset of a timeline drops it */
	CHECK_EQ(tsig1(&e, h[1], 4), 0);
	CHECK_EQ(q1(&e, h[1], 0), 4);
	CHECK_EQ(sc_array(&e, DRM_IOCTL_SYNCOBJ_RESET, &h[1], 1, 0), 0);
	CHECK_EQ(q1(&e, h[1], 0), 0);
	CHECK_EQ(twait1(&e, h[1], 4, 0, 0), PW_EINVAL);
	/* SIGNAL on a timeline replaces it with a binary fence */
	CHECK_EQ(tsig1(&e, h[1], 9), 0);
	CHECK_EQ(sc_array(&e, DRM_IOCTL_SYNCOBJ_SIGNAL, &h[1], 1, 0), 0);
	CHECK_EQ(q1(&e, h[1], 0), 0);
	CHECK_EQ(sc_wait(&e, &h[1], 1, 0, 0, NULL), 0);
	env_fini(&e);
}

static void test_timeline_signal_query(void)
{
	struct env e;
	uint32_t h[2], bad[2];
	uint64_t pts[2] = { 3, 10 }, out[2];

	ENV_INIT(&e);
	h[0] = mk(&e, 0);
	h[1] = mk(&e, 0);
	bad[0] = h[0];
	bad[1] = 55;
	CHECK_EQ(q1(&e, h[0], 0), 0);
	CHECK_EQ(q1(&e, h[0], DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED), 0);
	/* TIMELINE_SIGNAL errors */
	CHECK_EQ(sc_tsignal(&e, h, pts, 2, 1), PW_EINVAL);
	CHECK_EQ(sc_tsignal(&e, h, pts, 0, 0), PW_EINVAL);
	CHECK_EQ(sc_tsignal(&e, bad, pts, 2, 0), PW_ENOENT);
	CHECK_EQ(sc_tsignal(&e, NULL, pts, 2, 0), PW_EFAULT);
	CHECK_EQ(sc_tsignal(&e, h, pts, PW_SYNC_MAX_HANDLES + 1, 0), PW_EINVAL);
	/* QUERY errors */
	CHECK_EQ(sc_query(&e, h, out, 2, 2), PW_EINVAL);
	CHECK_EQ(sc_query(&e, h, out, 0, 0), PW_EINVAL);
	CHECK_EQ(sc_query(&e, bad, out, 2, 0), PW_ENOENT);
	CHECK_EQ(sc_query(&e, h, NULL, 2, 0), PW_EFAULT);

	CHECK_EQ(sc_tsignal(&e, h, pts, 2, 0), 0);
	CHECK_EQ(sc_query(&e, h, out, 2, 0), 0);
	CHECK_EQ(out[0], 3);
	CHECK_EQ(out[1], 10);
	for (uint64_t p = 11; p <= 20; p++)
		CHECK_EQ(tsig1(&e, h[1], p), 0);
	CHECK_EQ(q1(&e, h[1], 0), 20);
	CHECK_EQ(q1(&e, h[1], DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED), 20);
	/* waits: submitted points are signalled; future ones are not submitted */
	CHECK_EQ(twait1(&e, h[1], 1, 0, 0), 0);
	CHECK_EQ(twait1(&e, h[1], 15, 0, 0), 0);
	CHECK_EQ(twait1(&e, h[1], 20, 0, 0), 0);
	CHECK_EQ(twait1(&e, h[1], 21, 0, 0), PW_EINVAL);
	CHECK_EQ(twait1(&e, h[1], 21, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, 0), PW_ETIME);
	CHECK_EQ(twait1(&e, h[1], 21, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, 0), PW_ETIME);
	CHECK_EQ(twait1(&e, h[1], 20, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, 0), 0);
	/* binary wait on a timeline waits for its last point */
	CHECK_EQ(sc_wait(&e, &h[1], 1, 0, 0, NULL), 0);
	/* NULL points array == point 0 for every handle (Linux adds a point 0 chain link) */
	CHECK_EQ(sc_tsignal(&e, &h[0], NULL, 1, 0), 0);
	CHECK_EQ(q1(&e, h[0], DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED), 3);
	CHECK_EQ(q1(&e, h[0], 0), 3);
	env_fini(&e);
}

/* Out-of-order completion: a point is only signalled once it and every earlier point are. */
static void test_timeline_ordering(void)
{
	struct env e;
	struct pw_fence *f[4], *k2 = NULL, *k3 = NULL, *k1 = NULL;
	uint32_t h;

	ENV_INIT(&e);
	h = mk(&e, 0);
	for (int i = 1; i <= 3; i++) {
		f[i] = pw_fence_create(e.dev, (uint64_t)i);
		CHECK_EQ(pw_syncobj_attach_fence(e.file, h, (uint64_t)i, f[i]), 0);
	}
	CHECK_EQ(q1(&e, h, 0), 0);
	CHECK_EQ(q1(&e, h, DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED), 3);
	CHECK_EQ(pw_syncobj_find_fence(e.file, h, 1, 0, 0, &k1), 0);
	CHECK(k1 == f[1]);		/* oldest pending point: the payload itself */
	CHECK_EQ(pw_syncobj_find_fence(e.file, h, 2, 0, 0, &k2), 0);
	CHECK_EQ(pw_syncobj_find_fence(e.file, h, 3, 0, 0, &k3), 0);
	REQUIRE(k1 && k2 && k3);
	CHECK(k2 != f[2]);		/* chain fence: waits for point 1 as well */
	CHECK_EQ(pw_fence_seqno(k2), 2);

	pw_fence_signal(f[2], PW_EIO);
	CHECK_EQ(q1(&e, h, 0), 0);
	CHECK(!pw_fence_is_signalled(k2));
	CHECK_EQ(twait1(&e, h, 2, 0, 0), PW_ETIME);
	pw_fence_signal(f[1], 0);
	CHECK(pw_fence_is_signalled(k2));
	CHECK_EQ(pw_fence_error(k2), PW_EIO);	/* the point's own payload error */
	CHECK(!pw_fence_is_signalled(k3));
	CHECK_EQ(q1(&e, h, 0), 2);
	CHECK_EQ(twait1(&e, h, 2, 0, 0), 0);
	CHECK_EQ(twait1(&e, h, 3, 0, 0), PW_ETIME);
	/* points 1 and 2 were collected: asking for them gives a fresh signalled fence */
	{
		struct pw_fence *g = NULL;

		CHECK_EQ(pw_syncobj_find_fence(e.file, h, 1, 0, 0, &g), 0);
		REQUIRE(g);
		CHECK(g != f[1] && pw_fence_is_signalled(g));
		pw_fence_put(g);
	}
	pw_fence_signal(f[3], 0);
	CHECK(pw_fence_is_signalled(k3));
	CHECK_EQ(q1(&e, h, 0), 3);
	CHECK_EQ(twait1(&e, h, 3, 0, 0), 0);
	pw_fence_put(k1);
	pw_fence_put(k2);
	pw_fence_put(k3);
	for (int i = 1; i <= 3; i++)
		pw_fence_put(f[i]);
	env_fini(&e);
}

/* dma_fence_chain_init(): a point <= the last starts a new context with seqno = max(). */
static void test_timeline_unordered(void)
{
	struct env e;
	struct pw_fence *f5, *f3;
	uint32_t h, g;

	ENV_INIT(&e);
	/* signalled points */
	h = mk(&e, 0);
	CHECK_EQ(tsig1(&e, h, 5), 0);
	CHECK_EQ(tsig1(&e, h, 3), 0);
	CHECK_EQ(q1(&e, h, DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED), 5);
	CHECK_EQ(q1(&e, h, 0), 5);
	CHECK_EQ(twait1(&e, h, 3, 0, 0), 0);
	CHECK_EQ(twait1(&e, h, 6, 0, 0), PW_EINVAL);

	/* pending points: the unordered head depends on everything before it */
	g = mk(&e, 0);
	f5 = pw_fence_create(e.dev, 5);
	f3 = pw_fence_create(e.dev, 3);
	pw_syncobj_attach_fence(e.file, g, 5, f5);
	pw_syncobj_attach_fence(e.file, g, 3, f3);
	CHECK_EQ(q1(&e, g, DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED), 5);
	pw_fence_signal(f3, 0);
	CHECK_EQ(q1(&e, g, 0), 0);	/* head's prev_seqno is 0 in a new context */
	CHECK_EQ(twait1(&e, g, 3, 0, 0), PW_ETIME);
	CHECK_EQ(twait1(&e, g, 5, 0, 0), PW_ETIME);
	pw_fence_signal(f5, 0);
	CHECK_EQ(q1(&e, g, 0), 5);
	CHECK_EQ(twait1(&e, g, 3, 0, 0), 0);
	pw_fence_put(f5);
	pw_fence_put(f3);
	env_fini(&e);
}

/* Signalled points are garbage-collected; long pending chains complete without recursion. */
static void test_timeline_gc(void)
{
	struct env e;
	enum { N = 2000 };
	struct pw_fence **f;
	long base, peak;
	uint32_t h;

	ENV_INIT(&e);
	h = mk(&e, 0);
	CHECK_EQ(tsig1(&e, h, 1), 0);
	base = mock_live_allocs(e.os);
	for (uint64_t p = 2; p <= N; p++)
		CHECK_EQ(tsig1(&e, h, p), 0);
	CHECK(mock_live_allocs(e.os) - base < 8);
	CHECK_EQ(q1(&e, h, 0), N);

	/* N pending points, completed newest-first: all chain fences signal, then GC */
	f = calloc(N + 1, sizeof(*f));
	REQUIRE(f);
	base = mock_live_allocs(e.os);
	for (uint64_t p = N + 1; p <= 2 * N; p++) {
		f[p - N] = pw_fence_create(e.dev, p);
		CHECK_EQ(pw_syncobj_attach_fence(e.file, h, p, f[p - N]), 0);
	}
	{
		struct pw_fence *mid = NULL;

		CHECK_EQ(pw_syncobj_find_fence(e.file, h, N + N / 2, 0, 0, &mid), 0);
		for (int i = N; i >= 2; i--)
			pw_fence_signal(f[i], 0);
		REQUIRE(mid);
		CHECK(!pw_fence_is_signalled(mid));
		CHECK_EQ(q1(&e, h, 0), N);
		pw_fence_signal(f[1], 0);
		CHECK(pw_fence_is_signalled(mid));
		pw_fence_put(mid);
	}
	CHECK_EQ(q1(&e, h, 0), 2 * N);
	peak = mock_live_allocs(e.os);
	for (int i = 1; i <= N; i++)
		pw_fence_put(f[i]);
	free(f);
	CHECK(peak - base >= N - 4);	/* our fences, alive until here */
	CHECK(mock_live_allocs(e.os) - base < 8);	/* only the head remains */
	env_fini(&e);
}

static void test_transfer(void)
{
	struct env e;
	struct pw_fence *f;
	uint32_t b, t, b2;

	ENV_INIT(&e);
	b = mk(&e, 0);
	t = mk(&e, 0);
	b2 = mk(&e, 0);
	CHECK_EQ(sc_transfer(&e, t, 1, b, 0, 0, 1), PW_EINVAL);		/* pad */
	CHECK_EQ(sc_transfer(&e, 99, 1, b, 0, 0, 0), PW_ENOENT);	/* dst */
	CHECK_EQ(sc_transfer(&e, t, 1, 99, 0, 0, 0), PW_ENOENT);	/* src */
	CHECK_EQ(sc_transfer(&e, t, 1, b, 0, 0, 0), PW_EINVAL);		/* no fence */
	CHECK_EQ(sc_transfer(&e, t, 1, b, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, 0), PW_EINVAL);
	CHECK_EQ(sc_transfer(&e, b2, 0, b, 0, 0, 0), PW_EINVAL);

	/* binary -> timeline point 7 */
	f = pw_fence_create(e.dev, 1);
	pw_syncobj_attach_fence(e.file, b, 0, f);
	CHECK_EQ(sc_transfer(&e, t, 7, b, 0, 0, 0), 0);
	CHECK_EQ(q1(&e, t, DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED), 7);
	CHECK_EQ(q1(&e, t, 0), 0);
	CHECK_EQ(twait1(&e, t, 7, 0, 0), PW_ETIME);
	/* timeline point 7 -> binary */
	CHECK_EQ(sc_transfer(&e, b2, 0, t, 7, 0, 0), 0);
	CHECK_EQ(sc_wait(&e, &b2, 1, 0, 0, NULL), PW_ETIME);
	/* a not-yet-submitted point */
	CHECK_EQ(sc_transfer(&e, b2, 0, t, 8, 0, 0), PW_EINVAL);
	pw_fence_signal(f, 0);
	CHECK_EQ(q1(&e, t, 0), 7);
	CHECK_EQ(sc_wait(&e, &b2, 1, 0, 0, NULL), 0);
	CHECK_EQ(twait1(&e, t, 7, 0, 0), 0);
	/* timeline -> timeline */
	CHECK_EQ(sc_transfer(&e, t, 9, t, 7, 0, 0), 0);
	CHECK_EQ(q1(&e, t, 0), 9);
	/* timeline point onto a binary syncobj that already has a fence: the binary fence becomes
	 * a dependency of the new point */
	pw_fence_put(f);
	f = pw_fence_create(e.dev, 2);
	pw_syncobj_attach_fence(e.file, b, 0, f);
	CHECK_EQ(tsig1(&e, b, 4), 0);
	CHECK_EQ(q1(&e, b, DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED), 4);
	CHECK_EQ(twait1(&e, b, 4, 0, 0), PW_ETIME);
	CHECK_EQ(q1(&e, b, 0), 0);
	pw_fence_signal(f, 0);
	CHECK_EQ(twait1(&e, b, 4, 0, 0), 0);
	CHECK_EQ(q1(&e, b, 0), 4);
	pw_fence_put(f);
	env_fini(&e);
}

struct attacher {
	struct env *e;
	uint32_t handle;
	uint64_t point;
	unsigned int delay_ms;
	struct pw_fence *fence;
};

static void *attacher_thread(void *arg)
{
	struct attacher *a = arg;

	usleep(a->delay_ms * 1000u);
	CHECK_EQ(pw_syncobj_attach_fence(a->e->file, a->handle, a->point, a->fence), 0);
	return NULL;
}

static void test_transfer_wait_for_submit(void)
{
	struct env e;
	struct attacher a;
	uint32_t src, dst;
	pthread_t t;

	ENV_INIT(&e);
	src = mk(&e, 0);
	dst = mk(&e, 0);
	a = (struct attacher){ &e, src, 2, 20, pw_fence_create_signalled(e.dev, 0) };
	pthread_create(&t, NULL, attacher_thread, &a);
	CHECK_EQ(sc_transfer(&e, dst, 0, src, 2, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, 0), 0);
	pthread_join(t, NULL);
	CHECK_EQ(sc_wait(&e, &dst, 1, 0, 0, NULL), 0);
	pw_fence_put(a.fence);
	env_fini(&e);
}

static void test_kernel_find_attach(void)
{
	struct env e;
	struct pw_fence *f, *out = (struct pw_fence *)1;
	struct attacher a;
	uint64_t t0, dt;
	pthread_t t;
	uint32_t h;

	ENV_INIT(&e);
	h = mk(&e, 0);
	CHECK_EQ(pw_syncobj_find_fence(e.file, 4242, 0, 0, 0, &out), PW_EINVAL);
	CHECK(out == NULL);
	CHECK_EQ(pw_syncobj_find_fence(e.file, h, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, 0, &out),
		 PW_EINVAL);
	CHECK_EQ(pw_syncobj_find_fence(e.file, h, 0, 0, UINT64_MAX, &out), PW_EINVAL);
	t0 = tnow(&e);
	CHECK_EQ(pw_syncobj_find_fence(e.file, h, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
				       t0 + 30 * MS, &out), PW_ETIME);
	dt = tnow(&e) - t0;
	CHECK(dt >= 30 * MS && dt < 1000 * MS);
	CHECK(out == NULL);
	CHECK_EQ(pw_syncobj_attach_fence(e.file, 4242, 0, NULL), PW_EINVAL);

	/* WAIT_FOR_SUBMIT: blocks until the attach, returns the attached fence */
	f = pw_fence_create(e.dev, 77);
	a = (struct attacher){ &e, h, 0, 20, f };
	pthread_create(&t, NULL, attacher_thread, &a);
	CHECK_EQ(pw_syncobj_find_fence(e.file, h, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
				       UINT64_MAX, &out), 0);
	pthread_join(t, NULL);
	CHECK(out == f);
	pw_fence_put(out);
	/* same for a timeline point */
	a = (struct attacher){ &e, h, 3, 20, f };
	pthread_create(&t, NULL, attacher_thread, &a);
	CHECK_EQ(pw_syncobj_find_fence(e.file, h, 3, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
				       tnow(&e) + 5000 * MS, &out), 0);
	pthread_join(t, NULL);
	REQUIRE(out);
	/* the binary fence f is the base of the new timeline, so point 3 waits on both */
	CHECK(!pw_fence_is_signalled(out));
	pw_fence_signal(f, 0);
	CHECK(pw_fence_is_signalled(out));
	pw_fence_put(out);
	pw_fence_put(f);

	/* object API: preallocated point can't fail; unused prealloc is freed */
	{
		struct pw_syncobj *so = pw_syncobj_lookup(e.file, h);
		struct pw_syncobj_point *pt = pw_syncobj_point_alloc(e.dev);
		struct pw_fence *s = pw_fence_create_signalled(e.dev, 0);

		REQUIRE(so && pt && s);
		CHECK(pw_syncobj_lookup(e.file, 999) == NULL);
		pw_syncobj_add_point(so, pt, s, 10);
		CHECK_EQ(q1(&e, h, 0), 10);
		pw_syncobj_point_free(pw_syncobj_point_alloc(e.dev));
		pw_syncobj_point_free(NULL);
		pw_syncobj_replace_fence(so, NULL);
		CHECK_EQ(q1(&e, h, 0), 0);
		CHECK(pw_syncobj_get(so) == so);
		pw_syncobj_put(so);
		pw_syncobj_put(so);
		pw_syncobj_put(NULL);
		pw_fence_put(s);
	}
	env_fini(&e);
}

/* ------------------------------------------------------------------------------------------ */
/* notify (PANWIN_IOCTL_SYNCOBJ_EVENT backend)                                                */
/* ------------------------------------------------------------------------------------------ */

struct notify_ctx {
	int calls;	/* __atomic */
};

static void notify_fn(void *ctx)
{
	__atomic_fetch_add(&((struct notify_ctx *)ctx)->calls, 1, __ATOMIC_SEQ_CST);
}

static void test_notify(void)
{
	struct env e;
	struct notify_ctx c[6];
	struct pw_fence *f;
	void *ck[6];
	uint32_t hs, h0, ht;

	ENV_INIT(&e);
	memset(c, 0, sizeof(c));
	hs = mk(&e, DRM_SYNCOBJ_CREATE_SIGNALED);
	h0 = mk(&e, 0);
	ht = mk(&e, 0);
	/* errors */
	CHECK_EQ(pw_file_syncobj_notify(e.file, 999, 0, 0, notify_fn, &c[0], &ck[0]), PW_ENOENT);
	CHECK_EQ(pw_file_syncobj_notify(e.file, hs, 0, 1u << 9, notify_fn, &c[0], &ck[0]),
		 PW_EINVAL);
	CHECK_EQ(pw_file_syncobj_notify(e.file, h0, 0, 0, notify_fn, &c[0], &ck[0]), PW_EINVAL);
	CHECK(ck[0] == NULL);
	CHECK_EQ(c[0].calls, 0);

	/* already signalled: fires from the call */
	CHECK_EQ(pw_file_syncobj_notify(e.file, hs, 0, 0, notify_fn, &c[0], &ck[0]), 0);
	CHECK_EQ(c[0].calls, 1);
	CHECK(!pw_file_syncobj_notify_cancel(e.file, ck[0]));	/* after fire */
	CHECK_EQ(c[0].calls, 1);

	/* fires on signal, exactly once */
	f = pw_fence_create(e.dev, 1);
	pw_syncobj_attach_fence(e.file, h0, 0, f);
	CHECK_EQ(pw_file_syncobj_notify(e.file, h0, 0, 0, notify_fn, &c[1], &ck[1]), 0);
	/* cancelled before firing */
	CHECK_EQ(pw_file_syncobj_notify(e.file, h0, 0, 0, notify_fn, &c[2], &ck[2]), 0);
	CHECK(ck[1] != ck[2]);
	CHECK(pw_file_syncobj_notify_cancel(e.file, ck[2]));
	CHECK(!pw_file_syncobj_notify_cancel(e.file, ck[2]));
	CHECK_EQ(c[1].calls, 0);
	pw_fence_signal(f, 0);
	CHECK_EQ(c[1].calls, 1);
	CHECK_EQ(c[2].calls, 0);
	CHECK(!pw_file_syncobj_notify_cancel(e.file, ck[1]));
	CHECK(!pw_file_syncobj_notify_cancel(e.file, NULL));
	pw_fence_put(f);

	/* WAIT_FOR_SUBMIT: waits for the attach, then for the signal */
	CHECK_EQ(pw_file_syncobj_notify(e.file, ht, 2, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
					notify_fn, &c[3], &ck[3]), 0);
	/* WAIT_AVAILABLE: fires on the attach */
	CHECK_EQ(pw_file_syncobj_notify(e.file, ht, 2, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
					notify_fn, &c[4], &ck[4]), 0);
	/* still waiting for submit when cancelled */
	CHECK_EQ(pw_file_syncobj_notify(e.file, ht, 9, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
					notify_fn, &c[5], &ck[5]), 0);
	CHECK(pw_file_syncobj_notify_cancel(e.file, ck[5]));
	f = pw_fence_create(e.dev, 2);
	CHECK_EQ(pw_syncobj_attach_fence(e.file, ht, 1, f), 0);	/* point 1: not yet */
	CHECK_EQ(c[3].calls + c[4].calls, 0);
	pw_fence_put(f);
	f = pw_fence_create(e.dev, 2);
	CHECK_EQ(pw_syncobj_attach_fence(e.file, ht, 2, f), 0);
	CHECK_EQ(c[3].calls, 0);
	CHECK_EQ(c[4].calls, 1);
	pw_fence_signal(f, 0);
	CHECK_EQ(c[3].calls, 0);	/* point 1 is still pending */
	{
		struct pw_fence *p1 = NULL;

		CHECK_EQ(pw_syncobj_find_fence(e.file, ht, 1, 0, 0, &p1), 0);
		REQUIRE(p1);
		pw_fence_signal(p1, 0);
		pw_fence_put(p1);
	}
	CHECK_EQ(c[3].calls, 1);
	CHECK_EQ(c[5].calls, 0);
	pw_fence_put(f);

	/* left pending at close: dropped without calling fn, no leak */
	CHECK_EQ(pw_file_syncobj_notify(e.file, ht, 50, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
					notify_fn, &c[5], &ck[5]), 0);
	f = pw_fence_create(e.dev, 3);
	pw_syncobj_attach_fence(e.file, h0, 0, f);
	CHECK_EQ(pw_file_syncobj_notify(e.file, h0, 0, 0, notify_fn, &c[5], &ck[5]), 0);
	CHECK_EQ(sc_destroy(&e, h0), 0);	/* the notify keeps the syncobj alive */
	pw_file_close(e.file);
	e.file = NULL;
	pw_fence_signal(f, 0);
	CHECK_EQ(c[5].calls, 0);
	pw_fence_put(f);
	CHECK_EQ(pw_file_open(e.dev, &e.file), 0);
	env_fini(&e);
}

/* fire vs cancel: exactly one of {fn called once, cancel returned true}. */
struct race_ctx {
	struct pw_fence *f;
	int go;		/* __atomic */
};

static void *race_signal_thread(void *arg)
{
	struct race_ctx *r = arg;

	while (!__atomic_load_n(&r->go, __ATOMIC_SEQ_CST))
		;
	pw_fence_signal(r->f, 0);
	return NULL;
}

static void test_notify_cancel_race(void)
{
	struct env e;
	int fired = 0, cancelled = 0;
	uint32_t h;

	ENV_INIT(&e);
	h = mk(&e, 0);
	for (int i = 0; i < 400; i++) {
		struct race_ctx r = { 0 };
		struct notify_ctx c = { 0 };
		pthread_t t;
		void *ck;
		bool cret;
		bool submit = i & 1;

		r.f = pw_fence_create(e.dev, (uint64_t)i);
		if (!submit)
			pw_syncobj_attach_fence(e.file, h, 0, r.f);
		else
			CHECK_EQ(sc_array(&e, DRM_IOCTL_SYNCOBJ_RESET, &h, 1, 0), 0);
		CHECK_EQ(pw_file_syncobj_notify(e.file, h, 0,
						submit ? DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT : 0,
						notify_fn, &c, &ck), 0);
		if (submit)
			pw_syncobj_attach_fence(e.file, h, 0, r.f);
		pthread_create(&t, NULL, race_signal_thread, &r);
		__atomic_store_n(&r.go, 1, __ATOMIC_SEQ_CST);
		if (i % 3)
			usleep((unsigned int)(i % 7) * 10u);
		cret = pw_file_syncobj_notify_cancel(e.file, ck);
		pthread_join(t, NULL);
		/* after join the callback (if any) has completed */
		CHECK(c.calls <= 1);
		CHECK((c.calls == 1) != cret);
		fired += c.calls;
		cancelled += cret;
		pw_fence_put(r.f);
	}
	fprintf(stderr, "    notify race: %d fired, %d cancelled\n", fired, cancelled);
	CHECK_EQ(fired + cancelled, 400);
	env_fini(&e);
}

/* ------------------------------------------------------------------------------------------ */
/* destroy with pending waiters, concurrency                                                  */
/* ------------------------------------------------------------------------------------------ */

static void test_destroy_pending_waiters(void)
{
	struct env e;
	struct ioctl_waiter w1 = { 0 }, w2 = { 0 };
	struct pw_fence *f;
	pthread_t t1, t2;
	uint32_t h1, h2;

	ENV_INIT(&e);
	h1 = mk(&e, 0);
	h2 = mk(&e, 0);
	f = pw_fence_create(e.dev, 1);
	pw_syncobj_attach_fence(e.file, h1, 0, f);
	w1 = (struct ioctl_waiter){ .e = &e, .handles = { h1 }, .count = 1,
				    .timeout = INT64_MAX };
	w2 = (struct ioctl_waiter){ .e = &e, .handles = { h2 }, .count = 1,
				    .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
				    .timeout = (int64_t)(tnow(&e) + 80 * MS) };
	pthread_create(&t1, NULL, ioctl_waiter_thread, &w1);
	pthread_create(&t2, NULL, ioctl_waiter_thread, &w2);
	usleep(20000);
	CHECK_EQ(sc_destroy(&e, h1), 0);
	CHECK_EQ(sc_destroy(&e, h2), 0);
	usleep(10000);
	CHECK(!__atomic_load_n(&w1.done, __ATOMIC_SEQ_CST));
	/* the fence still signals the waiter of the destroyed syncobj */
	pw_fence_signal(f, 0);
	pthread_join(t1, NULL);
	CHECK_EQ(w1.ret, 0);
	/* nobody can attach to a destroyed handle: the WAIT_FOR_SUBMIT waiter times out */
	pthread_join(t2, NULL);
	CHECK_EQ(w2.ret, PW_ETIME);
	pw_fence_put(f);

	/* chain fences handed out survive a RESET / DESTROY of their timeline */
	{
		struct pw_fence *a = pw_fence_create(e.dev, 1), *b = pw_fence_create(e.dev, 2);
		struct pw_fence *k = NULL;
		uint32_t h = mk(&e, 0);

		pw_syncobj_attach_fence(e.file, h, 1, a);
		pw_syncobj_attach_fence(e.file, h, 2, b);
		CHECK_EQ(pw_syncobj_find_fence(e.file, h, 2, 0, 0, &k), 0);
		REQUIRE(k);
		CHECK_EQ(sc_array(&e, DRM_IOCTL_SYNCOBJ_RESET, &h, 1, 0), 0);
		CHECK_EQ(sc_destroy(&e, h), 0);
		pw_fence_signal(b, 0);
		CHECK(!pw_fence_is_signalled(k));
		pw_fence_signal(a, 0);
		CHECK(pw_fence_is_signalled(k));
		pw_fence_put(k);
		pw_fence_put(a);
		pw_fence_put(b);
	}
	env_fini(&e);
}

/* Several threads wait on increasing timeline points (any/all, submit or not) while two
 * signaller threads attach and signal points in parallel. */
#define CONC_POINTS 200

struct conc {
	struct env *e;
	uint32_t h[2];
	int fails;	/* __atomic */
	int stop;	/* __atomic */
};

struct conc_sig {
	struct conc *c;
	int which;
};

static void *conc_signaller(void *arg)
{
	struct conc_sig *s = arg;
	struct conc *c = s->c;

	for (uint64_t p = 1; p <= CONC_POINTS; p++) {
		struct pw_fence *f = pw_fence_create(c->e->dev, p);

		pw_syncobj_attach_fence(c->e->file, c->h[s->which], p, f);
		if (p % 5 == 0)
			usleep(100);
		pw_fence_signal(f, 0);
		pw_fence_put(f);
	}
	return NULL;
}

static void *conc_waiter(void *arg)
{
	struct conc *c = arg;

	for (uint64_t p = 1; p <= CONC_POINTS; p += 7) {
		uint64_t pts[2] = { p, p };
		int ret = sc_twait(c->e, c->h, pts, 2, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
				   DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
				   (int64_t)(tnow(c->e) + 5000 * MS), NULL);

		if (ret)
			__atomic_fetch_add(&c->fails, 1, __ATOMIC_SEQ_CST);
	}
	return NULL;
}

static void *conc_querier(void *arg)
{
	struct conc *c = arg;
	uint64_t last = 0;

	while (!__atomic_load_n(&c->stop, __ATOMIC_SEQ_CST)) {
		uint64_t v = 0;

		if (sc_query(c->e, &c->h[0], &v, 1, 0) || v < last)
			__atomic_fetch_add(&c->fails, 1, __ATOMIC_SEQ_CST);
		last = v;
	}
	return NULL;
}

static void test_concurrent_timeline(void)
{
	struct env e;
	struct conc c = { 0 };
	struct conc_sig s[2];
	pthread_t sig[2], w[4], q;

	ENV_INIT(&e);
	c.e = &e;
	c.h[0] = mk(&e, 0);
	c.h[1] = mk(&e, 0);
	pthread_create(&q, NULL, conc_querier, &c);
	for (int i = 0; i < 4; i++)
		pthread_create(&w[i], NULL, conc_waiter, &c);
	for (int i = 0; i < 2; i++) {
		s[i] = (struct conc_sig){ &c, i };
		pthread_create(&sig[i], NULL, conc_signaller, &s[i]);
	}
	for (int i = 0; i < 2; i++)
		pthread_join(sig[i], NULL);
	for (int i = 0; i < 4; i++)
		pthread_join(w[i], NULL);
	__atomic_store_n(&c.stop, 1, __ATOMIC_SEQ_CST);
	pthread_join(q, NULL);
	CHECK_EQ(c.fails, 0);
	CHECK_EQ(q1(&e, c.h[0], 0), CONC_POINTS);
	CHECK_EQ(q1(&e, c.h[1], 0), CONC_POINTS);
	env_fini(&e);
}

/* ------------------------------------------------------------------------------------------ */
/* review additions                                                                           */
/* ------------------------------------------------------------------------------------------ */

/*
 * TRANSFER h:k -> h:k+1 makes point k's chain fence the payload of point k+1. Signalling the
 * bottom of such a chain must not recurse once per link (pw_fence_signal -> tl_node_cb ->
 * tl_advance -> pw_fence_signal ...): userspace controls the depth, and a Windows kernel stack is
 * a few dozen KiB. The signal runs on a thread with a small stack so recursion would crash.
 */
#define CHAIN_DEPTH 20000

struct chain_sig {
	struct pw_fence *f[2];
};

static void *chain_signal_thread(void *arg)
{
	struct chain_sig *s = arg;

	pw_fence_signal(s->f[0], 0);
	pw_fence_signal(s->f[1], 0);
	return NULL;
}

static void test_transfer_chain_depth(void)
{
	struct env e;
	struct chain_sig s;
	pthread_attr_t attr;
	pthread_t t;
	uint32_t h;

	ENV_INIT(&e);
	h = mk(&e, 0);
	s.f[0] = pw_fence_create(e.dev, 1);
	s.f[1] = pw_fence_create(e.dev, 2);
	REQUIRE(s.f[0] && s.f[1]);
	CHECK_EQ(pw_syncobj_attach_fence(e.file, h, 1, s.f[0]), 0);
	CHECK_EQ(pw_syncobj_attach_fence(e.file, h, 2, s.f[1]), 0);
	for (uint64_t k = 2; k < CHAIN_DEPTH; k++)
		CHECK_EQ(sc_transfer(&e, h, k + 1, h, k, 0, 0), 0);
	CHECK_EQ(q1(&e, h, DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED), CHAIN_DEPTH);
	CHECK_EQ(q1(&e, h, 0), 0);

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	REQUIRE(pthread_create(&t, &attr, chain_signal_thread, &s) == 0);
	pthread_join(t, NULL);
	pthread_attr_destroy(&attr);
	CHECK_EQ(q1(&e, h, 0), CHAIN_DEPTH);
	CHECK_EQ(twait1(&e, h, CHAIN_DEPTH, 0, 0), 0);
	pw_fence_put(s.f[0]);
	pw_fence_put(s.f[1]);

	/* Same, alternating between two timelines (A:k -> B:k+1 -> A:k+2 ...). */
	{
		uint32_t ab[2] = { mk(&e, 0), mk(&e, 0) };
		struct pw_fence *g = pw_fence_create(e.dev, 3);

		s.f[0] = pw_fence_create(e.dev, 1);
		s.f[1] = pw_fence_create(e.dev, 2);
		REQUIRE(g && s.f[0] && s.f[1]);
		CHECK_EQ(pw_syncobj_attach_fence(e.file, ab[0], 1, s.f[0]), 0);
		CHECK_EQ(pw_syncobj_attach_fence(e.file, ab[0], 2, g), 0);
		CHECK_EQ(pw_syncobj_attach_fence(e.file, ab[1], 1, s.f[1]), 0);
		for (uint64_t k = 2; k < CHAIN_DEPTH; k++)
			CHECK_EQ(sc_transfer(&e, ab[(k + 1) & 1], k + 1, ab[k & 1], k, 0, 0), 0);
		pw_fence_signal(g, 0);
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 256 * 1024);
		REQUIRE(pthread_create(&t, &attr, chain_signal_thread, &s) == 0);
		pthread_join(t, NULL);
		pthread_attr_destroy(&attr);
		CHECK_EQ(q1(&e, ab[CHAIN_DEPTH & 1], 0), CHAIN_DEPTH);
		CHECK_EQ(q1(&e, ab[(CHAIN_DEPTH - 1) & 1], 0), CHAIN_DEPTH - 1);
		pw_fence_put(g);
		pw_fence_put(s.f[0]);
		pw_fence_put(s.f[1]);
	}
	env_fini(&e);
}

/*
 * pw_file_syncobj_notify() racing with the fence signal / attach that fires it: once the notify
 * is armed, another thread may fire it and drop the last reference before the registering call
 * returns. The cookie must still be valid (ASan catches a use after free) and fn runs once.
 */
struct notify_race {
	struct env *e;
	uint32_t h;
	struct pw_fence *f;
	bool attach;
	bool destroy;
	int go;		/* __atomic */
};

static void *notify_race_thread(void *arg)
{
	struct notify_race *r = arg;

	while (!__atomic_load_n(&r->go, __ATOMIC_SEQ_CST))
		;
	if (r->destroy)
		CHECK_EQ(sc_destroy(r->e, r->h), 0);
	if (r->attach)
		pw_syncobj_attach_fence(r->e->file, r->h, 0, r->f);
	else
		pw_fence_signal(r->f, 0);
	return NULL;
}

static void test_notify_register_race(void)
{
	struct env e;
	uint32_t h;

	ENV_INIT(&e);
	h = mk(&e, 0);
	for (int i = 0; i < 1000; i++) {
		struct notify_race r = { .e = &e, .h = h, .attach = i & 1 };
		struct notify_ctx c = { 0 };
		pthread_t t;
		void *ck = NULL;

		CHECK_EQ(sc_array(&e, DRM_IOCTL_SYNCOBJ_RESET, &h, 1, 0), 0);
		if (r.attach) {
			r.f = pw_fence_create_signalled(e.dev, 0);
		} else {
			r.f = pw_fence_create(e.dev, (uint64_t)i);
			pw_syncobj_attach_fence(e.file, h, 0, r.f);
		}
		pthread_create(&t, NULL, notify_race_thread, &r);
		__atomic_store_n(&r.go, 1, __ATOMIC_SEQ_CST);
		CHECK_EQ(pw_file_syncobj_notify(e.file, h, 0,
						DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
						notify_fn, &c, &ck), 0);
		pthread_join(t, NULL);
		CHECK(ck != NULL);
		CHECK_EQ(c.calls, 1);
		CHECK(!pw_file_syncobj_notify_cancel(e.file, ck));
		pw_fence_put(r.f);
	}
	/* The handle destroyed (and the fence signalled) while the notify is being registered:
	 * the registration must not drop the last syncobj reference under its lock. */
	for (int i = 0; i < 1000; i++) {
		struct notify_race r = { .e = &e, .destroy = true };
		struct notify_ctx c = { 0 };
		pthread_t t;
		void *ck = NULL;
		int ret;

		r.h = mk(&e, 0);
		r.f = pw_fence_create(e.dev, (uint64_t)i);
		pw_syncobj_attach_fence(e.file, r.h, 0, r.f);
		pthread_create(&t, NULL, notify_race_thread, &r);
		__atomic_store_n(&r.go, 1, __ATOMIC_SEQ_CST);
		ret = pw_file_syncobj_notify(e.file, r.h, 0, 0, notify_fn, &c, &ck);
		pthread_join(t, NULL);
		CHECK(ret == 0 || ret == PW_ENOENT);
		CHECK_EQ(c.calls, ret == 0 ? 1 : 0);
		pw_fence_put(r.f);
	}
	env_fini(&e);
}

/* Linux semantics that the other tests do not pin down. */
static void test_semantics_misc(void)
{
	struct env e;
	struct pw_fence *f, *out = NULL;
	uint32_t b, t, hs[2], first;
	uint64_t pts[2];

	ENV_INIT(&e);
	b = mk(&e, 0);
	t = mk(&e, 0);

	/* A signalled fence's error does not make WAIT fail (dma_fence_wait returns 0). */
	f = pw_fence_create(e.dev, 1);
	pw_syncobj_attach_fence(e.file, b, 0, f);
	pw_fence_signal(f, PW_EIO);
	CHECK_EQ(pw_fence_error(f), PW_EIO);
	CHECK_EQ(sc_wait(&e, &b, 1, 0, 0, NULL), 0);
	pw_fence_put(f);

	/* Binary syncobj, point > 0: EINVAL (dma_fence_chain_find_seqno on a non-chain fence). */
	CHECK_EQ(twait1(&e, b, 1, 0, 0), PW_EINVAL);
	CHECK_EQ(pw_syncobj_find_fence(e.file, b, 1, 0, 0, &out), PW_EINVAL);
	CHECK(out == NULL);
	/* ... and WAIT_FOR_SUBMIT on it times out (no point can appear without a timeline). */
	CHECK_EQ(twait1(&e, b, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
			(int64_t)(tnow(&e) + 10 * MS)), PW_ETIME);

	/* TIMELINE_WAIT with a NULL points array waits on point 0 of each handle. */
	CHECK_EQ(tsig1(&e, t, 5), 0);
	hs[0] = b;
	hs[1] = t;
	CHECK_EQ(sc_twait(&e, hs, NULL, 2, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, 0, &first), 0);
	CHECK_EQ(first, 0);

	/* The same syncobj twice in one TIMELINE_SIGNAL: both points are added in order. */
	hs[0] = hs[1] = t;
	pts[0] = 6;
	pts[1] = 8;
	CHECK_EQ(sc_tsignal(&e, hs, pts, 2, 0), 0);
	CHECK_EQ(q1(&e, t, 0), 8);
	CHECK_EQ(q1(&e, t, DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED), 8);

	/* RESET does not satisfy a WAIT_FOR_SUBMIT waiter; a later SIGNAL does. */
	CHECK_EQ(sc_array(&e, DRM_IOCTL_SYNCOBJ_RESET, &t, 1, 0), 0);
	CHECK_EQ(sc_wait(&e, &t, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
			 (int64_t)(tnow(&e) + 10 * MS), NULL), PW_ETIME);
	CHECK_EQ(sc_array(&e, DRM_IOCTL_SYNCOBJ_SIGNAL, &t, 1, 0), 0);
	CHECK_EQ(sc_wait(&e, &t, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, 0, NULL), 0);

	/* count_handles over the cap is refused before anything is read. */
	CHECK_EQ(sc_wait(&e, hs, PW_SYNC_MAX_HANDLES + 1, 0, 0, NULL), PW_EINVAL);
	env_fini(&e);
}

const struct test_case sync_tests[] = {
	{ "sync_fence_basic", test_fence_basic },
	{ "sync_fence_callbacks", test_fence_callbacks },
	{ "sync_fence_remove_while_running", test_fence_remove_while_running },
	{ "sync_fence_wait", test_fence_wait },
	{ "sync_fence_concurrent", test_fence_concurrent },
	{ "sync_create_destroy", test_create_destroy },
	{ "sync_wait_errors", test_wait_errors },
	{ "sync_wait_any_all", test_wait_any_all },
	{ "sync_wait_blocking_any_all", test_wait_blocking_any },
	{ "sync_wait_timeouts", test_wait_timeouts },
	{ "sync_wait_for_submit", test_wait_for_submit },
	{ "sync_reset_signal", test_reset_signal },
	{ "sync_timeline_signal_query", test_timeline_signal_query },
	{ "sync_timeline_ordering", test_timeline_ordering },
	{ "sync_timeline_unordered", test_timeline_unordered },
	{ "sync_timeline_gc", test_timeline_gc },
	{ "sync_transfer", test_transfer },
	{ "sync_transfer_wait_for_submit", test_transfer_wait_for_submit },
	{ "sync_kernel_find_attach", test_kernel_find_attach },
	{ "sync_notify", test_notify },
	{ "sync_notify_cancel_race", test_notify_cancel_race },
	{ "sync_destroy_pending_waiters", test_destroy_pending_waiters },
	{ "sync_concurrent_timeline", test_concurrent_timeline },
	{ "sync_review_transfer_chain_depth", test_transfer_chain_depth },
	{ "sync_review_notify_register_race", test_notify_register_race },
	{ "sync_review_semantics_misc", test_semantics_misc },
	{ NULL, NULL },
};
