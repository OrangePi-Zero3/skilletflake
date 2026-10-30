// SPDX-License-Identifier: MIT
/*
 * test_jm_sched.c - job-manager scheduling (docs/status/jm-arch.md tasks C1-C8): SUBMIT and
 * JM_CTX_* through pw_file_ioctl with the real panfrost_drm.h structs, on the mock G31 with
 * jm-hw's job-chain executor (mock_jm.c) and jm-mem's BOs. Job chains are Mesa genxml v7.xml
 * Job Headers + WRITE_VALUE payloads written through the user CPU mapping of a BO, like Mesa
 * does. Owner: jm-sched.
 *
 * Fence errors are not visible through the uAPI (SYNCOBJ_WAIT succeeds for failed jobs too, as
 * on Linux), so the tests read them with the kernel API (pw_syncobj_find_fence +
 * pw_fence_error).
 */
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "pw_bo.h"
#include "pw_jm_hw.h"
#include "pw_jm_mem.h"
#include "pw_jm_sched.h"
#include "pw_mmu.h"
#include "pw_sync.h"
#include "test_jm.h"

#define FIOC(f, req, arg) pw_file_ioctl((f), _IOC_NR(req), (arg), _IOC_SIZE(req))

/* Poll @cond every 1 ms for up to @ms. */
#define WAIT_FOR(cond, ms) do {							\
		for (unsigned int _i = 0; _i < (ms) && !(cond); _i++)		\
			usleep(1000);						\
	} while (0)

/* ---- BOs with a user CPU view ---- */

struct ubo {
	uint32_t handle;
	uint64_t va;
	uint8_t *cpu;		/* NULL for HEAP BOs */
	uint32_t size;
	uint32_t next_job;	/* next free 128-byte job slot */
};

static bool ubo_create(struct pw_os *os, struct pw_file *f, uint32_t size, uint32_t flags,
		       struct ubo *b)
{
	struct drm_panfrost_create_bo cb = { .size = size, .flags = flags };
	struct drm_panfrost_mmap_bo mb = { 0 };
	struct pw_mmap_target t;

	memset(b, 0, sizeof(*b));
	size = (size + 4095u) & ~4095u;
	cb.size = size;
	if (FIOC(f, DRM_IOCTL_PANFROST_CREATE_BO, &cb)) {
		CHECK(!"CREATE_BO failed");
		return false;
	}
	b->handle = cb.handle;
	b->va = cb.offset;
	b->size = size;
	if (flags & PANFROST_BO_HEAP)
		return true;
	mb.handle = cb.handle;
	if (FIOC(f, DRM_IOCTL_PANFROST_MMAP_BO, &mb) ||
	    pw_file_mmap_lookup(f, mb.offset, size, &t)) {
		CHECK(!"MMAP_BO failed");
		return false;
	}
	/* The handle keeps the BO (and its memory) alive; uncached memory: the view is what
	 * the GPU sees. */
	b->cpu = mock_mem_user_view(os, pw_bo_os_mem(t.bo));
	pw_bo_put(t.bo);
	memset(b->cpu, 0, size);
	return b->cpu != NULL;
}

static void put32(uint8_t *p, uint32_t v)
{
	memcpy(p, &v, 4);
}

static void put64(uint8_t *p, uint64_t v)
{
	memcpy(p, &v, 8);
}

static uint32_t get32(const uint8_t *p)
{
	uint32_t v;

	memcpy(&v, p, 4);
	return v;
}

/* Mesa genxml v7.xml Job Header (32 bytes): exception status @0, Is 64b | Type << 1 |
 * Index << 16 @16, dependencies @20, next @24; WRITE_VALUE payload: address @32, type @40,
 * immediate @48. */
enum { JT_NULL = 1, JT_WRITE_VALUE = 2, JT_FRAGMENT = 9 };
enum { WV_IMM32 = 6, WV_IMM64 = 7 };

static void job_hdr(uint8_t *p, uint32_t type, uint32_t index, uint64_t next)
{
	memset(p, 0, 64);
	put32(p + 16, 1u | (type << 1) | (index << 16));
	put64(p + 24, next);
}

/* A slot of @b for one job; returns its VA. */
static uint64_t job_slot(struct ubo *b, uint8_t **cpu)
{
	uint32_t off = b->next_job++ * 128u;

	CHECK(off + 128u <= b->size);
	if (off + 128u > b->size)
		off = 0;
	*cpu = b->cpu + off;
	return b->va + off;
}

/* One-job chain: WRITE_VALUE @imm (32 bit) to @addr. */
static uint64_t wv_chain(struct ubo *b, uint64_t addr, uint32_t imm)
{
	uint8_t *p;
	uint64_t va = job_slot(b, &p);

	job_hdr(p, JT_WRITE_VALUE, 1, 0);
	put64(p + 32, addr);
	put32(p + 40, WV_IMM32);
	put64(p + 48, imm);
	return va;
}

/* One-job chain of @type without payload (NULL, FRAGMENT: completed without shaders). */
static uint64_t typed_chain(struct ubo *b, uint32_t type)
{
	uint8_t *p;
	uint64_t va = job_slot(b, &p);

	job_hdr(p, type, 1, 0);
	return va;
}

/* ---- syncobjs and submits ---- */

static uint32_t so_create(struct pw_file *f, bool signalled)
{
	struct drm_syncobj_create sc = { 0 };

	sc.flags = signalled ? DRM_SYNCOBJ_CREATE_SIGNALED : 0;
	CHECK_EQ(FIOC(f, DRM_IOCTL_SYNCOBJ_CREATE, &sc), PW_OK);
	return sc.handle;
}

/* SYNCOBJ_WAIT on one handle with a deadline @ms from now (0 = poll). */
static int so_wait(struct pw_os *os, struct pw_file *f, uint32_t h, uint32_t ms)
{
	struct drm_syncobj_wait w = { 0 };

	w.handles = (uint64_t)(uintptr_t)&h;
	w.count_handles = 1;
	w.timeout_nsec = ms ? (int64_t)(pw_os_time_ns(os) + (uint64_t)ms * 1000000u) : 0;
	return FIOC(f, DRM_IOCTL_SYNCOBJ_WAIT, &w);
}

/* The syncobj's current fence (a reference), or NULL. */
static struct pw_fence *so_fence(struct pw_file *f, uint32_t h)
{
	struct pw_fence *fe = NULL;

	if (pw_syncobj_find_fence(f, h, 0, 0, 0, &fe))
		return NULL;
	return fe;
}

/* Wait for the syncobj's fence and return its error (1 = no fence / not signalled). */
static int so_result(struct pw_os *os, struct pw_file *f, uint32_t h, uint32_t ms)
{
	struct pw_fence *fe = so_fence(f, h);
	int ret = 1;

	if (!fe)
		return 1;
	if (pw_fence_wait(fe, pw_os_time_ns(os) + (uint64_t)ms * 1000000u) == PW_OK)
		ret = pw_fence_error(fe);
	pw_fence_put(fe);
	return ret;
}

static int fence_result(struct pw_os *os, struct pw_fence *fe, uint32_t ms)
{
	if (pw_fence_wait(fe, pw_os_time_ns(os) + (uint64_t)ms * 1000000u) != PW_OK)
		return 1;
	return pw_fence_error(fe);
}

struct sub {
	uint64_t jc;
	uint32_t reqs;
	const uint32_t *bos;
	uint32_t nbos;
	const uint32_t *ins;
	uint32_t nins;
	uint32_t out;
	uint32_t ctx;
};

static int submit(struct pw_file *f, const struct sub *s)
{
	struct drm_panfrost_submit a;

	memset(&a, 0, sizeof(a));
	a.jc = s->jc;
	a.requirements = s->reqs;
	a.bo_handles = (uint64_t)(uintptr_t)s->bos;
	a.bo_handle_count = s->nbos;
	a.in_syncs = (uint64_t)(uintptr_t)s->ins;
	a.in_sync_count = s->nins;
	a.out_sync = s->out;
	a.jm_ctx_handle = s->ctx;
	return FIOC(f, DRM_IOCTL_PANFROST_SUBMIT, &a);
}

static uint32_t ctx_create(struct pw_file *f, uint32_t prio)
{
	struct drm_panfrost_jm_ctx_create cc = { .priority = prio };

	CHECK_EQ(FIOC(f, DRM_IOCTL_PANFROST_JM_CTX_CREATE, &cc), PW_OK);
	return cc.handle;
}

static int ctx_destroy(struct pw_file *f, uint32_t h)
{
	struct drm_panfrost_jm_ctx_destroy cd = { .handle = h };

	return FIOC(f, DRM_IOCTL_PANFROST_JM_CTX_DESTROY, &cd);
}

static int wait_bo(struct pw_os *os, struct pw_file *f, uint32_t h, int64_t timeout_ns_rel)
{
	struct drm_panfrost_wait_bo w = { .handle = h };

	if (timeout_ns_rel == INT64_MAX)
		w.timeout_ns = INT64_MAX;
	else if (timeout_ns_rel > 0)
		w.timeout_ns = (int64_t)pw_os_time_ns(os) + timeout_ns_rel;
	return FIOC(f, DRM_IOCTL_PANFROST_WAIT_BO, &w);
}

static struct mock_js_stats js_stats(struct jenv *e, unsigned int js)
{
	struct mock_js_stats st;

	mock_jm_js_stats(e->os, js, &st);
	return st;
}

static bool js_stalled(struct jenv *e, unsigned int js)
{
	return js_stats(e, js).stalled;
}

static bool js_hung(struct jenv *e, unsigned int js)
{
	return js_stats(e, js).hung;
}

static struct pw_jm_sched_stats sched_stats(struct jenv *e)
{
	struct pw_jm_sched_stats st;

	pw_jm_sched_get_stats(e->dev, &st);
	return st;
}

/* Every job freed (the work item drops references after the fences signalled). */
static bool sched_idle(struct jenv *e)
{
	for (int i = 0; i < 2000; i++) {
		if (!sched_stats(e).jobs_alive)
			return true;
		usleep(1000);
	}
	return false;
}

/* ------------------------------------------------------------------------------------------ */

static void test_jm_submit_validation(void)
{
	struct jenv e;
	struct drm_panfrost_submit s;
	struct pw_jm_sched_stats st;
	struct ubo j, d;
	struct pw_bo *bo;
	struct pw_fence *fe;
	uint32_t nofence, sig, bad = 999, h[2];

	if (!jenv_init(&e))
		goto out;
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &j));
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &d));
	nofence = so_create(e.file, false);
	sig = so_create(e.file, true);

	memset(&s, 0, sizeof(s));
	s.jc = wv_chain(&j, d.va, 0x1234);
	s.pad = 1;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SUBMIT, &s), PW_EINVAL);
	s.pad = 0;
	s.jc = 0;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SUBMIT, &s), PW_EINVAL);
	s.jc = j.va;
	s.requirements = 4;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SUBMIT, &s), PW_EINVAL);
	s.requirements = 0;
	/* out_sync before the context (ENODEV wins) */
	s.out_sync = bad;
	s.jm_ctx_handle = 5;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SUBMIT, &s), PW_ENODEV);
	s.out_sync = nofence;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SUBMIT, &s), PW_EINVAL);	/* unknown ctx */
	s.jm_ctx_handle = 1000;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SUBMIT, &s), PW_EINVAL);
	/* context before in_syncs: ENOENT would show it ran first */
	s.jm_ctx_handle = 5;
	s.in_syncs = (uint64_t)(uintptr_t)&bad;
	s.in_sync_count = 1;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SUBMIT, &s), PW_EINVAL);
	s.jm_ctx_handle = 0;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SUBMIT, &s), PW_ENOENT);
	s.in_syncs = (uint64_t)(uintptr_t)&nofence;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SUBMIT, &s), PW_EINVAL);	/* no fence */
	s.in_syncs = 0;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SUBMIT, &s), PW_EFAULT);
	s.in_sync_count = PW_JM_MAX_IN_SYNCS + 1;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SUBMIT, &s), PW_ENOMEM);
	/* in_syncs before bo_handles */
	h[0] = d.handle;
	h[1] = bad;
	s.in_syncs = (uint64_t)(uintptr_t)&nofence;
	s.in_sync_count = 1;
	s.bo_handles = (uint64_t)(uintptr_t)h;
	s.bo_handle_count = 2;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SUBMIT, &s), PW_EINVAL);
	s.in_syncs = (uint64_t)(uintptr_t)&sig;
	s.requirements = PANFROST_JD_REQ_CYCLE_COUNT;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SUBMIT, &s), PW_ENOENT);
	s.bo_handles = 0;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SUBMIT, &s), PW_EFAULT);
	s.bo_handle_count = PW_JM_MAX_JOB_BOS + 1;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SUBMIT, &s), PW_ENOMEM);

	/* nothing changed: out_sync still has no fence, the BO none, no job, no cycle counter */
	CHECK_EQ(so_wait(e.os, e.file, nofence, 0), PW_EINVAL);
	bo = pw_file_bo_get(e.file, d.handle);
	CHECK(bo != NULL);
	if (bo) {
		CHECK(pw_jm_bo_fence_get(bo) == NULL);
		pw_bo_put(bo);
	}
	st = sched_stats(&e);
	CHECK_EQ(st.submitted, 0);
	CHECK_EQ(st.jobs_alive, 0);
	CHECK_EQ(mock_reg(e.os, GPU_STATUS) & GPU_STATUS_CYCLE_COUNT_ACTIVE, 0);
	CHECK_EQ(get32(d.cpu), 0);

	/* the same submit, valid now: CYCLE_COUNT, a signalled in_sync, the BO twice */
	h[1] = d.handle;
	s.bo_handles = (uint64_t)(uintptr_t)h;
	s.bo_handle_count = 2;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SUBMIT, &s), PW_OK);
	CHECK_EQ(so_result(e.os, e.file, nofence, 2000), 0);
	CHECK_EQ(get32(d.cpu), 0x1234);
	CHECK(sched_idle(&e));
	/* the job held the cycle counter while queued and released it when freed */
	CHECK_EQ(mock_reg(e.os, GPU_STATUS) & GPU_STATUS_CYCLE_COUNT_ACTIVE, 0);
	bo = pw_file_bo_get(e.file, d.handle);
	fe = bo ? pw_jm_bo_fence_get(bo) : NULL;
	CHECK(fe != NULL && pw_fence_is_signalled(fe));
	pw_fence_put(fe);
	pw_bo_put(bo);
	st = sched_stats(&e);
	CHECK_EQ(st.submitted, 1);
	CHECK_EQ(st.completed, 1);
out:
	jenv_fini(&e);
}

static void test_jm_ctx(void)
{
	struct jenv e;
	struct drm_panfrost_jm_ctx_create cc;
	struct drm_panfrost_jm_ctx_destroy cd;
	struct ubo j, d;
	struct sub s = { 0 };
	uint32_t out, low;

	if (!jenv_init(&e))
		goto out;
	memset(&cc, 0, sizeof(cc));
	cc.priority = PANFROST_JM_CTX_PRIORITY_MEDIUM;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_JM_CTX_CREATE, &cc), PW_OK);
	CHECK_EQ(cc.handle, 1);
	memset(&cd, 0, sizeof(cd));
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_JM_CTX_DESTROY, &cd), PW_EINVAL);	/* default */
	cd.handle = cc.handle;
	cd.pad = 1;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_JM_CTX_DESTROY, &cd), PW_EINVAL);
	cd.pad = 0;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_JM_CTX_DESTROY, &cd), PW_OK);
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_JM_CTX_DESTROY, &cd), PW_EINVAL);
	cd.handle = 65;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_JM_CTX_DESTROY, &cd), PW_EINVAL);
	cc.priority = PANFROST_JM_CTX_PRIORITY_HIGH;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_JM_CTX_CREATE, &cc), PW_EACCES);
	cc.priority = 7;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_JM_CTX_CREATE, &cc), PW_EINVAL);

	/* handles 1..64 (xa_alloc in [0, 64]), then EBUSY; the lowest free handle is reused */
	for (uint32_t i = 1; i <= PW_JM_MAX_CTX_PER_FILE; i++)
		CHECK_EQ(ctx_create(e.file, i & 1 ? PANFROST_JM_CTX_PRIORITY_LOW :
					     PANFROST_JM_CTX_PRIORITY_MEDIUM), i);
	cc.priority = PANFROST_JM_CTX_PRIORITY_LOW;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_JM_CTX_CREATE, &cc), PW_EBUSY);
	CHECK_EQ(ctx_destroy(e.file, 10), PW_OK);
	CHECK_EQ(ctx_create(e.file, PANFROST_JM_CTX_PRIORITY_MEDIUM), 10);
	CHECK_EQ(ctx_destroy(e.file, 33), PW_OK);

	/* submits on a LOW context and on a destroyed one */
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &j));
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &d));
	low = 1;
	out = so_create(e.file, false);
	s.jc = wv_chain(&j, d.va, 77);
	s.out = out;
	s.ctx = 33;
	CHECK_EQ(submit(e.file, &s), PW_EINVAL);
	s.ctx = low;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	CHECK_EQ(so_wait(e.os, e.file, out, 2000), PW_OK);
	CHECK_EQ(so_result(e.os, e.file, out, 10), 0);
	CHECK_EQ(get32(d.cpu), 77);
out:
	jenv_fini(&e);	/* closes the file with 64 contexts */
}

/* A WRITE_VALUE chain from a user BO: memory written, descriptors DONE, out_sync signalled. */
static void test_jm_submit_write_value(void)
{
	struct jenv e;
	struct ubo j, d;
	struct mock_js_stats s0, s1;
	struct sub s = { 0 };
	uint32_t bos[2], out;
	uint8_t *p;
	uint64_t va;

	if (!jenv_init(&e))
		goto out;
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &j));
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &d));
	/* three chained jobs: IMM32, IMM64, IMM32 */
	va = job_slot(&j, &p);
	job_hdr(p, JT_WRITE_VALUE, 1, j.va + 128);
	put64(p + 32, d.va);
	put32(p + 40, WV_IMM32);
	put64(p + 48, 0xaaaa5555u);
	job_slot(&j, &p);
	job_hdr(p, JT_WRITE_VALUE, 2, j.va + 256);
	put64(p + 32, d.va + 8);
	put32(p + 40, WV_IMM64);
	put64(p + 48, 0x0102030405060708ull);
	job_slot(&j, &p);
	job_hdr(p, JT_WRITE_VALUE, 3, 0);
	put64(p + 32, d.va + 16);
	put32(p + 40, WV_IMM32);
	put64(p + 48, 0xbeef);
	bos[0] = j.handle;
	bos[1] = d.handle;
	out = so_create(e.file, false);
	s0 = js_stats(&e, 1);

	s.jc = va;
	s.bos = bos;
	s.nbos = 2;
	s.out = out;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	CHECK_EQ(so_wait(e.os, e.file, out, 2000), PW_OK);
	CHECK_EQ(so_result(e.os, e.file, out, 10), 0);
	CHECK_EQ(get32(d.cpu), 0xaaaa5555u);
	CHECK_EQ(get32(d.cpu + 8), 0x05060708u);
	CHECK_EQ(get32(d.cpu + 12), 0x01020304u);
	CHECK_EQ(get32(d.cpu + 16), 0xbeef);
	for (unsigned int i = 0; i < 3; i++)
		CHECK_EQ(get32(j.cpu + i * 128u), 1);	/* exception status DONE */
	s1 = js_stats(&e, 1);
	CHECK_EQ(s1.runs, s0.runs + 1);
	CHECK_EQ(s1.jobs, s0.jobs + 3);
	CHECK_EQ(s1.last_head, va);
	CHECK_EQ(js_stats(&e, 0).runs, 0);
	CHECK_EQ(wait_bo(e.os, e.file, d.handle, 0), PW_OK);

	/* panvk's pattern: the same syncobj as in_sync and out_sync */
	s.jc = wv_chain(&j, d.va, 0x2222);
	s.ins = &out;
	s.nins = 1;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	CHECK_EQ(so_wait(e.os, e.file, out, 2000), PW_OK);
	CHECK_EQ(get32(d.cpu), 0x2222);
	CHECK(sched_idle(&e));
out:
	jenv_fini(&e);
}

/* PANFROST_JD_REQ_FS -> JS0, everything else JS1 (panfrost_job_get_slot). */
static void test_jm_slot_select(void)
{
	struct jenv e;
	struct ubo j, d;
	struct sub s = { 0 };
	uint32_t out;

	if (!jenv_init(&e))
		goto out;
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &j));
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &d));
	out = so_create(e.file, false);
	s.out = out;

	s.jc = wv_chain(&j, d.va, 1);
	s.reqs = PANFROST_JD_REQ_FS;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	CHECK_EQ(so_result(e.os, e.file, out, 2000), 0);
	CHECK_EQ(js_stats(&e, 0).runs, 1);
	CHECK_EQ(js_stats(&e, 1).runs, 0);

	s.jc = typed_chain(&j, JT_FRAGMENT);
	CHECK_EQ(submit(e.file, &s), PW_OK);
	CHECK_EQ(so_result(e.os, e.file, out, 2000), 0);
	CHECK_EQ(js_stats(&e, 0).runs, 2);

	s.jc = wv_chain(&j, d.va, 2);
	s.reqs = 0;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	CHECK_EQ(so_result(e.os, e.file, out, 2000), 0);
	CHECK_EQ(js_stats(&e, 1).runs, 1);
	CHECK_EQ(get32(d.cpu), 2);

	/* a FRAGMENT job without FS lands on JS1, whose JS_FEATURES lack it: config fault */
	s.jc = typed_chain(&j, JT_FRAGMENT);
	CHECK_EQ(submit(e.file, &s), PW_OK);
	CHECK_EQ(so_result(e.os, e.file, out, 2000), PW_EINVAL);
	CHECK_EQ(js_stats(&e, 1).config_faults, 1);
	CHECK_EQ(js_stats(&e, 0).runs, 2);
out:
	jenv_fini(&e);
}

/* An in_sync whose fence comes from a job of another file (an imported sync file). */
static void test_jm_in_sync_cross_file(void)
{
	struct jenv e;
	struct pw_file *f2 = NULL;
	struct ubo j1, d1, j2, d2;
	struct sub a = { 0 }, b = { 0 };
	struct pw_fence *fa = NULL, *fb = NULL;
	uint32_t sa, sb, sc;

	if (!jenv_init(&e))
		goto out;
	CHECK_EQ(pw_file_open(e.dev, &f2), PW_OK);
	if (!f2)
		goto out;
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &j1));
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &d1));
	CHECK(ubo_create(e.os, f2, 4096, PANFROST_BO_NOEXEC, &j2));
	CHECK(ubo_create(e.os, f2, 4096, PANFROST_BO_NOEXEC, &d2));
	sa = so_create(e.file, false);
	sb = so_create(f2, false);
	sc = so_create(f2, false);

	mock_jm_inject(e.os, 1, MOCK_JM_INJ_STALL, 0, 1);
	a.jc = wv_chain(&j1, d1.va, 0xa);
	a.out = sa;
	CHECK_EQ(submit(e.file, &a), PW_OK);
	WAIT_FOR(js_stalled(&e, 1), 2000);
	CHECK(js_stalled(&e, 1));
	fa = so_fence(e.file, sa);
	CHECK(fa != NULL);
	CHECK_EQ(pw_syncobj_attach_fence(f2, sb, 0, fa), PW_OK);

	b.jc = wv_chain(&j2, d2.va, 0xb);
	b.reqs = PANFROST_JD_REQ_FS;
	b.ins = &sb;
	b.nins = 1;
	b.out = sc;
	CHECK_EQ(submit(f2, &b), PW_OK);
	usleep(20000);
	CHECK_EQ(js_stats(&e, 0).starts, 0);	/* still waiting: the ioctl did not block */
	CHECK_EQ(so_wait(e.os, f2, sc, 0), PW_ETIME);
	CHECK_EQ(get32(d2.cpu), 0);

	mock_jm_kick(e.os, 1);
	CHECK_EQ(so_wait(e.os, f2, sc, 2000), PW_OK);
	fb = so_fence(f2, sc);
	CHECK(fb && pw_fence_error(fb) == 0);
	CHECK(fa && fb && pw_fence_signal_time_ns(fa) <= pw_fence_signal_time_ns(fb));
	CHECK_EQ(get32(d1.cpu), 0xa);
	CHECK_EQ(get32(d2.cpu), 0xb);
	CHECK_EQ(js_stats(&e, 0).runs, 1);
out:
	pw_fence_put(fa);
	pw_fence_put(fb);
	if (f2)
		pw_file_close(f2);
	jenv_fini(&e);
}

/* Implicit sync: job B (JS0) waits for job A (JS1) because both use BO X; WAIT_BO on X. */
static void test_jm_implicit_sync(void)
{
	struct jenv e;
	struct ubo j, x;
	struct sub a = { 0 }, b = { 0 };
	uint32_t bos[2], sa, sb;

	if (!jenv_init(&e))
		goto out;
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &j));
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &x));
	bos[0] = j.handle;
	bos[1] = x.handle;
	sa = so_create(e.file, false);
	sb = so_create(e.file, false);

	mock_jm_inject(e.os, 1, MOCK_JM_INJ_STALL, 0, 1);
	a.jc = wv_chain(&j, x.va, 1);
	a.bos = bos;
	a.nbos = 2;
	a.out = sa;
	CHECK_EQ(submit(e.file, &a), PW_OK);
	WAIT_FOR(js_stalled(&e, 1), 2000);
	b = a;
	b.jc = wv_chain(&j, x.va, 2);
	b.reqs = PANFROST_JD_REQ_FS;
	b.out = sb;
	CHECK_EQ(submit(e.file, &b), PW_OK);
	usleep(20000);
	CHECK_EQ(js_stats(&e, 0).starts, 0);
	CHECK_EQ(wait_bo(e.os, e.file, x.handle, 0), PW_EBUSY);
	CHECK_EQ(wait_bo(e.os, e.file, x.handle, 5000000), PW_ETIMEDOUT);	/* 5 ms */

	mock_jm_kick(e.os, 1);
	CHECK_EQ(wait_bo(e.os, e.file, x.handle, INT64_MAX), PW_OK);
	CHECK_EQ(so_result(e.os, e.file, sa, 10), 0);
	CHECK_EQ(so_result(e.os, e.file, sb, 10), 0);
	CHECK_EQ(get32(x.cpu), 2);	/* B wrote last */
	CHECK_EQ(js_stats(&e, 0).runs, 1);
out:
	jenv_fini(&e);
}

/* Two dependent jobs on one slot are pipelined: B waits in NEXT behind A (drm_sched's
 * "scheduled" dependency on the same scheduler); a third one stays queued. The two chains
 * carry different JOB_CHAIN_FLAGs. */
static void test_jm_pipeline(void)
{
	struct jenv e;
	struct ubo j, x;
	struct sub s = { 0 };
	struct mock_js_stats st;
	uint32_t bos[2], out[3], cfg_a, cfg_b;
	uint64_t va[3];

	if (!jenv_init(&e))
		goto out;
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &j));
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &x));
	bos[0] = j.handle;
	bos[1] = x.handle;
	s.bos = bos;
	s.nbos = 2;

	mock_jm_inject(e.os, 1, MOCK_JM_INJ_STALL, 0, 1);
	for (unsigned int i = 0; i < 3; i++) {
		out[i] = so_create(e.file, false);
		va[i] = wv_chain(&j, x.va, 10 + i);
		s.jc = va[i];
		s.out = out[i];
		CHECK_EQ(submit(e.file, &s), PW_OK);
		if (!i)
			WAIT_FOR(js_stalled(&e, 1), 2000);
	}
	st = js_stats(&e, 1);
	CHECK(st.stalled);
	CHECK(st.next_valid);		/* B is in NEXT although it depends on A */
	CHECK_EQ(st.starts, 2);		/* C waits: two chains per slot */
	CHECK_EQ(st.last_head, va[0]);
	cfg_a = st.last_config;
	cfg_b = mock_reg(e.os, JS_CONFIG_NEXT(1));
	CHECK_EQ(mock_reg(e.os, JS_HEAD_NEXT(1)), (uint32_t)va[1]);
	CHECK((cfg_a ^ cfg_b) & JS_CONFIG_JOB_CHAIN_FLAG);

	mock_jm_kick(e.os, 1);
	for (unsigned int i = 0; i < 3; i++)
		CHECK_EQ(so_result(e.os, e.file, out[i], 2000), 0);
	CHECK_EQ(get32(x.cpu), 12);
	CHECK_EQ(js_stats(&e, 1).runs, 3);
	CHECK(sched_stats(&e).hw_submits >= 3);
out:
	jenv_fini(&e);
}

/* Faults: injected failure, invalid descriptor, unhandled page fault; later jobs still run,
 * also when the job behind a failing one waited in NEXT. */
static void test_jm_job_faults(void)
{
	struct jenv e;
	struct ubo j, d;
	struct sub s = { 0 };
	uint32_t out, o[3];

	if (!jenv_init(&e))
		goto out;
	CHECK(ubo_create(e.os, e.file, 8192, PANFROST_BO_NOEXEC, &j));
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &d));
	out = so_create(e.file, false);
	s.out = out;

	mock_jm_inject(e.os, 1, MOCK_JM_INJ_FAIL, PW_JM_EXC_JOB_BUS_FAULT, 1);
	s.jc = wv_chain(&j, d.va, 1);
	CHECK_EQ(submit(e.file, &s), PW_OK);
	CHECK_EQ(so_result(e.os, e.file, out, 2000), PW_EINVAL);
	CHECK_EQ(get32(d.cpu), 0);
	s.jc = wv_chain(&j, d.va, 2);
	CHECK_EQ(submit(e.file, &s), PW_OK);
	CHECK_EQ(so_result(e.os, e.file, out, 2000), 0);
	CHECK_EQ(get32(d.cpu), 2);

	/* type 0 descriptor: JOB_CONFIG_FAULT */
	s.jc = typed_chain(&j, 0);
	CHECK_EQ(submit(e.file, &s), PW_OK);
	CHECK_EQ(so_result(e.os, e.file, out, 2000), PW_EINVAL);

	/* write outside any BO: unhandled page fault, AS disabled, the next job re-enables it */
	mock_log_clear(e.os);
	s.jc = wv_chain(&j, 3ull << 30, 3);
	CHECK_EQ(submit(e.file, &s), PW_OK);
	CHECK_EQ(so_result(e.os, e.file, out, 2000), PW_EINVAL);
	CHECK(mock_log_contains(e.os, "Unhandled page fault"));
	s.jc = wv_chain(&j, d.va, 4);
	CHECK_EQ(submit(e.file, &s), PW_OK);
	CHECK_EQ(so_result(e.os, e.file, out, 2000), 0);
	CHECK_EQ(get32(d.cpu), 4);

	/* C runs (stalled), A waits in NEXT and fails once promoted, B behind it still runs */
	mock_jm_inject(e.os, 1, MOCK_JM_INJ_STALL, 0, 1);
	for (unsigned int i = 0; i < 3; i++) {
		o[i] = so_create(e.file, false);
		s.out = o[i];
		s.jc = wv_chain(&j, d.va + 4 * (i + 1), 100 + i);
		CHECK_EQ(submit(e.file, &s), PW_OK);
		if (!i) {
			WAIT_FOR(js_stalled(&e, 1), 2000);
			mock_jm_inject(e.os, 1, MOCK_JM_INJ_FAIL, PW_JM_EXC_JOB_READ_FAULT, 1);
		}
	}
	mock_jm_kick(e.os, 1);
	CHECK_EQ(so_result(e.os, e.file, o[0], 2000), 0);
	CHECK_EQ(so_result(e.os, e.file, o[1], 2000), PW_EINVAL);
	CHECK_EQ(so_result(e.os, e.file, o[2], 2000), 0);
	CHECK_EQ(get32(d.cpu + 4), 100);
	CHECK_EQ(get32(d.cpu + 8), 0);
	CHECK_EQ(get32(d.cpu + 12), 102);
out:
	jenv_fini(&e);
}

/*
 * Hang -> timeout -> reset. Phase 1: A (default context) hangs on JS1 with a 100 ms timeout;
 * B (another context) waits in NEXT; D runs stalled on JS0 with a long timeout. A fails with
 * -ECANCELED; B and D are innocent: resubmitted (D resumed from JS_TAIL) and complete. Phase
 * 2: the job of the guilty context waiting in NEXT is cancelled too. Later jobs still run.
 */
static void test_jm_timeout_reset(void)
{
	struct jenv e;
	struct ubo j, d;
	struct sub s = { 0 };
	struct pw_jm_sched_stats st;
	uint32_t sa, sb, sd, se, x, resets0;

	if (!jenv_init(&e))
		goto out;
	CHECK(ubo_create(e.os, e.file, 8192, PANFROST_BO_NOEXEC, &j));
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &d));
	x = ctx_create(e.file, PANFROST_JM_CTX_PRIORITY_MEDIUM);
	sa = so_create(e.file, false);
	sb = so_create(e.file, false);
	sd = so_create(e.file, false);
	se = so_create(e.file, false);
	resets0 = mock_soft_resets(e.os);
	mock_log_clear(e.os);

	pw_jm_sched_set_job_timeout(e.dev, 10000);
	mock_jm_inject(e.os, 0, MOCK_JM_INJ_STALL, 0, 1);
	s.jc = wv_chain(&j, d.va + 12, 0xd);
	s.reqs = PANFROST_JD_REQ_FS;
	s.out = sd;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	WAIT_FOR(js_stalled(&e, 0), 2000);

	pw_jm_sched_set_job_timeout(e.dev, 300);
	mock_jm_inject(e.os, 1, MOCK_JM_INJ_HANG, 0, 1);
	s.reqs = 0;
	s.jc = wv_chain(&j, d.va, 0xa);
	s.out = sa;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	WAIT_FOR(js_hung(&e, 1), 2000);
	pw_jm_sched_set_job_timeout(e.dev, 10000);
	s.jc = wv_chain(&j, d.va + 4, 0xb);
	s.out = sb;
	s.ctx = x;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	CHECK(js_stats(&e, 1).next_valid);

	CHECK_EQ(so_result(e.os, e.file, sa, 3000), PW_ECANCELED);
	CHECK_EQ(so_result(e.os, e.file, sb, 3000), 0);
	CHECK_EQ(so_result(e.os, e.file, sd, 3000), 0);
	CHECK_EQ(get32(d.cpu), 0);
	CHECK_EQ(get32(d.cpu + 4), 0xb);
	CHECK_EQ(get32(d.cpu + 12), 0xd);
	CHECK(mock_log_contains(e.os, "gpu sched timeout"));
	CHECK_EQ(mock_soft_resets(e.os), resets0 + 1);
	st = sched_stats(&e);
	CHECK_EQ(st.timeouts, 1);
	CHECK_EQ(st.resets, 1);
	CHECK_EQ(js_stats(&e, 0).terminated, 1);	/* D was stopped by the reset and resumed */

	/* phase 2: the guilty context's job in NEXT is cancelled, the other context's runs */
	pw_jm_sched_set_job_timeout(e.dev, 100);
	mock_jm_inject(e.os, 1, MOCK_JM_INJ_HANG, 0, 1);
	s.ctx = 0;
	s.jc = wv_chain(&j, d.va + 16, 0x1);
	s.out = sa;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	WAIT_FOR(js_hung(&e, 1), 2000);
	s.jc = wv_chain(&j, d.va + 20, 0x2);
	s.out = sb;
	CHECK_EQ(submit(e.file, &s), PW_OK);	/* same context, in NEXT */
	s.jc = wv_chain(&j, d.va + 24, 0x3);
	s.out = sd;
	s.ctx = x;
	CHECK_EQ(submit(e.file, &s), PW_OK);	/* queued */
	CHECK_EQ(so_result(e.os, e.file, sa, 3000), PW_ECANCELED);
	CHECK_EQ(so_result(e.os, e.file, sb, 3000), PW_ECANCELED);
	CHECK_EQ(so_result(e.os, e.file, sd, 3000), 0);
	CHECK_EQ(get32(d.cpu + 16), 0);
	CHECK_EQ(get32(d.cpu + 20), 0);
	CHECK_EQ(get32(d.cpu + 24), 0x3);
	CHECK_EQ(sched_stats(&e).resets, 2);

	/* later jobs run normally */
	pw_jm_sched_set_job_timeout(e.dev, PW_JM_JOB_TIMEOUT_MS);
	s.ctx = 0;
	s.jc = wv_chain(&j, d.va + 28, 0x77);
	s.out = se;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	CHECK_EQ(so_result(e.os, e.file, se, 2000), 0);
	CHECK_EQ(get32(d.cpu + 28), 0x77);
out:
	jenv_fini(&e);
}

/* JM_CTX_DESTROY with jobs on the GPU and queued behind a dependency. */
static void test_jm_ctx_destroy_inflight(void)
{
	struct jenv e;
	struct ubo j, d;
	struct sub s = { 0 };
	struct pw_fence *fa = NULL, *fw = NULL;
	struct mock_js_stats st0, st1;
	uint32_t x, y, z, sa, sb, sc, sw;

	if (!jenv_init(&e))
		goto out;
	CHECK(ubo_create(e.os, e.file, 8192, PANFROST_BO_NOEXEC, &j));
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &d));
	sa = so_create(e.file, false);
	sb = so_create(e.file, false);
	sc = so_create(e.file, false);
	sw = so_create(e.file, false);

	/* 1: both of X's jobs on JS1 (running + NEXT) */
	x = ctx_create(e.file, PANFROST_JM_CTX_PRIORITY_MEDIUM);
	mock_jm_inject(e.os, 1, MOCK_JM_INJ_STALL, 0, 1);
	s.ctx = x;
	s.jc = wv_chain(&j, d.va, 1);
	s.out = sa;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	WAIT_FOR(js_stalled(&e, 1), 2000);
	s.jc = wv_chain(&j, d.va + 4, 2);
	s.out = sb;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	st0 = js_stats(&e, 1);
	CHECK_EQ(ctx_destroy(e.file, x), PW_OK);	/* returns at once */
	CHECK_EQ(so_result(e.os, e.file, sa, 2000), PW_ECANCELED);
	CHECK_EQ(so_result(e.os, e.file, sb, 2000), PW_ECANCELED);
	st1 = js_stats(&e, 1);
	CHECK_EQ(st1.terminated, st0.terminated + 1);
	CHECK_EQ(st1.runs, st0.runs);			/* B never started */
	CHECK_EQ(get32(d.cpu), 0);
	CHECK_EQ(get32(d.cpu + 4), 0);
	s.ctx = 0;
	s.jc = wv_chain(&j, d.va + 8, 3);
	s.out = sc;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	CHECK_EQ(so_result(e.os, e.file, sc, 2000), 0);

	/* 2: another context's chain runs, Y's job waits in NEXT: only Y's is stopped
	 * (JOB_CHAIN_FLAG-selected hard stop) */
	y = ctx_create(e.file, PANFROST_JM_CTX_PRIORITY_MEDIUM);
	mock_jm_inject(e.os, 1, MOCK_JM_INJ_STALL, 0, 1);
	s.ctx = 0;
	s.jc = wv_chain(&j, d.va + 12, 4);
	s.out = sa;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	WAIT_FOR(js_stalled(&e, 1), 2000);
	s.ctx = y;
	s.jc = wv_chain(&j, d.va + 16, 5);
	s.out = sb;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	st0 = js_stats(&e, 1);
	CHECK(st0.next_valid);
	CHECK_EQ(ctx_destroy(e.file, y), PW_OK);
	st1 = js_stats(&e, 1);
	CHECK(st1.stalled);				/* the running chain is untouched */
	CHECK(!st1.next_valid);
	CHECK_EQ(st1.next_cancels, st0.next_cancels + 1);
	CHECK_EQ(st1.terminated, st0.terminated);
	CHECK_EQ(so_wait(e.os, e.file, sb, 0), PW_ETIME);	/* completes with the slot */
	mock_jm_kick(e.os, 1);
	CHECK_EQ(so_result(e.os, e.file, sa, 2000), 0);
	CHECK_EQ(so_result(e.os, e.file, sb, 2000), PW_ECANCELED);
	CHECK_EQ(js_stats(&e, 1).runs, st0.runs);
	CHECK_EQ(get32(d.cpu + 12), 4);
	CHECK_EQ(get32(d.cpu + 16), 0);

	/* 3: a job of Z waits for a running job of the default context; destroying Z cancels
	 * it, but only once its dependency signalled (one fence per BO stays ordered) */
	z = ctx_create(e.file, PANFROST_JM_CTX_PRIORITY_LOW);
	mock_jm_inject(e.os, 1, MOCK_JM_INJ_STALL, 0, 1);
	s.ctx = 0;
	s.jc = wv_chain(&j, d.va + 20, 6);
	s.out = sa;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	WAIT_FOR(js_stalled(&e, 1), 2000);
	fa = so_fence(e.file, sa);
	s.ctx = z;
	s.reqs = PANFROST_JD_REQ_FS;
	s.ins = &sa;
	s.nins = 1;
	s.jc = wv_chain(&j, d.va + 24, 7);
	s.out = sw;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	fw = so_fence(e.file, sw);
	CHECK_EQ(ctx_destroy(e.file, z), PW_OK);
	usleep(20000);
	CHECK(fw && !pw_fence_is_signalled(fw));
	mock_jm_kick(e.os, 1);
	CHECK(fa && fence_result(e.os, fa, 2000) == 0);
	CHECK(fw && fence_result(e.os, fw, 2000) == PW_ECANCELED);
	CHECK(fa && fw && pw_fence_signal_time_ns(fw) >= pw_fence_signal_time_ns(fa));
	CHECK_EQ(js_stats(&e, 0).runs, 0);
	CHECK_EQ(get32(d.cpu + 24), 0);
out:
	pw_fence_put(fa);
	pw_fence_put(fw);
	jenv_fini(&e);
}

/*
 * A destroyed context's job whose dependency is ahead of it on the same slot (drm_sched's
 * "scheduled" shortcut let it into the slot early) is cancelled only after that dependency
 * signalled: 1) it waits in NEXT; 2) with a single AS, it waits in the requeue list because
 * the AS is busy with the other file's job.
 */
static void test_jm_cancel_order(void)
{
	struct mock_config cfg = { 0 };
	struct jenv e;
	struct pw_file *f2 = NULL;
	struct ubo j1, d1, j2, d2;
	struct sub s = { 0 };
	struct pw_fence *fa = NULL, *fw = NULL;
	uint32_t sa, sw, imp, z;
	struct pw_jm_sched_stats st0;

	cfg.as_present = 0x1;
	if (!jenv_init_cfg(&e, &cfg))
		goto out;
	CHECK_EQ(pw_file_open(e.dev, &f2), PW_OK);
	if (!f2)
		goto out;
	CHECK(ubo_create(e.os, e.file, 8192, PANFROST_BO_NOEXEC, &j1));
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &d1));
	CHECK(ubo_create(e.os, f2, 4096, PANFROST_BO_NOEXEC, &j2));
	CHECK(ubo_create(e.os, f2, 4096, PANFROST_BO_NOEXEC, &d2));
	sa = so_create(e.file, false);
	sw = so_create(e.file, false);

	/* 1: same file, W (context Z) in NEXT behind A, which it depends on */
	z = ctx_create(e.file, PANFROST_JM_CTX_PRIORITY_MEDIUM);
	mock_jm_inject(e.os, 1, MOCK_JM_INJ_STALL, 0, 1);
	s.jc = wv_chain(&j1, d1.va, 1);
	s.out = sa;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	WAIT_FOR(js_stalled(&e, 1), 2000);
	fa = so_fence(e.file, sa);
	s.jc = wv_chain(&j1, d1.va + 4, 2);
	s.ins = &sa;
	s.nins = 1;
	s.out = sw;
	s.ctx = z;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	fw = so_fence(e.file, sw);
	CHECK(js_stats(&e, 1).next_valid);	/* the dependency is ahead of it on JS1 */
	CHECK_EQ(ctx_destroy(e.file, z), PW_OK);
	usleep(20000);
	CHECK(fw && !pw_fence_is_signalled(fw));
	mock_jm_kick(e.os, 1);
	CHECK(fa && fence_result(e.os, fa, 2000) == 0);
	CHECK(fw && fence_result(e.os, fw, 2000) == PW_ECANCELED);
	CHECK(fa && fw && pw_fence_signal_time_ns(fw) >= pw_fence_signal_time_ns(fa));
	CHECK_EQ(get32(d1.cpu + 4), 0);
	pw_fence_put(fa);
	pw_fence_put(fw);
	fa = fw = NULL;

	/* 2: file 2's job depends on file 1's running job on JS1, but the only AS is busy */
	imp = so_create(f2, false);
	z = ctx_create(f2, PANFROST_JM_CTX_PRIORITY_MEDIUM);
	mock_jm_inject(e.os, 1, MOCK_JM_INJ_STALL, 0, 1);
	memset(&s, 0, sizeof(s));
	s.jc = wv_chain(&j1, d1.va + 8, 3);
	s.out = sa;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	WAIT_FOR(js_stalled(&e, 1), 2000);
	fa = so_fence(e.file, sa);
	CHECK_EQ(pw_syncobj_attach_fence(f2, imp, 0, fa), PW_OK);
	st0 = sched_stats(&e);
	s.jc = wv_chain(&j2, d2.va, 4);
	s.ins = &imp;
	s.nins = 1;
	s.out = imp;
	s.ctx = z;
	CHECK_EQ(submit(f2, &s), PW_OK);
	fw = so_fence(f2, imp);
	CHECK(sched_stats(&e).as_busy > st0.as_busy);	/* picked, then no AS */
	CHECK_EQ(ctx_destroy(f2, z), PW_OK);
	usleep(20000);
	CHECK(fw && !pw_fence_is_signalled(fw));
	mock_jm_kick(e.os, 1);
	CHECK(fa && fence_result(e.os, fa, 2000) == 0);
	CHECK(fw && fence_result(e.os, fw, 2000) == PW_ECANCELED);
	CHECK(fa && fw && pw_fence_signal_time_ns(fw) >= pw_fence_signal_time_ns(fa));
	CHECK_EQ(get32(d2.cpu), 0);
	CHECK_EQ(get32(d1.cpu + 8), 3);

	/* the AS is shared fine afterwards */
	memset(&s, 0, sizeof(s));
	s.jc = wv_chain(&j2, d2.va + 4, 5);
	s.out = imp;
	CHECK_EQ(submit(f2, &s), PW_OK);
	CHECK_EQ(so_result(e.os, f2, imp, 2000), 0);
	CHECK_EQ(get32(d2.cpu + 4), 5);
out:
	pw_fence_put(fa);
	pw_fence_put(fw);
	if (f2)
		pw_file_close(f2);
	jenv_fini(&e);
}

/* File close with a running job, a job in NEXT and a job waiting on another file's job. */
static void test_jm_file_close_inflight(void)
{
	struct jenv e;
	struct pw_file *f2 = NULL;
	struct ubo j1, d1, j2, d2;
	struct sub s = { 0 };
	struct pw_fence *fp = NULL, *fa = NULL, *fb = NULL, *fc = NULL;
	uint32_t sp, sa, sb, sc, imp;
	long allocs;

	if (!jenv_init(&e))
		goto out;
	CHECK_EQ(pw_file_open(e.dev, &f2), PW_OK);
	if (!f2)
		goto out;
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &j1));
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &d1));
	CHECK(ubo_create(e.os, f2, 4096, PANFROST_BO_NOEXEC, &j2));
	CHECK(ubo_create(e.os, f2, 4096, PANFROST_BO_NOEXEC, &d2));
	sp = so_create(e.file, false);
	sa = so_create(f2, false);
	sb = so_create(f2, false);
	sc = so_create(f2, false);
	imp = so_create(f2, false);
	allocs = mock_live_allocs(e.os);

	/* P: file 1, stalled on JS0 */
	mock_jm_inject(e.os, 0, MOCK_JM_INJ_STALL, 0, 1);
	s.reqs = PANFROST_JD_REQ_FS;
	s.jc = wv_chain(&j1, d1.va, 0x50);
	s.out = sp;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	WAIT_FOR(js_stalled(&e, 0), 2000);
	fp = so_fence(e.file, sp);
	CHECK_EQ(pw_syncobj_attach_fence(f2, imp, 0, fp), PW_OK);

	/* file 2: A stalled on JS1, B in NEXT, C (JS0) waiting for P */
	mock_jm_inject(e.os, 1, MOCK_JM_INJ_STALL, 0, 1);
	s.reqs = 0;
	s.jc = wv_chain(&j2, d2.va, 0xa);
	s.out = sa;
	CHECK_EQ(submit(f2, &s), PW_OK);
	WAIT_FOR(js_stalled(&e, 1), 2000);
	s.jc = wv_chain(&j2, d2.va + 4, 0xb);
	s.out = sb;
	CHECK_EQ(submit(f2, &s), PW_OK);
	s.reqs = PANFROST_JD_REQ_FS;
	s.jc = wv_chain(&j2, d2.va + 8, 0xc);
	s.ins = &imp;
	s.nins = 1;
	s.out = sc;
	CHECK_EQ(submit(f2, &s), PW_OK);
	fa = so_fence(f2, sa);
	fb = so_fence(f2, sb);
	fc = so_fence(f2, sc);

	pw_file_close(f2);
	f2 = NULL;
	/* nothing of file 2 is on the GPU any more. (review) The bottom half that took them off
	 * the slot signals their fences right after it drops the scheduler lock, which may be
	 * just after the close returned: wait rather than poll. */
	CHECK(!js_stats(&e, 1).running);
	CHECK(fa && fence_result(e.os, fa, 2000) == PW_ECANCELED);
	CHECK(fb && fence_result(e.os, fb, 2000) == PW_ECANCELED);
	CHECK(fc && !pw_fence_is_signalled(fc));
	CHECK(!js_stats(&e, 1).running);
	CHECK(js_stalled(&e, 0));

	mock_jm_kick(e.os, 0);
	CHECK(fp && fence_result(e.os, fp, 2000) == 0);
	CHECK(fc && fence_result(e.os, fc, 2000) == PW_ECANCELED);
	CHECK_EQ(js_stats(&e, 0).runs, 1);		/* C never ran */
	CHECK_EQ(get32(d1.cpu), 0x50);	/* (d2 went away with file 2) */
	CHECK(sched_idle(&e));
	pw_fence_put(fa);
	pw_fence_put(fb);
	pw_fence_put(fc);
	fa = fb = fc = NULL;
	/* file 2's memory is all gone (only file 1's syncobj fence holder remains) */
	CHECK(mock_live_allocs(e.os) < allocs);
out:
	pw_fence_put(fp);
	pw_fence_put(fa);
	pw_fence_put(fb);
	pw_fence_put(fc);
	if (f2)
		pw_file_close(f2);
	jenv_fini(&e);
}

/* HEAP growth on a GPU fault while a submitted job runs. */
static void test_jm_submit_heap(void)
{
	struct jenv e;
	struct ubo j, h;
	struct sub s = { 0 };
	struct mock_js_stats s0, s1;
	uint32_t bos[2], out, v = 0;
	uint8_t *p;
	struct pw_vm *vm;

	if (!jenv_init(&e))
		goto out;
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &j));
	CHECK(ubo_create(e.os, e.file, 4u << 20, PANFROST_BO_HEAP | PANFROST_BO_NOEXEC, &h));
	s.jc = job_slot(&j, &p);
	job_hdr(p, JT_WRITE_VALUE, 1, j.va + 128);
	put64(p + 32, h.va + (2u << 20) + 0x40);
	put32(p + 40, WV_IMM32);
	put64(p + 48, 0x600d);
	job_slot(&j, &p);
	job_hdr(p, JT_WRITE_VALUE, 2, 0);
	put64(p + 32, h.va + 0x80);
	put32(p + 40, WV_IMM32);
	put64(p + 48, 0x700d);
	bos[0] = j.handle;
	bos[1] = h.handle;
	s.bos = bos;
	s.nbos = 2;
	out = so_create(e.file, false);
	s.out = out;
	s0 = js_stats(&e, 1);
	CHECK_EQ(submit(e.file, &s), PW_OK);
	CHECK_EQ(so_result(e.os, e.file, out, 3000), 0);
	s1 = js_stats(&e, 1);
	CHECK_EQ(s1.page_faults, s0.page_faults + 2);
	CHECK_EQ(s1.fault_retries, s0.fault_retries + 2);
	vm = pw_jm_vm_mmu(pw_jm_file_vm(e.file));
	CHECK(vm && vm->as >= 0);	/* the slot stays bound for reuse */
	if (vm && vm->as >= 0) {
		CHECK_EQ(mock_gpu_read(e.os, (unsigned int)vm->as, h.va + (2u << 20) + 0x40, &v, 4),
			 0);
		CHECK_EQ(v, 0x600d);
		CHECK_EQ(mock_gpu_read(e.os, (unsigned int)vm->as, h.va + 0x80, &v, 4), 0);
		CHECK_EQ(v, 0x700d);
	}
	CHECK(!mock_log_contains(e.os, "Unhandled"));
out:
	jenv_fini(&e);
}

/* Priorities: with the slot full, a MEDIUM job queued after a LOW one runs first. */
static void test_jm_priority(void)
{
	struct jenv e;
	struct ubo j, d;
	struct sub s = { 0 };
	struct mock_jm_job_rec rec[64];
	uint32_t lo, out[4];
	uint64_t va_lo = 0, va_med = 0;
	unsigned int n, pos_lo = 99, pos_med = 99;

	if (!jenv_init(&e))
		goto out;
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &j));
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &d));
	lo = ctx_create(e.file, PANFROST_JM_CTX_PRIORITY_LOW);
	for (unsigned int i = 0; i < 4; i++)
		out[i] = so_create(e.file, false);
	mock_jm_job_log_clear(e.os, 1);
	mock_jm_inject(e.os, 1, MOCK_JM_INJ_STALL, 0, 1);
	for (unsigned int i = 0; i < 4; i++) {
		s.jc = wv_chain(&j, d.va + 4 * i, i + 1);
		s.out = out[i];
		s.ctx = i == 2 ? lo : 0;
		if (i == 2)
			va_lo = s.jc;
		if (i == 3)
			va_med = s.jc;
		CHECK_EQ(submit(e.file, &s), PW_OK);
		if (!i)
			WAIT_FOR(js_stalled(&e, 1), 2000);
	}
	mock_jm_kick(e.os, 1);
	for (unsigned int i = 0; i < 4; i++)
		CHECK_EQ(so_result(e.os, e.file, out[i], 2000), 0);
	n = mock_jm_job_log(e.os, 1, rec, 64);
	CHECK_EQ(n, 4);
	for (unsigned int i = 0; i < n; i++) {
		if (rec[i].va == va_lo)
			pos_lo = i;
		if (rec[i].va == va_med)
			pos_med = i;
	}
	CHECK(pos_med < pos_lo && pos_lo < 99);
out:
	jenv_fini(&e);
}

/* Many submits from one file: every job writes its index, implicit sync orders them all. */
static void test_jm_many_submits(void)
{
	enum { N = 400 };
	struct jenv e;
	struct ubo j, d;
	struct sub s = { 0 };
	uint32_t bos[2], out;
	struct pw_jm_sched_stats st;

	if (!jenv_init(&e))
		goto out;
	CHECK(ubo_create(e.os, e.file, N * 128, PANFROST_BO_NOEXEC, &j));
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &d));
	bos[0] = j.handle;
	bos[1] = d.handle;
	out = so_create(e.file, false);
	s.bos = bos;
	s.nbos = 2;
	s.out = out;
	for (uint32_t i = 0; i < N; i++) {
		s.jc = wv_chain(&j, d.va + 4 * (i % 1024), i + 1);
		s.reqs = (i % 3 == 0) ? PANFROST_JD_REQ_FS : 0;
		CHECK_EQ(submit(e.file, &s), PW_OK);
	}
	CHECK_EQ(wait_bo(e.os, e.file, d.handle, INT64_MAX), PW_OK);
	CHECK_EQ(so_result(e.os, e.file, out, 5000), 0);
	for (uint32_t i = 0; i < N; i++) {
		if (get32(d.cpu + 4 * i) != i + 1) {
			CHECK_EQ(get32(d.cpu + 4 * i), i + 1);
			break;
		}
	}
	CHECK(sched_idle(&e));
	st = sched_stats(&e);
	CHECK_EQ(st.submitted, N);
	CHECK_EQ(st.completed, N);
	CHECK_EQ(js_stats(&e, 0).runs + js_stats(&e, 1).runs, N);
out:
	jenv_fini(&e);
}

/* ---- concurrency ---- */

struct stress_thr {
	struct jenv *e;
	struct pw_file *f;
	pthread_t thr;
	unsigned int id, n;
	uint32_t seed;
	struct ubo j, d;
	uint32_t out, ctx;
	int errors;
};

static uint32_t rnd(uint32_t *s)
{
	*s = *s * 1103515245u + 12345u;
	return *s >> 8;
}

/* Submits @n jobs writing 1..n into its own data BO, randomly on both slots and its two
 * contexts, sometimes waiting for its previous job through the out_sync. */
static void *stress_fn(void *arg)
{
	struct stress_thr *t = arg;
	uint32_t bos[2] = { t->j.handle, t->d.handle };
	struct sub s = { 0 };

	s.bos = bos;
	s.nbos = 2;
	s.out = t->out;
	for (unsigned int i = 0; i < t->n; i++) {
		uint32_t r = rnd(&t->seed);

		s.jc = wv_chain(&t->j, t->d.va + 4 * i, i + 1);
		s.reqs = (r & 1) ? PANFROST_JD_REQ_FS : 0;
		s.ctx = (r & 2) ? t->ctx : 0;
		s.ins = (r & 4) && i ? &t->out : NULL;
		s.nins = s.ins ? 1 : 0;
		if (submit(t->f, &s))
			t->errors++;
		if (!(r & 0x70))
			usleep(r % 500);
	}
	return NULL;
}

/* 4 files x 2 threads, 2 AS slots for 4 address spaces (LRU eviction, AS waits). */
static void test_jm_stress_threads(void)
{
	enum { NFILES = 4, PER = 2, NJOBS = 130 };
	struct mock_config cfg = { 0 };
	struct jenv e;
	struct pw_file *files[NFILES] = { 0 };
	struct stress_thr t[NFILES * PER];
	struct pw_jm_sched_stats st;

	cfg.as_present = 0x3;
	if (!jenv_init_cfg(&e, &cfg))
		goto out;
	files[0] = e.file;
	for (unsigned int i = 1; i < NFILES; i++)
		CHECK_EQ(pw_file_open(e.dev, &files[i]), PW_OK);
	memset(t, 0, sizeof(t));
	for (unsigned int i = 0; i < NFILES * PER; i++) {
		struct stress_thr *x = &t[i];

		x->e = &e;
		x->f = files[i / PER];
		x->id = i;
		x->n = NJOBS;
		x->seed = 0x1234567u * (i + 1);
		CHECK(ubo_create(e.os, x->f, NJOBS * 128, PANFROST_BO_NOEXEC, &x->j));
		CHECK(ubo_create(e.os, x->f, 4096, PANFROST_BO_NOEXEC, &x->d));
		x->out = so_create(x->f, false);
		x->ctx = ctx_create(x->f, i & 1 ? PANFROST_JM_CTX_PRIORITY_LOW :
					     PANFROST_JM_CTX_PRIORITY_MEDIUM);
	}
	for (unsigned int i = 0; i < NFILES * PER; i++)
		CHECK_EQ(pthread_create(&t[i].thr, NULL, stress_fn, &t[i]), 0);
	for (unsigned int i = 0; i < NFILES * PER; i++)
		pthread_join(t[i].thr, NULL);
	for (unsigned int i = 0; i < NFILES * PER; i++) {
		struct stress_thr *x = &t[i];

		CHECK_EQ(x->errors, 0);
		CHECK_EQ(wait_bo(e.os, x->f, x->d.handle, INT64_MAX), PW_OK);
		CHECK_EQ(so_result(e.os, x->f, x->out, 5000), 0);
		for (unsigned int k = 0; k < x->n; k++) {
			if (get32(x->d.cpu + 4 * k) != k + 1) {
				fprintf(stderr, "    thread %u job %u\n", i, k);
				CHECK_EQ(get32(x->d.cpu + 4 * k), k + 1);
				break;
			}
		}
	}
	CHECK(sched_idle(&e));
	st = sched_stats(&e);
	CHECK_EQ(st.submitted, NFILES * PER * NJOBS);
	CHECK_EQ(st.completed, st.submitted);
	CHECK_EQ(st.resets, 0);
	printf("    %llu jobs, %llu hw submits, %u AS waits\n", (unsigned long long)st.submitted,
	       (unsigned long long)st.hw_submits, st.as_busy);
out:
	for (unsigned int i = 1; i < NFILES; i++) {
		if (files[i])
			pw_file_close(files[i]);
	}
	jenv_fini(&e);
}

/* ---- teardown races ---- */

struct race_ctx {
	struct jenv *e;
	struct pw_file *f;
	volatile bool stop;
	unsigned int submits, cancelled;
};

/* Creates and destroys contexts while submitting on them. */
static void *ctx_churn_fn(void *arg)
{
	struct race_ctx *r = arg;
	struct ubo j, d;
	struct sub s = { 0 };
	uint32_t out, ctx;

	if (!ubo_create(r->e->os, r->f, 512 * 128, PANFROST_BO_NOEXEC, &j) ||
	    !ubo_create(r->e->os, r->f, 4096, PANFROST_BO_NOEXEC, &d))
		return NULL;
	out = so_create(r->f, false);
	s.out = out;
	for (unsigned int i = 0; i < 120 && !r->stop; i++) {
		ctx = ctx_create(r->f, PANFROST_JM_CTX_PRIORITY_MEDIUM);
		s.ctx = ctx;
		for (unsigned int k = 0; k < 4; k++) {
			s.jc = wv_chain(&j, d.va + 4 * k, i);
			s.reqs = k & 1 ? PANFROST_JD_REQ_FS : 0;
			if (submit(r->f, &s) == PW_OK)
				r->submits++;
		}
		CHECK_EQ(ctx_destroy(r->f, ctx), PW_OK);
		CHECK_EQ(submit(r->f, &s), PW_EINVAL);	/* the handle is gone */
		/* the last job is done or cancelled before its descriptors are reused */
		if (so_result(r->e->os, r->f, out, 3000) == PW_ECANCELED)
			r->cancelled++;
		WAIT_FOR(wait_bo(r->e->os, r->f, j.handle, 0) == PW_OK, 3000);
	}
	return NULL;
}

/* Opens a file, submits a few jobs on both slots, closes it while they run. */
static void *file_churn_fn(void *arg)
{
	struct race_ctx *r = arg;

	for (unsigned int i = 0; i < 40 && !r->stop; i++) {
		struct pw_file *f = NULL;
		struct ubo j, d;
		struct sub s = { 0 };
		uint32_t bos[2];

		if (pw_file_open(r->e->dev, &f) || !f) {
			CHECK(!"file open");
			break;
		}
		if (ubo_create(r->e->os, f, 4096, PANFROST_BO_NOEXEC, &j) &&
		    ubo_create(r->e->os, f, 4096, PANFROST_BO_NOEXEC, &d)) {
			bos[0] = j.handle;
			bos[1] = d.handle;
			s.bos = bos;
			s.nbos = 2;
			for (unsigned int k = 0; k < 6; k++) {
				s.jc = wv_chain(&j, d.va + 4 * k, k);
				s.reqs = k & 1 ? PANFROST_JD_REQ_FS : 0;
				CHECK_EQ(submit(f, &s), PW_OK);
				r->submits++;
			}
		}
		if (i & 1)
			usleep(200);
		pw_file_close(f);
	}
	return NULL;
}

static void test_jm_teardown_races(void)
{
	struct jenv e;
	struct race_ctx rc[2], rf[2];
	pthread_t th[4];
	struct pw_file *fa = NULL, *fb = NULL;
	struct pw_jm_sched_stats st;

	if (!jenv_init(&e))
		goto out;
	CHECK_EQ(pw_file_open(e.dev, &fa), PW_OK);
	CHECK_EQ(pw_file_open(e.dev, &fb), PW_OK);
	memset(rc, 0, sizeof(rc));
	memset(rf, 0, sizeof(rf));
	rc[0].e = rc[1].e = rf[0].e = rf[1].e = &e;
	rc[0].f = fa;
	rc[1].f = fb;
	CHECK_EQ(pthread_create(&th[0], NULL, ctx_churn_fn, &rc[0]), 0);
	CHECK_EQ(pthread_create(&th[1], NULL, ctx_churn_fn, &rc[1]), 0);
	CHECK_EQ(pthread_create(&th[2], NULL, file_churn_fn, &rf[0]), 0);
	CHECK_EQ(pthread_create(&th[3], NULL, file_churn_fn, &rf[1]), 0);
	for (unsigned int i = 0; i < 4; i++)
		pthread_join(th[i], NULL);
	printf("    ctx churn: %u + %u submits (%u + %u last jobs cancelled), file churn: %u + %u\n",
	       rc[0].submits, rc[1].submits, rc[0].cancelled, rc[1].cancelled, rf[0].submits,
	       rf[1].submits);
	pw_file_close(fa);
	pw_file_close(fb);
	fa = fb = NULL;
	CHECK(sched_idle(&e));
	st = sched_stats(&e);
	CHECK_EQ(st.completed, st.submitted);
	CHECK_EQ(st.resets, 0);
out:
	if (fa)
		pw_file_close(fa);
	if (fb)
		pw_file_close(fb);
	jenv_fini(&e);
}

/* Device teardown with a job still queued behind an unsignalled dependency of a closed file is
 * impossible (jobs always end); but a hung job at file close: the hard stop ends it. */
static void test_jm_close_hung(void)
{
	struct jenv e;
	struct ubo j, d;
	struct sub s = { 0 };
	struct pw_fence *fe = NULL;
	uint32_t out;

	if (!jenv_init(&e))
		goto out;
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &j));
	CHECK(ubo_create(e.os, e.file, 4096, PANFROST_BO_NOEXEC, &d));
	out = so_create(e.file, false);
	mock_jm_inject(e.os, 1, MOCK_JM_INJ_HANG, 0, 1);
	s.jc = wv_chain(&j, d.va, 1);
	s.out = out;
	CHECK_EQ(submit(e.file, &s), PW_OK);
	WAIT_FOR(js_hung(&e, 1), 2000);
	fe = so_fence(e.file, out);
	pw_file_close(e.file);
	e.file = NULL;
	CHECK(!js_stats(&e, 1).running);
	CHECK(fe && fence_result(e.os, fe, 2000) == PW_ECANCELED);	/* (review) see above */
	CHECK(sched_idle(&e));
	CHECK_EQ(sched_stats(&e).resets, 0);
out:
	pw_fence_put(fe);
	jenv_fini(&e);
}

/* (review) File close also waits for a job of a context that JM_CTX_DESTROY killed earlier
 * and that is still the current job of its slot (pw_jm_sched_file_fini contract): the job IRQ
 * is held back, so its TERMINATED event is pending when the file closes. */
struct close_arg {
	struct pw_file *f;
	int done;
};

static void *close_fn(void *arg)
{
	struct close_arg *a = arg;

	pw_file_close(a->f);
	__atomic_store_n(&a->done, 1, __ATOMIC_RELEASE);
	return NULL;
}

static void test_jm_close_after_ctx_destroy(void)
{
	struct jenv e;
	struct pw_file *f2 = NULL;
	struct ubo j, d;
	struct sub s = { 0 };
	struct pw_fence *fe = NULL;
	struct close_arg ca = { 0 };
	pthread_t thr;
	bool started = false;
	uint32_t x, out;

	if (!jenv_init(&e))
		goto out;
	CHECK_EQ(pw_file_open(e.dev, &f2), PW_OK);
	if (!f2)
		goto out;
	CHECK(ubo_create(e.os, f2, 4096, PANFROST_BO_NOEXEC, &j));
	CHECK(ubo_create(e.os, f2, 4096, PANFROST_BO_NOEXEC, &d));
	out = so_create(f2, false);
	x = ctx_create(f2, PANFROST_JM_CTX_PRIORITY_MEDIUM);
	mock_jm_inject(e.os, 1, MOCK_JM_INJ_STALL, 0, 1);
	s.ctx = x;
	s.jc = wv_chain(&j, d.va, 1);
	s.out = out;
	CHECK_EQ(submit(f2, &s), PW_OK);
	WAIT_FOR(js_stalled(&e, 1), 2000);
	fe = so_fence(f2, out);

	mock_detach_irq(e.os);		/* the hard stop's event stays pending */
	CHECK_EQ(ctx_destroy(f2, x), PW_OK);
	CHECK(!js_stats(&e, 1).running);
	CHECK(fe && !pw_fence_is_signalled(fe));
	ca.f = f2;
	f2 = NULL;
	started = pthread_create(&thr, NULL, close_fn, &ca) == 0;
	CHECK(started);
	usleep(150000);
	/* the scheduler has not seen the job end: the close must still be waiting */
	CHECK_EQ(__atomic_load_n(&ca.done, __ATOMIC_ACQUIRE), 0);
	mock_attach_irq(e.os, e.dev);
	if (started)
		pthread_join(thr, NULL);
	CHECK_EQ(ca.done, 1);
	/* the close returned after the scheduler took the job off the slot (the fence is
	 * signalled just after that, outside the scheduler lock) */
	CHECK(fe && fence_result(e.os, fe, 2000) == PW_ECANCELED);
	CHECK(sched_idle(&e));
out:
	pw_fence_put(fe);
	if (f2)
		pw_file_close(f2);
	jenv_fini(&e);
}

const struct test_case jm_sched_tests[] = {
	{ "jm_submit_validation", test_jm_submit_validation },
	{ "jm_ctx", test_jm_ctx },
	{ "jm_submit_write_value", test_jm_submit_write_value },
	{ "jm_slot_select", test_jm_slot_select },
	{ "jm_in_sync_cross_file", test_jm_in_sync_cross_file },
	{ "jm_implicit_sync", test_jm_implicit_sync },
	{ "jm_pipeline", test_jm_pipeline },
	{ "jm_job_faults", test_jm_job_faults },
	{ "jm_timeout_reset", test_jm_timeout_reset },
	{ "jm_ctx_destroy_inflight", test_jm_ctx_destroy_inflight },
	{ "jm_cancel_order", test_jm_cancel_order },
	{ "jm_file_close_inflight", test_jm_file_close_inflight },
	{ "jm_close_hung", test_jm_close_hung },
	{ "jm_submit_heap", test_jm_submit_heap },
	{ "jm_priority", test_jm_priority },
	{ "jm_many_submits", test_jm_many_submits },
	{ "jm_stress_threads", test_jm_stress_threads },
	{ "jm_teardown_races", test_jm_teardown_races },
	{ "jm_close_after_ctx_destroy", test_jm_close_after_ctx_destroy },
	{ NULL, NULL },
};
