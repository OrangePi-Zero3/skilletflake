// SPDX-License-Identifier: MIT
/*
 * test_sched.c - groups, GROUP_SUBMIT, job completion/faults/timeouts and tiler heaps, driven
 * through pw_file_ioctl with real uAPI structs against the mock G610 whose MCU executes the
 * command streams (mock_csf.c). The CS encodings below are written from Mesa's genxml v10.xml
 * (MIT), independently of the core's.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mock_gpu.h"
#include "pw_bo.h"
#include "pw_sched.h"
#include "pw_sync.h"
#include "test.h"

struct senv {
	struct pw_os *os;
	struct pw_device *dev;
	struct pw_file *file;
};

static bool senv_init(struct senv *e)
{
	struct mock_config cfg = { 0 };
	struct pw_device_params p = { 0 };

	memset(e, 0, sizeof(*e));
	if (!g_fw_path) {
		test_skip("real firmware not available");
		return false;
	}
	cfg.fw_path = g_fw_path;
	e->os = mock_create(&cfg);
	p.os = e->os;
	p.boot_firmware = true;
	if (pw_device_alloc(&p, &e->dev)) {
		CHECK(!"device alloc failed");
		return false;
	}
	mock_attach_irq(e->os, e->dev);
	if (pw_device_probe(e->dev) || pw_file_open(e->dev, &e->file)) {
		CHECK(!"bring-up / file open failed");
		return false;
	}
	return true;
}

static void senv_fini(struct senv *e)
{
	if (e->file)
		pw_file_close(e->file);
	e->file = NULL;
	mock_detach_irq(e->os);
	pw_device_destroy(e->dev);
	check_no_leaks(e->os);
	CHECK_EQ(mock_model_errors(e->os), 0);
	CHECK_EQ(mock_irq_unclaimed(e->os), 0);
	mock_destroy(e->os);
}

#define IOC(e, req, arg) pw_file_ioctl((e)->file, _IOC_NR(req), (arg), _IOC_SIZE(req))

/* ---- CS instructions (genxml v10.xml) ---- */
#define OP(o)		((uint64_t)(o) << 56)
static uint64_t cs_mov48(uint32_t r, uint64_t v)
{
	return OP(1) | ((uint64_t)r << 48) | (v & 0xffffffffffffull);
}
static uint64_t cs_mov32(uint32_t r, uint32_t v)
{
	return OP(2) | ((uint64_t)r << 48) | v;
}
static uint64_t cs_add_imm32(uint32_t d, uint32_t s, int32_t imm)
{
	return OP(16) | ((uint64_t)d << 48) | ((uint64_t)s << 40) | (uint32_t)imm;
}
static uint64_t cs_store_multiple(uint32_t base, uint32_t addr, uint16_t mask, int16_t off)
{
	return OP(21) | ((uint64_t)base << 48) | ((uint64_t)addr << 40) | ((uint64_t)mask << 16) |
	       (uint16_t)off;
}
static uint64_t cs_sync_add64(uint32_t addr, uint32_t data)
{
	return OP(51) | ((uint64_t)addr << 40) | ((uint64_t)data << 32);
}
static uint64_t cs_sync_set32(uint32_t addr, uint32_t data)
{
	return OP(38) | ((uint64_t)addr << 40) | ((uint64_t)data << 32);
}
/* cond: 0 Lequal, 1 Greater */
static uint64_t cs_sync_wait32(uint32_t cond, uint32_t addr, uint32_t data)
{
	return OP(39) | ((uint64_t)cond << 28) | ((uint64_t)addr << 40) | ((uint64_t)data << 32);
}
static uint64_t cs_call(uint32_t addr, uint32_t len)
{
	return OP(32) | ((uint64_t)addr << 40) | ((uint64_t)len << 32);
}
static uint64_t cs_heap_set(uint32_t addr)
{
	return OP(48) | ((uint64_t)addr << 40);
}
#define CS_RUN_COMPUTE	OP(4)
#define CS_RUN_IDVS	OP(6)
#define CS_NOP		OP(0)
#define CS_BAD		OP(0x3f)	/* no such opcode */

/* ---- object helpers ---- */

static uint32_t bo_new(struct senv *e, uint64_t size)
{
	struct drm_panthor_bo_create c = { .size = size };

	CHECK_EQ(IOC(e, DRM_IOCTL_PANTHOR_BO_CREATE, &c), PW_OK);
	return c.handle;
}

static uint32_t vm_new(struct senv *e)
{
	struct drm_panthor_vm_create c = { 0 };

	CHECK_EQ(IOC(e, DRM_IOCTL_PANTHOR_VM_CREATE, &c), PW_OK);
	return c.id;
}

static void map_bo(struct senv *e, uint32_t vm, uint32_t bo, uint64_t va, uint64_t size)
{
	struct drm_panthor_vm_bind_op op = {
		.flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, .bo_handle = bo, .va = va, .size = size,
	};
	struct drm_panthor_vm_bind b = { .vm_id = vm, .ops = DRM_PANTHOR_OBJ_ARRAY(1, &op) };

	CHECK_EQ(IOC(e, DRM_IOCTL_PANTHOR_VM_BIND, &b), PW_OK);
}

/* CPU access to a user BO through its pages (the GPU sees "DRAM" directly: no cache). */
static void bo_rw(struct senv *e, uint32_t handle, uint64_t off, void *buf, size_t len,
		  bool write)
{
	struct pw_bo *bo = pw_file_bo_get(e->file, handle);
	uint8_t *b = buf;

	REQUIRE(bo);
	while (len) {
		size_t chunk = 0x1000 - (off & 0xfff);
		uint8_t *host;

		if (chunk > len)
			chunk = len;
		host = mock_pa_to_host(e->os, pw_os_mem_page_pa(bo->mem, (size_t)(off >> 12)) +
					      (off & 0xfff));
		REQUIRE(host);
		if (write)
			memcpy(host, b, chunk);
		else
			memcpy(b, host, chunk);
		off += chunk;
		b += chunk;
		len -= chunk;
	}
	pw_bo_put(bo);
}

static void bo_zero(struct senv *e, uint32_t handle, uint64_t size)
{
	void *z = calloc(1, size);

	bo_rw(e, handle, 0, z, size, true);
	free(z);
}

static uint64_t bo_r64(struct senv *e, uint32_t handle, uint64_t off)
{
	uint64_t v = 0;

	bo_rw(e, handle, off, &v, 8, false);
	return v;
}

static void bo_w32(struct senv *e, uint32_t handle, uint64_t off, uint32_t v)
{
	bo_rw(e, handle, off, &v, 4, true);
}

static int group_create(struct senv *e, uint32_t vm, uint32_t nqueues, uint32_t ringbuf_size,
			uint32_t *handle)
{
	struct drm_panthor_queue_create q[8];
	struct drm_panthor_group_create g = {
		.queues = DRM_PANTHOR_OBJ_ARRAY(nqueues, q),
		.max_compute_cores = 1, .max_fragment_cores = 1, .max_tiler_cores = 1,
		.priority = PANTHOR_GROUP_PRIORITY_MEDIUM,
		.compute_core_mask = MOCK_G610_SHADER_PRESENT,
		.fragment_core_mask = MOCK_G610_SHADER_PRESENT,
		.tiler_core_mask = 1, .vm_id = vm,
	};
	int ret;

	memset(q, 0, sizeof(q));
	for (uint32_t i = 0; i < 8; i++) {
		q[i].priority = (uint8_t)i;
		q[i].ringbuf_size = ringbuf_size;
	}
	ret = IOC(e, DRM_IOCTL_PANTHOR_GROUP_CREATE, &g);
	if (handle)
		*handle = g.group_handle;
	return ret;
}

static int group_destroy(struct senv *e, uint32_t handle)
{
	struct drm_panthor_group_destroy d = { .group_handle = handle };

	return IOC(e, DRM_IOCTL_PANTHOR_GROUP_DESTROY, &d);
}

static uint32_t group_state(struct senv *e, uint32_t handle, uint32_t *fatal_queues)
{
	struct drm_panthor_group_get_state s = { .group_handle = handle };

	CHECK_EQ(IOC(e, DRM_IOCTL_PANTHOR_GROUP_GET_STATE, &s), PW_OK);
	if (fatal_queues)
		*fatal_queues = s.fatal_queues;
	return s.state;
}

static int submit(struct senv *e, uint32_t group, uint32_t queue, uint64_t va, uint32_t size,
		  const struct drm_panthor_sync_op *ops, uint32_t nops)
{
	static const struct drm_panthor_sync_op none[1];
	struct drm_panthor_queue_submit qs = {
		.queue_index = queue, .stream_size = size, .stream_addr = va,
		.syncs = DRM_PANTHOR_OBJ_ARRAY(nops, nops ? ops : none),
	};
	struct drm_panthor_group_submit gs = {
		.group_handle = group, .queue_submits = DRM_PANTHOR_OBJ_ARRAY(1, &qs),
	};

	return IOC(e, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs);
}

static struct drm_panthor_sync_op sig_op(uint32_t h, uint64_t point)
{
	struct drm_panthor_sync_op op = {
		.flags = DRM_PANTHOR_SYNC_OP_SIGNAL |
			 (point ? DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ :
				  DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ),
		.handle = h, .timeline_value = point,
	};
	return op;
}

static struct drm_panthor_sync_op wait_op(uint32_t h, uint64_t point)
{
	struct drm_panthor_sync_op op = {
		.flags = DRM_PANTHOR_SYNC_OP_WAIT |
			 (point ? DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ :
				  DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ),
		.handle = h, .timeline_value = point,
	};
	return op;
}

static uint32_t syncobj_new(struct senv *e)
{
	struct drm_syncobj_create c = { 0 };

	CHECK_EQ(IOC(e, DRM_IOCTL_SYNCOBJ_CREATE, &c), PW_OK);
	return c.handle;
}

static int64_t mono_deadline(uint32_t ms)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec + (int64_t)ms * 1000000;
}

/* SYNCOBJ_WAIT (binary, point 0) or TIMELINE_WAIT, both WAIT_FOR_SUBMIT. */
static int so_wait(struct senv *e, uint32_t h, uint64_t point, uint32_t ms)
{
	if (!point) {
		struct drm_syncobj_wait w = {
			.handles = (uintptr_t)&h, .timeout_nsec = mono_deadline(ms),
			.count_handles = 1, .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
		};
		return IOC(e, DRM_IOCTL_SYNCOBJ_WAIT, &w);
	} else {
		struct drm_syncobj_timeline_wait w = {
			.handles = (uintptr_t)&h, .points = (uintptr_t)&point,
			.timeout_nsec = mono_deadline(ms), .count_handles = 1,
			.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
		};
		return IOC(e, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &w);
	}
}

/* Error of the fence at (h, point): kernel API, userspace has no way to read it. */
static int fence_err(struct senv *e, uint32_t h, uint64_t point, bool *signalled)
{
	struct pw_fence *f = NULL;
	int err;

	if (pw_syncobj_find_fence(e->file, h, point, 0, 0, &f)) {
		*signalled = false;
		return 1;
	}
	*signalled = pw_fence_is_signalled(f);
	err = pw_fence_error(f);
	pw_fence_put(f);
	return err;
}

static bool fence_pending(struct senv *e, uint32_t h, uint64_t point)
{
	bool s;

	fence_err(e, h, point, &s);
	return !s;
}

/* Write @n instructions at @off of @bo; returns the byte size. */
static uint32_t put_cs(struct senv *e, uint32_t bo, uint64_t off, const uint64_t *ins,
		       uint32_t n)
{
	bo_rw(e, bo, off, (void *)ins, n * 8u, true);
	return n * 8u;
}

#define STREAM_VA	0x1000000ull	/* 64 KiB of command streams */
#define DATA_VA		0x2000000ull	/* 64 KiB of data */
#define BUF_SIZE	0x10000ull

struct world {
	uint32_t vm, stream, data;
};

static void world_init(struct senv *e, struct world *w)
{
	w->vm = vm_new(e);
	w->stream = bo_new(e, BUF_SIZE);
	w->data = bo_new(e, BUF_SIZE);
	bo_zero(e, w->stream, BUF_SIZE);
	bo_zero(e, w->data, BUF_SIZE);
	map_bo(e, w->vm, w->stream, STREAM_VA, BUF_SIZE);
	map_bo(e, w->vm, w->data, DATA_VA, BUF_SIZE);
}

/* "data[off] += add" stream at stream offset @soff (64-byte aligned). */
static uint32_t cs_add_stream(struct senv *e, struct world *w, uint64_t soff, uint64_t off,
			      uint64_t add)
{
	const uint64_t ins[] = {
		cs_mov48(0, DATA_VA + off), cs_mov48(2, add), CS_RUN_COMPUTE,
		cs_sync_add64(0, 2),
	};

	return put_cs(e, w->stream, soff, ins, 4);
}

/* Blocks until data32[off] > 0 ("Greater" than r4 = 0), then adds 1 to data[off2]. */
static uint32_t cs_wait_stream(struct senv *e, struct world *w, uint64_t soff, uint64_t off,
			       uint64_t off2)
{
	const uint64_t ins[] = {
		cs_mov48(0, DATA_VA + off), cs_mov32(4, 0), cs_sync_wait32(1, 0, 4),
		cs_mov48(0, DATA_VA + off2), cs_mov48(2, 1), cs_sync_add64(0, 2),
	};

	return put_cs(e, w->stream, soff, ins, 6);
}

/* ------------------------------------------------------------------------------------------ */

static void test_sched_group_create_destroy(void)
{
	struct drm_panthor_queue_create q[2] = {
		{ .priority = 1, .ringbuf_size = 0x1000 }, { .priority = 2, .ringbuf_size = 0x10000 },
	};
	struct drm_panthor_group_create g;
	struct drm_panthor_group_get_state st = { 0 };
	struct senv e;
	uint32_t vm, h = 0, h2 = 0;
	uint32_t handles[129];
	int n;

	if (!senv_init(&e))
		goto out;
	vm = vm_new(&e);

#define RESET_G() do { \
		memset(&g, 0, sizeof(g)); \
		g.queues = (struct drm_panthor_obj_array)DRM_PANTHOR_OBJ_ARRAY(2, q); \
		g.compute_core_mask = MOCK_G610_SHADER_PRESENT; \
		g.fragment_core_mask = MOCK_G610_SHADER_PRESENT; \
		g.tiler_core_mask = 1; g.max_compute_cores = 4; g.max_fragment_cores = 4; \
		g.max_tiler_cores = 1; g.vm_id = vm; \
	} while (0)

	/* Argument checks (drv.c:1231, group_priority_permit(), sched.c:3638-3657). */
	RESET_G(); g.queues.count = 0;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_CREATE, &g), PW_EINVAL);
	RESET_G(); g.queues.count = 33;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_CREATE, &g), PW_EINVAL);
	RESET_G(); g.priority = PANTHOR_GROUP_PRIORITY_HIGH;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_CREATE, &g), PW_EACCES);
	RESET_G(); g.priority = PANTHOR_GROUP_PRIORITY_REALTIME + 1;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_CREATE, &g), PW_EINVAL);
	RESET_G(); g.pad = 1;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_CREATE, &g), PW_EINVAL);
	RESET_G(); g.compute_core_mask |= 1ull << 40;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_CREATE, &g), PW_EINVAL);
	RESET_G(); g.tiler_core_mask = 2;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_CREATE, &g), PW_EINVAL);
	RESET_G(); g.max_fragment_cores = 5;	/* shader_present has 4 cores */
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_CREATE, &g), PW_EINVAL);
	RESET_G(); g.vm_id = 77;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_CREATE, &g), PW_EINVAL);
	{
		struct drm_panthor_queue_create many[9];

		memset(many, 0, sizeof(many));
		for (int i = 0; i < 9; i++)
			many[i].ringbuf_size = 0x1000;
		RESET_G();
		g.queues = (struct drm_panthor_obj_array)DRM_PANTHOR_OBJ_ARRAY(9, many);
		/* 9 > the 8 CS slots per CSG of the mock FW */
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_CREATE, &g), PW_EINVAL);
	}
	q[1].ringbuf_size = 0x20000;
	RESET_G();
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_CREATE, &g), PW_EINVAL);
	q[1].ringbuf_size = 0x3000;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_CREATE, &g), PW_EINVAL);
	q[1].ringbuf_size = 0x800;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_CREATE, &g), PW_EINVAL);
	q[1].ringbuf_size = 0x10000;
	q[1].priority = 16;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_CREATE, &g), PW_EINVAL);
	q[1].priority = 2;
	q[0].pad[1] = 1;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_CREATE, &g), PW_EINVAL);
	q[0].pad[1] = 0;

	/* Success; the handle is the lowest free one. */
	RESET_G();
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_CREATE, &g), PW_OK);
	h = g.group_handle;
	CHECK_EQ(h, 1);
	CHECK_EQ(group_state(&e, h, NULL), 0);
	CHECK_EQ(group_create(&e, vm, 1, 0x1000, &h2), PW_OK);
	CHECK_EQ(h2, 2);

	/* GET_STATE / DESTROY errors */
	st.group_handle = h;
	st.pad = 1;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_GET_STATE, &st), PW_EINVAL);
	st.pad = 0;
	st.group_handle = 99;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_GET_STATE, &st), PW_EINVAL);
	{
		struct drm_panthor_group_destroy d = { .group_handle = h, .pad = 1 };

		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_DESTROY, &d), PW_EINVAL);
	}
	CHECK_EQ(group_destroy(&e, 0), PW_EINVAL);
	CHECK_EQ(group_destroy(&e, 1000), PW_EINVAL);
	CHECK_EQ(group_destroy(&e, h), PW_OK);
	CHECK_EQ(group_destroy(&e, h), PW_EINVAL);
	st.group_handle = h;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_GET_STATE, &st), PW_EINVAL);
	CHECK_EQ(group_destroy(&e, h2), PW_OK);

	/* MAX_GROUPS_PER_POOL = 128, then EBUSY (xa_alloc limit). */
	for (n = 0; n < 129; n++) {
		int ret = group_create(&e, vm, 1, 0x1000, &handles[n]);

		if (n < 128) {
			CHECK_EQ(ret, PW_OK);
			CHECK_EQ(handles[n], (uint32_t)n + 1);
		} else {
			CHECK_EQ(ret, PW_EBUSY);
		}
	}
	CHECK_EQ(group_destroy(&e, 64), PW_OK);
	CHECK_EQ(group_create(&e, vm, 1, 0x1000, &h), PW_OK);
	CHECK_EQ(h, 64);
	/* The rest go with the file. */
#undef RESET_G
out:
	if (e.os)
		senv_fini(&e);
}

static void test_sched_submit_basic(void)
{
	struct senv e;
	struct world w;
	struct mock_cs_stats cs;
	uint32_t grp, so, sz;
	struct drm_panthor_sync_op sig;
	uint64_t ring[16], ep;
	bool signalled;
	int as;

	if (!senv_init(&e))
		goto out;
	world_init(&e, &w);
	CHECK_EQ(group_create(&e, w.vm, 2, 0x1000, &grp), PW_OK);
	so = syncobj_new(&e);
	sz = cs_add_stream(&e, &w, 0, 0x100, 5);
	sig = sig_op(so, 0);

	/* job_create() argument checks (drv.c:1066) */
	{
		struct drm_panthor_queue_submit qs = {
			.queue_index = 0, .stream_size = sz, .stream_addr = STREAM_VA,
			.syncs = DRM_PANTHOR_OBJ_ARRAY(1, &sig),
		};
		struct drm_panthor_group_submit gs = {
			.group_handle = grp, .queue_submits = DRM_PANTHOR_OBJ_ARRAY(1, &qs),
		};

		gs.pad = 1;
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs), PW_EINVAL);
		gs.pad = 0;
		qs.pad = 1;
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs), PW_EINVAL);
		qs.pad = 0;
		qs.stream_addr = STREAM_VA + 8;
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs), PW_EINVAL);
		qs.stream_addr = STREAM_VA;
		qs.stream_size = 12;
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs), PW_EINVAL);
		qs.stream_size = 0;
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs), PW_EINVAL);
		qs.stream_size = sz;
		qs.latest_flush = 1u << 24;
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs), PW_EINVAL);
		qs.latest_flush = 0;
		qs.queue_index = 2;
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs), PW_EINVAL);
		qs.queue_index = 0;
		gs.group_handle = 9;
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs), PW_EINVAL);
		gs.group_handle = grp;
		/* Sync ops: bad flags, binary with a point, missing signal syncobj, missing wait
		 * fence. Nothing must have been queued by any of these. */
		sig.flags |= 0x100;
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs), PW_EINVAL);
		sig = sig_op(so, 0);
		sig.timeline_value = 3;
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs), PW_EINVAL);
		sig = sig_op(so + 100, 0);
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs), PW_EINVAL);
		sig = wait_op(so, 0);	/* no fence attached yet */
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs), PW_EINVAL);
		qs.syncs.stride = 8;	/* smaller than the mandatory part */
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs), PW_EINVAL);
		CHECK_EQ(bo_r64(&e, w.data, 0x100), 0);
		/* An empty submit is fine. */
		gs.queue_submits.count = 0;
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs), PW_OK);
	}

	sig = sig_op(so, 0);
	CHECK_EQ(submit(&e, grp, 0, STREAM_VA, sz, &sig, 1), PW_OK);
	CHECK_EQ(so_wait(&e, so, 0, 2000), PW_OK);
	CHECK_EQ(fence_err(&e, so, 0, &signalled), 0);
	CHECK(signalled);
	CHECK_EQ(bo_r64(&e, w.data, 0x100), 5);

	/* What the MCU saw: CSG 0 running on the VM's AS, queue 0 on doorbell 1 with its
	 * priority, the kernel sequence executed (one flush, one CALL, user SYNC + kernel SYNC). */
	CHECK(mock_csf_csg_running(e.os, 0, &ep));
	CHECK_EQ(ep >> 28, 15);		/* CSG priority of slot 0 */
	CHECK_EQ(ep & 0xff, 1);		/* max compute cores */
	mock_csf_cs_stats(e.os, 0, 0, &cs);
	CHECK(cs.started);
	CHECK(!cs.fatal);
	CHECK_EQ(cs.doorbell, 1);
	CHECK_EQ(cs.priority, 0);
	CHECK_EQ(cs.flushes, 1);
	CHECK_EQ(cs.calls, 1);
	CHECK_EQ(cs.runs, 1);
	CHECK_EQ(cs.syncs, 2);
	CHECK_EQ(cs.extract, 128);	/* 11 instructions padded to 16 */
	CHECK_EQ(cs.ringbuf_size, 0x1000);
	mock_csf_cs_stats(e.os, 0, 1, &cs);
	CHECK(cs.started);
	CHECK_EQ(cs.priority, 1);
	CHECK_EQ(cs.instrs, 0);

	/* The ring holds panthor's prepare_job_instrs() sequence (sched.c:3195), regs 92-94
	 * (96 CS registers, 4 unpreserved), sb mask 0xff. */
	mock_csf_cs_stats(e.os, 0, 0, &cs);
	as = (int)cs.as;
	CHECK(as > 0);
	CHECK_EQ(mock_gpu_read(e.os, (unsigned int)as, cs.ringbuf_base, ring, sizeof(ring)), 0);
	CHECK_EQ(ring[0], (2ull << 56) | (94ull << 48) | 0);
	CHECK_EQ(ring[1], (36ull << 56) | (94ull << 40) | 0x233);
	CHECK_EQ(ring[2], (1ull << 56) | (92ull << 48) | STREAM_VA);
	CHECK_EQ(ring[3], (2ull << 56) | (94ull << 48) | sz);
	CHECK_EQ(ring[4], (3ull << 56) | (1ull << 16));
	CHECK_EQ(ring[5], (32ull << 56) | (92ull << 40) | (94ull << 32));
	CHECK_EQ(ring[6] & ~0xffffffffffffull, (1ull << 56) | (92ull << 48));
	CHECK_EQ(ring[7], (1ull << 56) | (94ull << 48) | 1);
	CHECK_EQ(ring[8], (3ull << 56) | (0xffull << 16));
	CHECK_EQ(ring[9], (51ull << 56) | (92ull << 40) | (94ull << 32) | 1);
	CHECK_EQ(ring[10], 47ull << 56);
	for (int i = 11; i < 16; i++)
		CHECK_EQ(ring[i], 0);
	{
		/* The queue's syncobj (ring[6]'s address) is in the kernel VA range and reads 1. */
		uint64_t sync_va = ring[6] & 0xffffffffffffull, seq = 0;
		struct pw_uvm *uvm = pw_file_vm_get(e.file, w.vm);
		uint64_t ks = 0, ke = 0;

		REQUIRE(uvm);
		pw_uvm_kernel_va_range(uvm, &ks, &ke);
		pw_uvm_put(uvm);
		CHECK(sync_va >= ks && sync_va < ke);
		CHECK(cs.ringbuf_base >= ks && cs.ringbuf_base < ke);
		CHECK_EQ(mock_gpu_read(e.os, (unsigned int)as, sync_va, &seq, 8), 0);
		CHECK_EQ(seq, 1);
		/* Kernel mappings are NOEXEC */
		CHECK(mock_gpu_translate(e.os, (unsigned int)as, sync_va, 'x', NULL, NULL, NULL));
	}

	/* A second job on the other queue, with latest_flush and a timeline point. */
	{
		uint32_t tl = syncobj_new(&e);
		struct drm_panthor_sync_op ops[2] = { sig_op(tl, 7), sig_op(so, 0) };

		sz = cs_add_stream(&e, &w, 0x40, 0x108, 3);
		CHECK_EQ(submit(&e, grp, 1, STREAM_VA + 0x40, sz, ops, 2), PW_OK);
		CHECK_EQ(so_wait(&e, tl, 7, 2000), PW_OK);
		CHECK_EQ(so_wait(&e, so, 0, 2000), PW_OK);
		CHECK_EQ(bo_r64(&e, w.data, 0x108), 3);
		mock_csf_cs_stats(e.os, 0, 1, &cs);
		CHECK_EQ(cs.syncs, 2);
		CHECK_EQ(cs.doorbell, 1);	/* all queues of a group share the doorbell */
	}
	CHECK_EQ(group_state(&e, grp, NULL), 0);
	CHECK_EQ(group_destroy(&e, grp), PW_OK);
	CHECK(!mock_csf_csg_running(e.os, 0, NULL));
out:
	if (e.os)
		senv_fini(&e);
}

/* Many jobs through a 4 KiB ring (32 jobs of 128 bytes): wraps, and waits for space. */
static void test_sched_ring_wrap(void)
{
	enum { N = 300 };
	struct senv e;
	struct world w;
	struct mock_cs_stats cs;
	uint32_t grp, tl, sz;

	if (!senv_init(&e))
		goto out;
	world_init(&e, &w);
	CHECK_EQ(group_create(&e, w.vm, 1, 0x1000, &grp), PW_OK);
	tl = syncobj_new(&e);
	sz = cs_add_stream(&e, &w, 0, 0, 1);
	for (uint32_t i = 0; i < N; i++) {
		struct drm_panthor_sync_op op = sig_op(tl, i + 1);

		CHECK_EQ(submit(&e, grp, 0, STREAM_VA, sz, &op, 1), PW_OK);
	}
	CHECK_EQ(so_wait(&e, tl, N, 5000), PW_OK);
	CHECK_EQ(bo_r64(&e, w.data, 0), N);
	mock_csf_cs_stats(e.os, 0, 0, &cs);
	CHECK_EQ(cs.extract, (uint64_t)N * 128);
	CHECK_EQ(cs.calls, N);
	CHECK(!cs.fatal);
	{
		struct drm_syncobj_timeline_array q = { 0 };
		uint64_t point = 0;

		q.handles = (uintptr_t)&tl;
		q.points = (uintptr_t)&point;
		q.count_handles = 1;
		CHECK_EQ(IOC(&e, DRM_IOCTL_SYNCOBJ_QUERY, &q), PW_OK);
		CHECK_EQ(point, N);
	}
out:
	if (e.os)
		senv_fini(&e);
}

/* stream_size == 0: completes with the queue's previous job (sched.c:3305). */
static void test_sched_sync_only(void)
{
	struct senv e;
	struct world w;
	uint32_t grp, a, b, c, sz;
	struct drm_panthor_sync_op op;
	bool s;

	if (!senv_init(&e))
		goto out;
	world_init(&e, &w);
	CHECK_EQ(group_create(&e, w.vm, 1, 0x1000, &grp), PW_OK);
	a = syncobj_new(&e);
	b = syncobj_new(&e);
	c = syncobj_new(&e);

	/* Nothing submitted before: signalled right away, and the group is not even bound. */
	op = sig_op(a, 0);
	CHECK_EQ(submit(&e, grp, 0, 0, 0, &op, 1), PW_OK);
	CHECK_EQ(so_wait(&e, a, 0, 1000), PW_OK);
	CHECK_EQ(fence_err(&e, a, 0, &s), 0);
	CHECK(!mock_csf_csg_running(e.os, 0, NULL));

	/* Behind a blocked job: not before it. */
	sz = cs_wait_stream(&e, &w, 0, 0x200, 0x208);
	op = sig_op(b, 0);
	CHECK_EQ(submit(&e, grp, 0, STREAM_VA, sz, &op, 1), PW_OK);
	op = sig_op(c, 0);
	CHECK_EQ(submit(&e, grp, 0, 0, 0, &op, 1), PW_OK);
	CHECK_EQ(so_wait(&e, c, 0, 100), PW_ETIME);
	CHECK(fence_pending(&e, b, 0));
	CHECK(fence_pending(&e, c, 0));
	bo_w32(&e, w.data, 0x200, 1);
	mock_csf_kick(e.os);
	CHECK_EQ(so_wait(&e, c, 0, 2000), PW_OK);
	CHECK_EQ(so_wait(&e, b, 0, 2000), PW_OK);
	CHECK_EQ(bo_r64(&e, w.data, 0x208), 1);
	CHECK_EQ(fence_err(&e, c, 0, &s), 0);
out:
	if (e.os)
		senv_fini(&e);
}

/* In-fences: across groups, within one batch, and the errors. */
static void test_sched_insync_deps(void)
{
	struct senv e;
	struct world w;
	struct mock_cs_stats cs;
	uint32_t g1, g2, s1, s2, s3, s4, sz1, sz2, sz3;
	struct drm_panthor_sync_op ops[2];
	bool s;

	if (!senv_init(&e))
		goto out;
	world_init(&e, &w);
	CHECK_EQ(group_create(&e, w.vm, 1, 0x1000, &g1), PW_OK);
	CHECK_EQ(group_create(&e, w.vm, 1, 0x1000, &g2), PW_OK);
	s1 = syncobj_new(&e);
	s2 = syncobj_new(&e);
	s3 = syncobj_new(&e);
	s4 = syncobj_new(&e);

	/* g1: blocked on memory, signals s1. g2: waits for s1, then data[0x300] += 7. */
	sz1 = cs_wait_stream(&e, &w, 0, 0x400, 0x408);
	ops[0] = sig_op(s1, 0);
	CHECK_EQ(submit(&e, g1, 0, STREAM_VA, sz1, ops, 1), PW_OK);
	sz2 = cs_add_stream(&e, &w, 0x40, 0x300, 7);
	ops[0] = wait_op(s1, 0);
	ops[1] = sig_op(s2, 0);
	CHECK_EQ(submit(&e, g2, 0, STREAM_VA + 0x40, sz2, ops, 2), PW_OK);
	usleep(50 * 1000);
	CHECK(fence_pending(&e, s1, 0));
	CHECK(fence_pending(&e, s2, 0));
	CHECK_EQ(bo_r64(&e, w.data, 0x300), 0);
	/* g2's job never reached a ring: g2 is not even bound (CSG 1 idle). */
	CHECK(!mock_csf_csg_running(e.os, 1, NULL));
	mock_csf_cs_stats(e.os, 0, 0, &cs);
	CHECK(cs.blocked_sync);

	bo_w32(&e, w.data, 0x400, 1);
	mock_csf_kick(e.os);
	CHECK_EQ(so_wait(&e, s2, 0, 2000), PW_OK);
	CHECK_EQ(fence_err(&e, s1, 0, &s), 0);
	CHECK_EQ(bo_r64(&e, w.data, 0x300), 7);
	CHECK_EQ(bo_r64(&e, w.data, 0x408), 1);

	/* One batch, two jobs on two queues of g1... (g1 has one queue: use g2 twice): job 0
	 * signals s3 point 1 of a timeline, job 1 waits for it (drv.c:664 ordering). */
	{
		struct drm_panthor_sync_op o0[1] = { sig_op(s3, 1) };
		struct drm_panthor_sync_op o1[2] = { wait_op(s3, 1), sig_op(s4, 0) };
		struct drm_panthor_queue_submit qs[2] = {
			{ .queue_index = 0, .stream_addr = STREAM_VA + 0x80, .syncs =
			  DRM_PANTHOR_OBJ_ARRAY(1, o0) },
			{ .queue_index = 0, .stream_addr = STREAM_VA + 0xc0, .syncs =
			  DRM_PANTHOR_OBJ_ARRAY(2, o1) },
		};
		struct drm_panthor_group_submit gs = {
			.group_handle = g2, .queue_submits = DRM_PANTHOR_OBJ_ARRAY(2, qs),
		};

		sz3 = cs_add_stream(&e, &w, 0x80, 0x310, 1);
		qs[0].stream_size = sz3;
		qs[1].stream_size = cs_add_stream(&e, &w, 0xc0, 0x310, 10);
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs), PW_OK);
		CHECK_EQ(so_wait(&e, s4, 0, 2000), PW_OK);
		CHECK_EQ(so_wait(&e, s3, 1, 2000), PW_OK);
		CHECK_EQ(bo_r64(&e, w.data, 0x310), 11);

		/* A job waiting on a point a later job of the same batch signals: the wait
		 * looks at the syncobj, where nothing is attached -> EINVAL. */
		o0[0] = wait_op(s3, 5);
		o1[0] = sig_op(s3, 5);
		o1[1] = sig_op(s4, 0);
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs), PW_EINVAL);
	}
	/* A wait on an unknown syncobj */
	ops[0] = wait_op(77, 0);
	CHECK_EQ(submit(&e, g1, 0, STREAM_VA, sz1, ops, 1), PW_EINVAL);
	/* A wait on an already-signalled CPU fence goes straight through. */
	{
		struct drm_syncobj_array sa = { .handles = (uintptr_t)&s4, .count_handles = 1 };
		uint32_t s5 = syncobj_new(&e);

		CHECK_EQ(IOC(&e, DRM_IOCTL_SYNCOBJ_SIGNAL, &sa), PW_OK);
		ops[0] = wait_op(s4, 0);
		ops[1] = sig_op(s5, 0);
		CHECK_EQ(submit(&e, g1, 0, STREAM_VA + 0x40, sz2, ops, 2), PW_OK);
		CHECK_EQ(so_wait(&e, s5, 0, 2000), PW_OK);
		CHECK_EQ(bo_r64(&e, w.data, 0x300), 14);
	}
	CHECK_EQ(group_state(&e, g1, NULL), 0);
	CHECK_EQ(group_state(&e, g2, NULL), 0);
out:
	if (e.os)
		senv_fini(&e);
}

/* User streams using more of the instruction set: nested CALLs, ADD_IMM32, STORE_MULTIPLE,
 * SYNC_SET32, then unbounded recursion (CS_CALL_STACK_OVERFLOW). */
static void test_sched_user_stream(void)
{
	struct senv e;
	struct world w;
	struct mock_cs_stats cs;
	uint32_t grp, g2, so, so2, sza, szb, szc;
	struct drm_panthor_sync_op op;
	uint32_t v[3] = { 0 };
	bool s;

	if (!senv_init(&e))
		goto out;
	world_init(&e, &w);
	CHECK_EQ(group_create(&e, w.vm, 1, 0x1000, &grp), PW_OK);
	so = syncobj_new(&e);
	{
		const uint64_t b[] = {
			cs_mov48(20, DATA_VA + 0xc10), cs_mov48(2, 1), cs_sync_add64(20, 2),
		};

		szb = put_cs(&e, w.stream, 0x480, b, 3);
	}
	{
		const uint64_t a[] = {
			cs_mov48(20, DATA_VA + 0xc00), cs_mov32(30, 0x11), cs_add_imm32(31, 30, 5),
			cs_store_multiple(30, 20, 0x3, 0),
			cs_mov48(22, DATA_VA + 0xc08), cs_mov32(32, 9), cs_sync_set32(22, 32),
			cs_mov48(24, STREAM_VA + 0x480), cs_mov32(26, szb), cs_call(24, 26),
			cs_call(24, 26),
		};

		sza = put_cs(&e, w.stream, 0x400, a, 11);
	}
	op = sig_op(so, 0);
	CHECK_EQ(submit(&e, grp, 0, STREAM_VA + 0x400, sza, &op, 1), PW_OK);
	CHECK_EQ(so_wait(&e, so, 0, 2000), PW_OK);
	CHECK_EQ(fence_err(&e, so, 0, &s), 0);
	bo_rw(&e, w.data, 0xc00, v, sizeof(v), false);
	CHECK_EQ(v[0], 0x11);
	CHECK_EQ(v[1], 0x16);
	CHECK_EQ(v[2], 9);
	CHECK_EQ(bo_r64(&e, w.data, 0xc10), 2);
	mock_csf_cs_stats(e.os, 0, 0, &cs);
	CHECK_EQ(cs.calls, 3);
	CHECK_EQ(cs.syncs, 4);	/* SYNC_SET32 + 2 x SYNC_ADD64 + the kernel's */

	/* A stream that calls itself forever: the MCU gives up at its call depth. */
	CHECK_EQ(group_create(&e, w.vm, 1, 0x1000, &g2), PW_OK);
	so2 = syncobj_new(&e);
	{
		const uint64_t c[] = {
			cs_mov48(24, STREAM_VA + 0x500), cs_mov32(26, 24), cs_call(24, 26),
		};

		szc = put_cs(&e, w.stream, 0x500, c, 3);
	}
	op = sig_op(so2, 0);
	CHECK_EQ(submit(&e, g2, 0, STREAM_VA + 0x500, szc, &op, 1), PW_OK);
	CHECK_EQ(so_wait(&e, so2, 0, 2000), PW_OK);
	CHECK_EQ(fence_err(&e, so2, 0, &s), PW_EINVAL);
	mock_csf_cs_stats(e.os, 1, 0, &cs);
	CHECK_EQ(cs.fatal_type, 0x4a);
	CHECK_EQ(group_state(&e, grp, NULL), 0);
out:
	if (e.os)
		senv_fini(&e);
}

static void test_sched_cs_fatal(void)
{
	struct senv e;
	struct world w;
	struct mock_cs_stats cs;
	uint32_t grp, ok_so, bad_so, after_so, fq = 0, sz;
	struct drm_panthor_sync_op op;
	const uint64_t bad[] = { cs_mov32(1, 2), CS_BAD };
	bool s;

	if (!senv_init(&e))
		goto out;
	world_init(&e, &w);
	CHECK_EQ(group_create(&e, w.vm, 2, 0x1000, &grp), PW_OK);
	ok_so = syncobj_new(&e);
	bad_so = syncobj_new(&e);
	after_so = syncobj_new(&e);

	sz = cs_add_stream(&e, &w, 0, 0x500, 1);
	op = sig_op(ok_so, 0);
	CHECK_EQ(submit(&e, grp, 1, STREAM_VA, sz, &op, 1), PW_OK);
	CHECK_EQ(so_wait(&e, ok_so, 0, 2000), PW_OK);

	put_cs(&e, w.stream, 0x40, bad, 2);
	op = sig_op(bad_so, 0);
	CHECK_EQ(submit(&e, grp, 1, STREAM_VA + 0x40, 16, &op, 1), PW_OK);
	CHECK_EQ(so_wait(&e, bad_so, 0, 2000), PW_OK);
	/* sched.c:2253: jobs of a fatal queue fail with -EINVAL; the earlier one is fine. */
	CHECK_EQ(fence_err(&e, bad_so, 0, &s), PW_EINVAL);
	CHECK_EQ(fence_err(&e, ok_so, 0, &s), 0);
	CHECK_EQ(group_state(&e, grp, &fq), DRM_PANTHOR_GROUP_STATE_FATAL_FAULT);
	CHECK_EQ(fq, 1u << 1);
	mock_csf_cs_stats(e.os, 0, 1, &cs);
	CHECK(cs.fatal);
	CHECK_EQ(cs.fatal_type, 0x49);	/* CS_INSTR_INVALID */
	/* The group was terminated (CSG TERMINATE) and can't take jobs anymore. */
	for (int i = 0; i < 1000 && mock_csf_csg_running(e.os, 0, NULL); i++)
		usleep(1000);
	CHECK(!mock_csf_csg_running(e.os, 0, NULL));
	op = sig_op(after_so, 0);
	CHECK_EQ(submit(&e, grp, 0, STREAM_VA, sz, &op, 1), PW_EINVAL);
	CHECK(mock_log_contains(e.os, "CS_FATAL.EXCEPTION_TYPE: 0x49"));

	/* A bad GPU address in a sync op: CS_BUS_FAULT. */
	{
		uint32_t g2, so2;
		const uint64_t ins[] = { cs_mov48(0, 0x7000000000ull), cs_mov48(2, 1),
					 cs_sync_add64(0, 2) };

		CHECK_EQ(group_create(&e, w.vm, 1, 0x1000, &g2), PW_OK);
		so2 = syncobj_new(&e);
		put_cs(&e, w.stream, 0x80, ins, 3);
		op = sig_op(so2, 0);
		CHECK_EQ(submit(&e, g2, 0, STREAM_VA + 0x80, 24, &op, 1), PW_OK);
		CHECK_EQ(so_wait(&e, so2, 0, 2000), PW_OK);
		CHECK_EQ(fence_err(&e, so2, 0, &s), PW_EINVAL);
		CHECK_EQ(group_state(&e, g2, &fq), DRM_PANTHOR_GROUP_STATE_FATAL_FAULT);
		CHECK_EQ(fq, 1);
	}
	CHECK_EQ(group_destroy(&e, grp), PW_OK);
out:
	if (e.os)
		senv_fini(&e);
}

static void test_sched_timeout(void)
{
	struct senv e;
	struct world w;
	uint32_t grp, a, b, c, sz;
	struct drm_panthor_sync_op op;
	uint64_t t0;
	bool s;

	if (!senv_init(&e))
		goto out;
	pw_sched_set_job_timeout(e.dev, 150);
	world_init(&e, &w);
	CHECK_EQ(group_create(&e, w.vm, 2, 0x1000, &grp), PW_OK);
	a = syncobj_new(&e);
	b = syncobj_new(&e);
	c = syncobj_new(&e);

	/* Queue 0: a job that never completes, and one queued behind it in the ring. Queue 1: a
	 * job whose in-fence is a's, so it never reaches the ring. */
	sz = cs_wait_stream(&e, &w, 0, 0x600, 0x608);
	op = sig_op(a, 0);
	t0 = now_ms();
	CHECK_EQ(submit(&e, grp, 0, STREAM_VA, sz, &op, 1), PW_OK);
	op = sig_op(b, 0);
	CHECK_EQ(submit(&e, grp, 0, STREAM_VA, sz, &op, 1), PW_OK);
	{
		struct drm_panthor_sync_op ops[2] = { wait_op(a, 0), sig_op(c, 0) };

		CHECK_EQ(submit(&e, grp, 1, STREAM_VA, sz, ops, 2), PW_OK);
	}
	CHECK_EQ(so_wait(&e, a, 0, 3000), PW_OK);
	CHECK(now_ms() - t0 >= 140);
	CHECK(now_ms() - t0 < 2000);
	CHECK_EQ(so_wait(&e, b, 0, 1000), PW_OK);
	CHECK_EQ(so_wait(&e, c, 0, 1000), PW_OK);
	/* sched.c:2253-2258: in-flight jobs of a timed-out group get -ETIMEDOUT, jobs that
	 * never ran -ECANCELED (queue_run_job() on a group that can't run). */
	CHECK_EQ(fence_err(&e, a, 0, &s), PW_ETIMEDOUT);
	CHECK_EQ(fence_err(&e, b, 0, &s), PW_ETIMEDOUT);
	CHECK_EQ(fence_err(&e, c, 0, &s), PW_ECANCELED);
	CHECK_EQ(group_state(&e, grp, NULL), DRM_PANTHOR_GROUP_STATE_TIMEDOUT);
	CHECK(mock_log_contains(e.os, "job timeout"));
	CHECK_EQ(bo_r64(&e, w.data, 0x608), 0);
	op = sig_op(a, 0);
	CHECK_EQ(submit(&e, grp, 0, STREAM_VA, sz, &op, 1), PW_EINVAL);

	/* A busy queue that keeps completing jobs does not time out: 6 jobs of ~60 ms each
	 * (released one by one by the CPU) take longer than the timeout in total. */
	{
		uint32_t g2, tl;

		CHECK_EQ(group_create(&e, w.vm, 1, 0x1000, &g2), PW_OK);
		tl = syncobj_new(&e);
		for (uint32_t i = 0; i < 6; i++) {
			const uint64_t ins[] = {
				cs_mov48(0, DATA_VA + 0x700), cs_mov32(4, i), cs_sync_wait32(1, 0, 4),
			};

			put_cs(&e, w.stream, 0x100 + i * 0x40, ins, 3);
			op = sig_op(tl, i + 1);
			CHECK_EQ(submit(&e, g2, 0, STREAM_VA + 0x100 + i * 0x40, 24, &op, 1),
				 PW_OK);
		}
		for (uint32_t i = 0; i < 6; i++) {
			usleep(60 * 1000);
			bo_w32(&e, w.data, 0x700, i + 1);
			mock_csf_kick(e.os);
			CHECK_EQ(so_wait(&e, tl, i + 1, 1000), PW_OK);
		}
		CHECK_EQ(fence_err(&e, tl, 6, &s), 0);
		CHECK_EQ(group_state(&e, g2, NULL), 0);
	}
out:
	if (e.os)
		senv_fini(&e);
}

/* An unhandled MMU fault on the group's VM fails the group (sched.c:2212), found by the next
 * work run (here the job timeout's). */
static void test_sched_vm_fault(void)
{
	struct senv e;
	struct world w;
	struct mock_cs_stats cs;
	uint32_t grp, a, sz, fq = 0;
	struct drm_panthor_sync_op op;
	bool s;

	if (!senv_init(&e))
		goto out;
	pw_sched_set_job_timeout(e.dev, 200);
	world_init(&e, &w);
	CHECK_EQ(group_create(&e, w.vm, 2, 0x1000, &grp), PW_OK);
	a = syncobj_new(&e);
	sz = cs_wait_stream(&e, &w, 0, 0xd00, 0xd08);
	op = sig_op(a, 0);
	CHECK_EQ(submit(&e, grp, 0, STREAM_VA, sz, &op, 1), PW_OK);
	usleep(20 * 1000);
	mock_csf_cs_stats(e.os, 0, 0, &cs);
	CHECK(cs.blocked_sync);
	mock_inject_mmu_fault(e.os, cs.as, 0x000003c1u, DATA_VA + 0xd00);
	CHECK_EQ(so_wait(&e, a, 0, 3000), PW_OK);
	CHECK_EQ(fence_err(&e, a, 0, &s), PW_EINVAL);
	CHECK_EQ(group_state(&e, grp, &fq), DRM_PANTHOR_GROUP_STATE_FATAL_FAULT);
	CHECK_EQ(fq, 3);
	CHECK(mock_log_contains(e.os, "group VM faulted"));
	/* The VM is unusable for good (pw_bo.h rule): no kernel mapping, so no new group. */
	CHECK_EQ(group_create(&e, w.vm, 1, 0x1000, NULL), PW_EINVAL);
out:
	if (e.os)
		senv_fini(&e);
}

static void test_sched_slots(void)
{
	enum { NG = MOCK_FW_GROUP_NUM + 1 };
	struct senv e;
	struct world w;
	uint32_t g[NG], so[NG], sz, blk;
	struct drm_panthor_sync_op op;

	if (!senv_init(&e))
		goto out;
	world_init(&e, &w);
	sz = cs_add_stream(&e, &w, 0, 0x800, 1);
	blk = cs_wait_stream(&e, &w, 0x40, 0x900, 0x908);
	/* 8 groups take the 8 CSG slots (each stays bound: no rotation in the MVP); group 0 is
	 * blocked. The 9th waits for a free slot. */
	for (uint32_t i = 0; i < NG; i++) {
		CHECK_EQ(group_create(&e, w.vm, 1, 0x1000, &g[i]), PW_OK);
		so[i] = syncobj_new(&e);
		op = sig_op(so[i], 0);
		CHECK_EQ(submit(&e, g[i], 0, STREAM_VA + (i ? 0 : 0x40), i ? sz : blk, &op, 1),
			 PW_OK);
	}
	for (uint32_t i = 1; i < NG - 1; i++)
		CHECK_EQ(so_wait(&e, so[i], 0, 2000), PW_OK);
	CHECK_EQ(so_wait(&e, so[NG - 1], 0, 100), PW_ETIME);
	CHECK_EQ(bo_r64(&e, w.data, 0x800), NG - 2);
	for (uint32_t i = 0; i < MOCK_FW_GROUP_NUM; i++)
		CHECK(mock_csf_csg_running(e.os, i, NULL));
	/* Destroying an idle group frees its slot. */
	CHECK_EQ(group_destroy(&e, g[3]), PW_OK);
	CHECK_EQ(so_wait(&e, so[NG - 1], 0, 2000), PW_OK);
	CHECK_EQ(bo_r64(&e, w.data, 0x800), NG - 1);
	/* Destroying the blocked group cancels its job. */
	CHECK_EQ(group_destroy(&e, g[0]), PW_OK);
	{
		bool s;

		CHECK_EQ(fence_err(&e, so[0], 0, &s), PW_ECANCELED);
		CHECK(s);
	}
out:
	if (e.os)
		senv_fini(&e);
}

static void test_sched_heap(void)
{
	struct senv e;
	struct world w;
	struct drm_panthor_tiler_heap_create hc, heap0;
	struct drm_panthor_tiler_heap_destroy hd = { 0 };
	uint64_t ks = 0, ke = 0, hdr[2], heap_start = 0, heap_end = 0;
	uint32_t vm2, grp, so, sz;
	struct pw_uvm *uvm;
	struct drm_panthor_sync_op op;
	int as;
	bool s;

	if (!senv_init(&e))
		goto out;
	world_init(&e, &w);
	vm2 = vm_new(&e);

#define HC(chunks, csize, maxc) do { \
		memset(&hc, 0, sizeof(hc)); \
		hc.vm_id = w.vm; hc.initial_chunk_count = (chunks); hc.chunk_size = (csize); \
		hc.max_chunks = (maxc); hc.target_in_flight = 2; \
	} while (0)

	HC(1, 0x20000, 4); hc.vm_id = 99;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &hc), PW_EINVAL);
	HC(0, 0x20000, 4);
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &hc), PW_EINVAL);
	HC(5, 0x20000, 4);
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &hc), PW_EINVAL);
	HC(1, 0x1f000, 4);
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &hc), PW_EINVAL);
	HC(1, 0x801000, 4);
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &hc), PW_EINVAL);
	HC(1, 0x20800, 4);
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &hc), PW_EINVAL);

	/* Two initial chunks of 128 KiB: newest first, linked to the older one. */
	HC(2, 0x20000, 3);
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &hc), PW_OK);
	CHECK_EQ(hc.handle, (w.vm << 16) | 0);
	heap0 = hc;
	uvm = pw_file_vm_get(e.file, w.vm);
	REQUIRE(uvm);
	pw_uvm_kernel_va_range(uvm, &ks, &ke);
	CHECK_EQ(pw_uvm_activate(uvm), PW_OK);
	as = pw_vm_as(pw_uvm_vm(uvm));
	pw_uvm_put(uvm);
	CHECK(hc.tiler_heap_ctx_gpu_va >= ks && hc.tiler_heap_ctx_gpu_va < ke);
	CHECK(hc.first_heap_chunk_gpu_va >= ks && hc.first_heap_chunk_gpu_va < ke);
	CHECK_EQ(hc.first_heap_chunk_gpu_va & 0xfff, 0);
	CHECK_EQ(mock_gpu_read(e.os, (unsigned int)as, hc.first_heap_chunk_gpu_va, hdr, 16), 0);
	CHECK_EQ(hdr[0] & 0xfff, 0x20000 >> 12);
	{
		uint64_t second = hdr[0] & ~0xfffull, hdr2[8], ctx[4];

		CHECK(second >= ks && second < ke && second != hc.first_heap_chunk_gpu_va);
		CHECK_EQ(mock_gpu_read(e.os, (unsigned int)as, second, hdr2, 64), 0);
		for (int i = 0; i < 8; i++)
			CHECK_EQ(hdr2[i], 0);
		/* The heap context reads zero; chunks and contexts are NOEXEC. */
		CHECK_EQ(mock_gpu_read(e.os, (unsigned int)as, hc.tiler_heap_ctx_gpu_va, ctx, 32),
			 0);
		for (int i = 0; i < 4; i++)
			CHECK_EQ(ctx[i], 0);
		CHECK(mock_gpu_translate(e.os, (unsigned int)as, second, 'x', NULL, NULL, NULL));
	}
	{
		struct drm_panthor_tiler_heap_create h2;

		HC(1, 0x20000, 1);
		h2 = hc;
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &h2), PW_OK);
		CHECK_EQ(h2.handle, (w.vm << 16) | 1);
		CHECK_EQ(h2.tiler_heap_ctx_gpu_va, heap0.tiler_heap_ctx_gpu_va + 64); /* 64B lines */
		hd.handle = h2.handle;
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY, &hd), PW_OK);
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY, &hd), PW_EINVAL);
	}
	/* Destroy errors: pad, unknown VM, VM without a pool (mmu.c:2144 -> ENOENT). */
	hd.handle = (w.vm << 16) | 0;
	hd.pad = 1;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY, &hd), PW_EINVAL);
	hd.pad = 0;
	hd.handle = (99u << 16);
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY, &hd), PW_EINVAL);
	hd.handle = (vm2 << 16);
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY, &hd), PW_ENOENT);
	hd.handle = (w.vm << 16) | 200;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY, &hd), PW_EINVAL);

	/* TILER_OOM: HEAP_SET + RUN_IDVS; the MCU raises TILER_OOM, the kernel grows the heap
	 * (heap 0: 2 chunks of max 3) and the CS resumes. */
	{
		/* heap 0: 2 chunks of max 3, target_in_flight 2 */
		const uint64_t ins[] = {
			cs_mov48(10, heap0.tiler_heap_ctx_gpu_va), cs_heap_set(10), CS_RUN_IDVS,
			cs_mov48(0, DATA_VA + 0xa00), cs_mov48(2, 1), cs_sync_add64(0, 2),
		};
		uint64_t pa;

		CHECK_EQ(group_create(&e, w.vm, 1, 0x1000, &grp), PW_OK);
		so = syncobj_new(&e);
		sz = put_cs(&e, w.stream, 0x200, ins, 6);
		mock_csf_inject_tiler_oom(e.os, 1, 1, 0, 0);
		op = sig_op(so, 0);
		CHECK_EQ(submit(&e, grp, 0, STREAM_VA + 0x200, sz, &op, 1), PW_OK);
		CHECK_EQ(so_wait(&e, so, 0, 2000), PW_OK);
		CHECK_EQ(fence_err(&e, so, 0, &s), 0);
		CHECK_EQ(mock_csf_oom_handled(e.os, &heap_start, &heap_end), 1);
		CHECK_EQ(heap_start, heap_end);
		CHECK_EQ(heap_start & 0xfff, 0x20000 >> 12);
		CHECK(heap_start >= ks && heap_start < ke);
		CHECK_EQ(mock_gpu_translate(e.os, (unsigned int)as, heap_start & ~0xfffull, 'w',
					    &pa, NULL, NULL), 0);
		CHECK_EQ(mock_gpu_read(e.os, (unsigned int)as, heap_start & ~0xfffull, hdr, 16),
			 0);
		CHECK_EQ(hdr[0], 0);	/* grown chunks are not linked by the kernel */
		CHECK_EQ(bo_r64(&e, w.data, 0xa00), 1);

		/* max_chunks reached: ENOMEM -> heap_start = 0, the FW copes, the job ends. */
		mock_csf_inject_tiler_oom(e.os, 1, 1, 0, 0);
		CHECK_EQ(submit(&e, grp, 0, STREAM_VA + 0x200, sz, &op, 1), PW_OK);
		CHECK_EQ(so_wait(&e, so, 0, 2000), PW_OK);
		CHECK_EQ(mock_csf_oom_handled(e.os, &heap_start, &heap_end), 2);
		CHECK_EQ(heap_start, 0);
		CHECK_EQ(group_state(&e, grp, NULL), 0);
		CHECK_EQ(bo_r64(&e, w.data, 0xa00), 2);

		/* Inconsistent counters (vt_end >= vt_start): the queue is failed. */
		mock_csf_inject_tiler_oom(e.os, 1, 1, 1, 0);
		CHECK_EQ(submit(&e, grp, 0, STREAM_VA + 0x200, sz, &op, 1), PW_OK);
		CHECK_EQ(so_wait(&e, so, 0, 2000), PW_OK);
		CHECK_EQ(fence_err(&e, so, 0, &s), PW_EINVAL);
		CHECK_EQ(group_state(&e, grp, NULL), DRM_PANTHOR_GROUP_STATE_FATAL_FAULT);
		CHECK(mock_log_contains(e.os, "Failed to extend the tiler heap"));
	}
	hd.handle = (w.vm << 16) | 0;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY, &hd), PW_OK);
	/* Heaps 1 (and anything else) go with the file. */
#undef HC
out:
	if (e.os)
		senv_fini(&e);
}

/* Close the file with jobs in flight, pending, and cross-group waits: everything is signalled
 * and freed (check_no_leaks in senv_fini), the CSGs are terminated. */
static void test_sched_teardown_inflight(void)
{
	struct senv e;
	struct world w;
	uint32_t g1, g2, a, b, c, sz, sz2;
	struct pw_fence *fa = NULL, *fb = NULL, *fc = NULL;
	struct drm_panthor_sync_op ops[2];

	if (!senv_init(&e))
		goto out;
	world_init(&e, &w);
	CHECK_EQ(group_create(&e, w.vm, 2, 0x1000, &g1), PW_OK);
	CHECK_EQ(group_create(&e, w.vm, 1, 0x1000, &g2), PW_OK);
	a = syncobj_new(&e);
	b = syncobj_new(&e);
	c = syncobj_new(&e);
	{
		struct drm_panthor_tiler_heap_create hc = {
			.vm_id = w.vm, .initial_chunk_count = 1, .chunk_size = 0x20000,
			.max_chunks = 2, .target_in_flight = 1,
		};

		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &hc), PW_OK);
	}
	sz = cs_wait_stream(&e, &w, 0, 0xb00, 0xb08);
	sz2 = cs_add_stream(&e, &w, 0x40, 0xb10, 1);
	ops[0] = sig_op(a, 0);
	CHECK_EQ(submit(&e, g1, 0, STREAM_VA, sz, ops, 1), PW_OK);	/* blocked */
	ops[0] = sig_op(b, 0);
	CHECK_EQ(submit(&e, g1, 0, STREAM_VA + 0x40, sz2, ops, 1), PW_OK);	/* behind it */
	ops[0] = wait_op(a, 0);
	ops[1] = sig_op(c, 0);
	CHECK_EQ(submit(&e, g2, 0, STREAM_VA + 0x40, sz2, ops, 2), PW_OK);	/* waits for a */
	CHECK_EQ(pw_syncobj_find_fence(e.file, a, 0, 0, 0, &fa), PW_OK);
	CHECK_EQ(pw_syncobj_find_fence(e.file, b, 0, 0, 0, &fb), PW_OK);
	CHECK_EQ(pw_syncobj_find_fence(e.file, c, 0, 0, 0, &fc), PW_OK);
	usleep(20 * 1000);
	CHECK(!pw_fence_is_signalled(fa));
	CHECK(mock_csf_csg_running(e.os, 0, NULL));

	pw_file_close(e.file);
	e.file = NULL;
	CHECK(pw_fence_is_signalled(fa));
	CHECK(pw_fence_is_signalled(fb));
	CHECK(pw_fence_is_signalled(fc));
	CHECK_EQ(pw_fence_error(fa), PW_ECANCELED);
	CHECK_EQ(pw_fence_error(fb), PW_ECANCELED);
	CHECK_EQ(pw_fence_error(fc), PW_ECANCELED);
	CHECK(!mock_csf_csg_running(e.os, 0, NULL));
	CHECK(!mock_csf_csg_running(e.os, 1, NULL));
	pw_fence_put(fa);
	pw_fence_put(fb);
	pw_fence_put(fc);
out:
	if (e.os)
		senv_fini(&e);
}

/* Concurrent submitters, each on its own group and timeline (racing destroy: see
 * test_sched_race_submit_destroy). */
struct thr_arg {
	struct senv *e;
	struct world *w;
	uint32_t grp, tl, idx;
	int fails;
};

static void *submit_thread(void *p)
{
	struct thr_arg *a = p;

	for (uint32_t i = 0; i < 100; i++) {
		struct drm_panthor_sync_op op = sig_op(a->tl, i + 1);

		if (submit(a->e, a->grp, 0, STREAM_VA + a->idx * 0x40, 32, &op, 1))
			a->fails++;
	}
	return NULL;
}

static void test_sched_threads(void)
{
	enum { NT = 4 };
	struct senv e;
	struct world w;
	struct thr_arg args[NT];
	pthread_t thr[NT];

	if (!senv_init(&e))
		goto out;
	world_init(&e, &w);
	for (uint32_t t = 0; t < NT; t++) {
		args[t] = (struct thr_arg){ .e = &e, .w = &w, .idx = t };
		CHECK_EQ(group_create(&e, w.vm, 1, 0x1000, &args[t].grp), PW_OK);
		args[t].tl = syncobj_new(&e);
		cs_add_stream(&e, &w, t * 0x40, t * 8, 1);
	}
	for (uint32_t t = 0; t < NT; t++)
		pthread_create(&thr[t], NULL, submit_thread, &args[t]);
	for (uint32_t t = 0; t < NT; t++)
		pthread_join(thr[t], NULL);
	for (uint32_t t = 0; t < NT; t++) {
		CHECK_EQ(args[t].fails, 0);
		CHECK_EQ(so_wait(&e, args[t].tl, 100, 5000), PW_OK);
		CHECK_EQ(bo_r64(&e, w.data, t * 8), 100);
	}
out:
	if (e.os)
		senv_fini(&e);
}

/* ---- reviewer additions: concurrency, error paths, teardown ---- */

/* Closing one file must not wait for another file's in-flight jobs. pw_sched_file_fini() used
 * to flush the device-wide work item, which re-arms itself while any queue of any file has a
 * job in flight: the close blocked until the other file's jobs finished or timed out. */
static void test_sched_close_other_busy(void)
{
	struct senv e;
	struct world wa, wb;
	struct pw_file *fa, *fb = NULL;
	uint32_t ga, gb, sa, sb, sz;
	struct drm_panthor_sync_op op;
	uint64_t t0, dt;
	bool s;

	if (!senv_init(&e))
		goto out;
	fa = e.file;
	CHECK_EQ(pw_file_open(e.dev, &fb), PW_OK);
	REQUIRE(fb);

	/* File B: a job blocked on memory (default 5 s job timeout). */
	e.file = fb;
	world_init(&e, &wb);
	CHECK_EQ(group_create(&e, wb.vm, 1, 0x1000, &gb), PW_OK);
	sb = syncobj_new(&e);
	sz = cs_wait_stream(&e, &wb, 0, 0x100, 0x108);
	op = sig_op(sb, 0);
	CHECK_EQ(submit(&e, gb, 0, STREAM_VA, sz, &op, 1), PW_OK);
	usleep(20 * 1000);
	CHECK(fence_pending(&e, sb, 0));

	/* File A: a group that ran a job, then the file is closed. */
	e.file = fa;
	world_init(&e, &wa);
	CHECK_EQ(group_create(&e, wa.vm, 1, 0x1000, &ga), PW_OK);
	sa = syncobj_new(&e);
	sz = cs_add_stream(&e, &wa, 0, 0x200, 3);
	op = sig_op(sa, 0);
	CHECK_EQ(submit(&e, ga, 0, STREAM_VA, sz, &op, 1), PW_OK);
	CHECK_EQ(so_wait(&e, sa, 0, 2000), PW_OK);
	t0 = now_ms();
	pw_file_close(fa);
	dt = now_ms() - t0;
	CHECK(dt < 1000);

	/* B is unaffected and still completes normally. */
	e.file = fb;
	CHECK(fence_pending(&e, sb, 0));
	bo_w32(&e, wb.data, 0x100, 1);
	mock_csf_kick(e.os);
	CHECK_EQ(so_wait(&e, sb, 0, 2000), PW_OK);
	CHECK_EQ(fence_err(&e, sb, 0, &s), 0);
	CHECK_EQ(bo_r64(&e, wb.data, 0x108), 1);
out:
	if (e.os)
		senv_fini(&e);
}

/* A job whose in-fence callback is armed is cancelled by GROUP_DESTROY; the in-fence signals
 * afterwards. The callback must have been removed (ASan catches a use after free). */
static void test_sched_destroy_armed_dep(void)
{
	struct senv e;
	struct world w;
	uint32_t g1, g2, a, b, sz1, sz2;
	struct drm_panthor_sync_op ops[2];
	struct pw_fence *f1 = NULL, *f2 = NULL;
	bool s;

	if (!senv_init(&e))
		goto out;
	world_init(&e, &w);
	CHECK_EQ(group_create(&e, w.vm, 1, 0x1000, &g1), PW_OK);
	CHECK_EQ(group_create(&e, w.vm, 2, 0x1000, &g2), PW_OK);
	a = syncobj_new(&e);
	b = syncobj_new(&e);
	sz1 = cs_wait_stream(&e, &w, 0, 0xe00, 0xe08);
	sz2 = cs_add_stream(&e, &w, 0x40, 0xe10, 1);
	ops[0] = sig_op(a, 0);
	CHECK_EQ(submit(&e, g1, 0, STREAM_VA, sz1, ops, 1), PW_OK);
	/* g2: two jobs on queue 1 waiting for a (the second one behind the first). */
	ops[0] = wait_op(a, 0);
	ops[1] = sig_op(b, 0);
	CHECK_EQ(submit(&e, g2, 1, STREAM_VA + 0x40, sz2, ops, 2), PW_OK);
	CHECK_EQ(pw_syncobj_find_fence(e.file, b, 0, 0, 0, &f1), PW_OK);
	CHECK_EQ(submit(&e, g2, 1, STREAM_VA + 0x40, sz2, ops, 2), PW_OK);
	CHECK_EQ(pw_syncobj_find_fence(e.file, b, 0, 0, 0, &f2), PW_OK);
	REQUIRE(f1 && f2 && f1 != f2);
	usleep(20 * 1000);
	CHECK(!pw_fence_is_signalled(f1));
	CHECK_EQ(group_destroy(&e, g2), PW_OK);
	CHECK(pw_fence_is_signalled(f1));
	CHECK(pw_fence_is_signalled(f2));
	CHECK_EQ(pw_fence_error(f1), PW_ECANCELED);
	CHECK_EQ(pw_fence_error(f2), PW_ECANCELED);
	pw_fence_put(f1);
	pw_fence_put(f2);
	/* Now release g1: a signals with no live waiter. */
	bo_w32(&e, w.data, 0xe00, 1);
	mock_csf_kick(e.os);
	CHECK_EQ(so_wait(&e, a, 0, 2000), PW_OK);
	CHECK_EQ(fence_err(&e, a, 0, &s), 0);
	CHECK_EQ(bo_r64(&e, w.data, 0xe10), 0);
	CHECK_EQ(group_destroy(&e, g2), PW_EINVAL);
out:
	if (e.os)
		senv_fini(&e);
}

/* Several threads submit to the same queue (ring wraps, ring-full waits, contention on the
 * queue lock) and to a second queue of the same group. */
struct sq_arg {
	struct senv *e;
	uint32_t grp, queue, size, tl;
	uint64_t va;
	int fails;
};

static void *same_queue_thread(void *p)
{
	struct sq_arg *a = p;

	for (uint32_t i = 0; i < 150; i++) {
		struct drm_panthor_sync_op op = sig_op(a->tl, i + 1);

		if (submit(a->e, a->grp, a->queue, a->va, a->size, &op, 1))
			a->fails++;
	}
	return NULL;
}

static void test_sched_same_queue_threads(void)
{
	enum { NT = 4 };
	struct senv e;
	struct world w;
	struct sq_arg args[NT];
	pthread_t thr[NT];
	uint32_t grp, sz0, sz1;

	if (!senv_init(&e))
		goto out;
	world_init(&e, &w);
	CHECK_EQ(group_create(&e, w.vm, 2, 0x1000, &grp), PW_OK);
	sz0 = cs_add_stream(&e, &w, 0, 0xf00, 1);
	sz1 = cs_add_stream(&e, &w, 0x40, 0xf08, 1);
	for (uint32_t t = 0; t < NT; t++) {
		args[t] = (struct sq_arg){ .e = &e, .grp = grp, .queue = t & 1,
					   .size = (t & 1) ? sz1 : sz0,
					   .va = STREAM_VA + ((t & 1) ? 0x40 : 0) };
		args[t].tl = syncobj_new(&e);
	}
	for (uint32_t t = 0; t < NT; t++)
		pthread_create(&thr[t], NULL, same_queue_thread, &args[t]);
	for (uint32_t t = 0; t < NT; t++)
		pthread_join(thr[t], NULL);
	for (uint32_t t = 0; t < NT; t++) {
		bool s;

		CHECK_EQ(args[t].fails, 0);
		CHECK_EQ(so_wait(&e, args[t].tl, 150, 5000), PW_OK);
		CHECK_EQ(fence_err(&e, args[t].tl, 150, &s), 0);
	}
	CHECK_EQ(bo_r64(&e, w.data, 0xf00), 2 * 150);
	CHECK_EQ(bo_r64(&e, w.data, 0xf08), 2 * 150);
	CHECK_EQ(group_state(&e, grp, NULL), 0);
out:
	if (e.os)
		senv_fini(&e);
}

/* Submitters race with GROUP_DESTROY of their group: every submit either fails with EINVAL or
 * publishes a fence that gets signalled (0 or ECANCELED); nothing hangs or leaks. */
#define RD_SUBMITS 400
struct rd_arg {
	struct senv *e;
	uint32_t grp, so, size;
	uint32_t nf;			/* fences published (binary syncobj @so, one per submit) */
	struct pw_fence *f[RD_SUBMITS];
	int bad_err;
};

static void *race_destroy_thread(void *p)
{
	struct rd_arg *a = p;

	for (uint32_t i = 0; i < RD_SUBMITS; i++) {
		struct drm_panthor_sync_op op = sig_op(a->so, 0);
		int ret = submit(a->e, a->grp, i & 1, STREAM_VA, a->size, &op, 1);

		if (ret == PW_OK) {
			/* Only this thread uses @so: the fence is the one just published. */
			if (pw_syncobj_find_fence(a->e->file, a->so, 0, 0, 0, &a->f[a->nf]))
				a->bad_err++;
			else
				a->nf++;
		} else if (ret != PW_EINVAL) {
			a->bad_err++;
		}
	}
	return NULL;
}

static void test_sched_race_submit_destroy(void)
{
	enum { NT = 3, ROUNDS = 6 };
	static struct rd_arg args[NT];
	struct senv e;
	struct world w;
	uint32_t sz;

	if (!senv_init(&e))
		goto out;
	world_init(&e, &w);
	sz = cs_add_stream(&e, &w, 0, 0xa00, 1);
	for (uint32_t r = 0; r < ROUNDS; r++) {
		pthread_t thr[NT];
		uint32_t grp;

		CHECK_EQ(group_create(&e, w.vm, 2, 0x1000, &grp), PW_OK);
		for (uint32_t t = 0; t < NT; t++) {
			args[t] = (struct rd_arg){ .e = &e, .grp = grp, .size = sz };
			args[t].so = syncobj_new(&e);
		}
		for (uint32_t t = 0; t < NT; t++)
			pthread_create(&thr[t], NULL, race_destroy_thread, &args[t]);
		usleep((r + 1) * 1000);
		CHECK_EQ(group_destroy(&e, grp), PW_OK);
		for (uint32_t t = 0; t < NT; t++)
			pthread_join(thr[t], NULL);
		for (uint32_t t = 0; t < NT; t++) {
			CHECK_EQ(args[t].bad_err, 0);
			for (uint32_t i = 0; i < args[t].nf; i++) {
				struct pw_fence *f = args[t].f[i];

				CHECK_EQ(pw_fence_wait(f, pw_os_time_ns(e.os) + 2000000000ull),
					 PW_OK);
				CHECK(pw_fence_error(f) == 0 || pw_fence_error(f) == PW_ECANCELED);
				pw_fence_put(f);
			}
		}
	}
out:
	if (e.os)
		senv_fini(&e);
}

/* VM_DESTROY under a live group, heap ioctls on a destroyed VM, then teardown. */
static void test_sched_vm_destroy_live(void)
{
	struct senv e;
	struct world w;
	uint32_t grp, a, sz;
	struct drm_panthor_sync_op op;
	struct drm_panthor_tiler_heap_create hc = {
		.initial_chunk_count = 1, .chunk_size = 0x20000, .max_chunks = 2,
		.target_in_flight = 1,
	};
	struct drm_panthor_tiler_heap_destroy hd = { 0 };
	struct drm_panthor_vm_destroy vd = { 0 };
	bool s;

	if (!senv_init(&e))
		goto out;
	pw_sched_set_job_timeout(e.dev, 200);
	world_init(&e, &w);
	CHECK_EQ(group_create(&e, w.vm, 1, 0x1000, &grp), PW_OK);
	hc.vm_id = w.vm;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &hc), PW_OK);
	hd.handle = hc.handle;
	a = syncobj_new(&e);
	sz = cs_add_stream(&e, &w, 0, 0xb80, 1);
	op = sig_op(a, 1);
	CHECK_EQ(submit(&e, grp, 0, STREAM_VA, sz, &op, 1), PW_OK);
	CHECK_EQ(so_wait(&e, a, 1, 2000), PW_OK);

	vd.id = w.vm;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_DESTROY, &vd), PW_OK);
	/* The heap's VM id no longer resolves (drv.c:1312). */
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY, &hd), PW_EINVAL);
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &hc), PW_EINVAL);
	/* A submit on the group whose VM is gone: accepted or refused, but its fence (if any)
	 * must signal. */
	op = sig_op(a, 2);
	if (submit(&e, grp, 0, STREAM_VA, sz, &op, 1) == PW_OK) {
		CHECK_EQ(so_wait(&e, a, 2, 3000), PW_OK);
		(void)fence_err(&e, a, 2, &s);
		CHECK(s);
	}
	CHECK_EQ(group_destroy(&e, grp), PW_OK);
out:
	if (e.os)
		senv_fini(&e);
}

const struct test_case sched_tests[] = {
	{ "sched_group_create_destroy", test_sched_group_create_destroy },
	{ "sched_submit_basic", test_sched_submit_basic },
	{ "sched_ring_wrap", test_sched_ring_wrap },
	{ "sched_sync_only", test_sched_sync_only },
	{ "sched_insync_deps", test_sched_insync_deps },
	{ "sched_user_stream", test_sched_user_stream },
	{ "sched_cs_fatal", test_sched_cs_fatal },
	{ "sched_timeout", test_sched_timeout },
	{ "sched_vm_fault", test_sched_vm_fault },
	{ "sched_slots", test_sched_slots },
	{ "sched_heap", test_sched_heap },
	{ "sched_teardown_inflight", test_sched_teardown_inflight },
	{ "sched_threads", test_sched_threads },
	{ "sched_close_other_busy", test_sched_close_other_busy },
	{ "sched_destroy_armed_dep", test_sched_destroy_armed_dep },
	{ "sched_same_queue_threads", test_sched_same_queue_threads },
	{ "sched_race_submit_destroy", test_sched_race_submit_destroy },
	{ "sched_vm_destroy_live", test_sched_vm_destroy_live },
	{ NULL, NULL },
};
