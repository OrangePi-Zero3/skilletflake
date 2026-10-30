/* SPDX-License-Identifier: MIT */
/*
 * test_libdrm_core.c - libdrm-win against the REAL kmd/core (transport "core": core + mock G610
 * booting the real mali_csffw.bin, in-process). Run under wine with PANWIN_TRANSPORT=core.
 *
 * Covers the libdrm-win <-> core contract end to end, through the same ioctl encoding as
 * panwin.sys: VERSION/GET_CAP/DEV_QUERY, BO create + mmap (user mapping aliases the GPU pages:
 * a CPU write is read by a command stream, and a GPU store is read back through the mapping),
 * VM_BIND, GROUP_CREATE/SUBMIT/DESTROY with syncobj signalling, binary + timeline syncobj
 * waits, SYNCOBJ_EVENT, the read-only LATEST_FLUSH_ID page, errno propagation, and a clean
 * (leak-free) teardown of the device after the last close.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "xf86drm.h"
#include "libdrm_win.h"
#include "panwin_core.h"
#include "panwin_transport.h"
#include "drm.h"
#include "panthor_drm.h"
#include "panwin/panwin_ioctl.h"

static int failures, checks;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: CHECK failed: %s (errno %d)\n", __FILE__, __LINE__, #c, errno); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); checks++; \
	if (_a != _b) { failures++; fprintf(stderr, "%s:%d: %s == %lld, expected %s == %lld (errno %d)\n", \
	__FILE__, __LINE__, #a, _a, #b, _b, errno); } } while (0)

/* ---- CS encodings (Mesa src/panfrost/genxml/v10.xml, cs_builder.h) ---- */
#define OP(o) ((uint64_t)(o) << 56)
static uint64_t cs_mov48(uint32_t r, uint64_t v) { return OP(1) | ((uint64_t)r << 48) | (v & 0xffffffffffffull); }
static uint64_t cs_add_imm32(uint32_t d, uint32_t s, int32_t imm)
{
	return OP(16) | ((uint64_t)d << 48) | ((uint64_t)s << 40) | (uint32_t)imm;
}
static uint64_t cs_ldst(uint32_t op, uint32_t base, uint32_t addr, uint16_t mask, int16_t off)
{
	return OP(op) | ((uint64_t)base << 48) | ((uint64_t)addr << 40) | ((uint64_t)mask << 16) |
	       (uint16_t)off;
}
#define CS_LOAD_MULTIPLE 20
#define CS_STORE_MULTIPLE 21
#define CS_RUN_COMPUTE OP(4)

static int64_t now_ns(void)
{
	LARGE_INTEGER f, c;
	int64_t sec;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	sec = c.QuadPart / f.QuadPart;
	return sec * 1000000000 + (c.QuadPart - sec * f.QuadPart) * 1000000000 / f.QuadPart;
}

struct sig_ctx {
	int fd;
	uint32_t h;
	uint64_t point;
	int ret;
};

static DWORD WINAPI signal_later(void *p)
{
	struct sig_ctx *c = p;

	Sleep(50);
	c->ret = drmSyncobjTimelineSignal(c->fd, &c->h, &c->point, 1);
	return 0;
}

static panwin_core_get_report_fn get_report;

static void report(struct panwin_core_report *r)
{
	memset(r, 0, sizeof(*r));
	r->version = PANWIN_CORE_REPORT_VERSION;
	CHECK_EQ(get_report(r), 0);
}

#define BUF 0x10000u
#define STREAM_VA 0x1000000ull
#define DATA_VA 0x2000000ull

int main(void)
{
	struct panwin_core_report r0, r;
	drmVersionPtr v;
	uint64_t cap = 0;
	int fd, fd2;

	setvbuf(stdout, NULL, _IONBF, 0);
	get_report = (panwin_core_get_report_fn)(void (*)(void))GetProcAddress(
		GetModuleHandleA("libdrm.dll"), PANWIN_CORE_GET_REPORT);
	if (!get_report) {
		fprintf(stderr, "libdrm.dll has no core transport (build with -Dcore_transport=true)\n");
		return 1;
	}
	if (strcmp(panwin_transport_name(), "core")) {
		fprintf(stderr, "PANWIN_TRANSPORT=core expected, transport is %s\n",
			panwin_transport_name());
		return 1;
	}
	report(&r0);
	CHECK_EQ(r0.device_live, 0);

	/* ---- open: brings up mock + core + FW ---- */
	fd = panwin_open("/dev/dri/renderD128", 0);
	CHECK(fd >= 0);
	if (fd < 0)
		return 1;
	report(&r);
	CHECK_EQ(r.device_live, 1);
	CHECK_EQ(r.devices_created, r0.devices_created + 1);

	v = drmGetVersion(fd);
	CHECK(v && !strcmp(v->name, "panthor"));
	if (v)
		printf("DRM %s %d.%d.%d (%s)\n", v->name, v->version_major, v->version_minor,
		       v->version_patchlevel, v->desc);
	drmFreeVersion(v);
	CHECK_EQ(drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap), 0);
	CHECK_EQ(cap, 1);

	struct drm_panthor_gpu_info gi = { 0 };
	struct drm_panthor_dev_query q = { .type = DRM_PANTHOR_DEV_QUERY_GPU_INFO,
					   .size = sizeof(gi), .pointer = (uintptr_t)&gi };
	CHECK_EQ(drmIoctl(fd, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), 0);
	CHECK_EQ(gi.gpu_id >> 16, 0xa867);
	printf("GPU_ID 0x%08x, shader_present 0x%llx\n", gi.gpu_id,
	       (unsigned long long)gi.shader_present);

	/* errno propagation: unknown handle -> ENOENT, through PANWIN_STATUS_FROM_ERRNO */
	struct drm_gem_close gc = { .handle = 4242 };
	CHECK_EQ(drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &gc), -1);
	CHECK(errno == EINVAL || errno == ENOENT);

	/* ---- the user MMIO (LATEST_FLUSH_ID) page ---- */
	struct drm_panthor_set_user_mmio_offset mo = { .offset = DRM_PANTHOR_USER_MMIO_OFFSET };
	CHECK_EQ(drmIoctl(fd, DRM_IOCTL_PANTHOR_SET_USER_MMIO_OFFSET, &mo), 0);
	void *flush = panwin_mmap(fd, DRM_PANTHOR_USER_MMIO_OFFSET, 4096, PANWIN_PROT_READ);
	CHECK(flush != NULL);
	CHECK(panwin_mmap(fd, DRM_PANTHOR_USER_MMIO_OFFSET, 4096,
			  PANWIN_PROT_READ | PANWIN_PROT_WRITE) == NULL && errno == EINVAL);
	if (flush) {
		MEMORY_BASIC_INFORMATION mbi;

		CHECK(VirtualQuery(flush, &mbi, sizeof(mbi)) && mbi.Protect == PAGE_READONLY);
		CHECK_EQ(panwin_munmap(flush, 4096), 0);
	}

	/* ---- BOs, mappings ---- */
	struct drm_panthor_bo_create bs = { .size = BUF }, bd = { .size = BUF };
	CHECK_EQ(drmIoctl(fd, DRM_IOCTL_PANTHOR_BO_CREATE, &bs), 0);
	CHECK_EQ(drmIoctl(fd, DRM_IOCTL_PANTHOR_BO_CREATE, &bd), 0);
	struct drm_panthor_bo_mmap_offset ms = { .handle = bs.handle }, md = { .handle = bd.handle };
	CHECK_EQ(drmIoctl(fd, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &ms), 0);
	CHECK_EQ(drmIoctl(fd, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &md), 0);
	uint64_t *stream = panwin_mmap(fd, ms.offset, BUF, PANWIN_PROT_READ | PANWIN_PROT_WRITE);
	uint32_t *data = panwin_mmap(fd, md.offset, BUF, PANWIN_PROT_READ | PANWIN_PROT_WRITE);
	uint32_t *data2 = panwin_mmap(fd, md.offset, BUF, PANWIN_PROT_READ);
	CHECK(stream && data && data2);
	if (!stream || !data || !data2)
		return 1;
	/* new BOs read as zero (Linux shmem semantics; PW_MEM_ZEROED) */
	CHECK_EQ(data[0] | data[BUF / 4 - 1] | (uint32_t)stream[0], 0);
	/* two mappings: distinct VAs, same pages */
	CHECK(data2 != data);
	data[7] = 0xabcd1234u;
	CHECK_EQ(data2[7], 0xabcd1234u);

	/* ---- VM + bind ---- */
	struct drm_panthor_vm_create vc = { 0 };
	CHECK_EQ(drmIoctl(fd, DRM_IOCTL_PANTHOR_VM_CREATE, &vc), 0);
	struct drm_panthor_vm_bind_op ops[2] = {
		{ .flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, .bo_handle = bs.handle, .va = STREAM_VA, .size = BUF },
		{ .flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, .bo_handle = bd.handle, .va = DATA_VA, .size = BUF },
	};
	struct drm_panthor_vm_bind vb = { .vm_id = vc.id, .ops = DRM_PANTHOR_OBJ_ARRAY(2, ops) };
	CHECK_EQ(drmIoctl(fd, DRM_IOCTL_PANTHOR_VM_BIND, &vb), 0);

	/* ---- group ---- */
	struct drm_panthor_csif_info ci = { 0 };
	q = (struct drm_panthor_dev_query){ .type = DRM_PANTHOR_DEV_QUERY_CSIF_INFO,
					   .size = sizeof(ci), .pointer = (uintptr_t)&ci };
	CHECK_EQ(drmIoctl(fd, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), 0);
	struct drm_panthor_queue_create qc[3] = {
		{ .priority = 0, .ringbuf_size = 0x10000 },
		{ .priority = 0, .ringbuf_size = 0x10000 },
		{ .priority = 0, .ringbuf_size = 0x10000 },
	};
	struct drm_panthor_group_create gcr = {
		.queues = DRM_PANTHOR_OBJ_ARRAY(3, qc), .max_compute_cores = 1,
		.max_fragment_cores = 1, .max_tiler_cores = 1,
		.priority = PANTHOR_GROUP_PRIORITY_MEDIUM, .compute_core_mask = gi.shader_present,
		.fragment_core_mask = gi.shader_present, .tiler_core_mask = 1, .vm_id = vc.id,
	};
	CHECK_EQ(drmIoctl(fd, DRM_IOCTL_PANTHOR_GROUP_CREATE, &gcr), 0);

	/* CPU write -> CS reads it through the GPU MMU -> CS stores value+1 -> CPU reads it. */
	data[0] = 0x12345678u;
	uint64_t cs[] = {
		cs_mov48(0, DATA_VA), cs_ldst(CS_LOAD_MULTIPLE, 10, 0, 1, 0),
		cs_add_imm32(10, 10, 1), cs_ldst(CS_STORE_MULTIPLE, 10, 0, 1, 0x100),
		CS_RUN_COMPUTE,
	};
	memcpy(stream, cs, sizeof(cs));

	uint32_t so = 0, tl = 0;
	CHECK_EQ(drmSyncobjCreate(fd, 0, &so), 0);
	CHECK_EQ(drmSyncobjCreate(fd, 0, &tl), 0);
	struct drm_panthor_sync_op sync[2] = {
		{ .flags = DRM_PANTHOR_SYNC_OP_SIGNAL | DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ, .handle = so },
		{ .flags = DRM_PANTHOR_SYNC_OP_SIGNAL | DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ,
		  .handle = tl, .timeline_value = 5 },
	};
	struct drm_panthor_queue_submit qs = {
		.queue_index = 1, .stream_size = sizeof(cs), .stream_addr = STREAM_VA,
		.syncs = DRM_PANTHOR_OBJ_ARRAY(2, sync),
	};
	struct drm_panthor_group_submit gs = { .group_handle = gcr.group_handle,
					       .queue_submits = DRM_PANTHOR_OBJ_ARRAY(1, &qs) };
	CHECK_EQ(drmIoctl(fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gs), 0);

	/* SYNCOBJ_EVENT on the timeline point (WAIT_FOR_SUBMIT not needed: already submitted) */
	void *ev = NULL;
	CHECK_EQ(panwin_syncobj_event(fd, tl, 5, 0, &ev), 0);
	CHECK_EQ(drmSyncobjWait(fd, &so, 1, now_ns() + 5000000000ll,
				DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL), 0);
	uint64_t pt = 5;
	CHECK_EQ(drmSyncobjTimelineWait(fd, &tl, &pt, 1, now_ns() + 5000000000ll, 0, NULL), 0);
	if (ev) {
		CHECK_EQ(WaitForSingleObject(ev, 5000), WAIT_OBJECT_0);
		CloseHandle(ev);
	}
	CHECK_EQ(data[0x100 / 4], 0x12345679u);
	CHECK_EQ(data2[0x100 / 4], 0x12345679u);
	uint64_t val = 0;
	CHECK_EQ(drmSyncobjQuery(fd, &tl, &val, 1), 0);
	CHECK_EQ(val, 5);

	struct drm_panthor_group_get_state gst = { .group_handle = gcr.group_handle };
	CHECK_EQ(drmIoctl(fd, DRM_IOCTL_PANTHOR_GROUP_GET_STATE, &gst), 0);
	CHECK_EQ(gst.state, 0);

	/* A wait that must time out (ETIME, not ETIMEDOUT) on an unsignalled point. */
	pt = 6;
	int64_t t0 = now_ns();
	CHECK_EQ(drmSyncobjTimelineWait(fd, &tl, &pt, 1, t0 + 50000000ll,
					DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL), -ETIME);
	CHECK(now_ns() - t0 >= 45000000ll);
	/* host signal of the point -> wait succeeds */
	CHECK_EQ(drmSyncobjTimelineSignal(fd, &tl, &pt, 1), 0);
	CHECK_EQ(drmSyncobjTimelineWait(fd, &tl, &pt, 1, now_ns() + 1000000000ll, 0, NULL), 0);

	/* "Infinite" (INT64_MAX, what Mesa passes for UINT64_MAX timeouts) wait, signalled from
	 * another thread 50 ms later: exercises the far-deadline path of the user wait. */
	{
		struct sig_ctx c = { .fd = fd, .h = tl, .point = 7 };
		HANDLE th = CreateThread(NULL, 0, signal_later, &c, 0, NULL);

		pt = 7;
		t0 = now_ns();
		CHECK_EQ(drmSyncobjTimelineWait(fd, &tl, &pt, 1, INT64_MAX,
						DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL), 0);
		CHECK(now_ns() - t0 >= 40000000ll);
		WaitForSingleObject(th, INFINITE);
		CloseHandle(th);
		CHECK_EQ(c.ret, 0);
	}

	report(&r);
	printf("CS executed: %llu instrs, %llu RUN_*, %u fatal\n", (unsigned long long)r.cs_instrs,
	       (unsigned long long)r.cs_runs, r.cs_fatal);
	CHECK(r.cs_instrs >= 5);
	CHECK(r.cs_runs >= 1);
	CHECK_EQ(r.cs_fatal, 0);

	/* ---- teardown: leave a mapping and a pending event behind on purpose on a dup'ed fd;
	 * the mapping outlives the fds (Linux semantics) and everything goes at the end ---- */
	struct drm_panthor_group_destroy gd = { .group_handle = gcr.group_handle };
	CHECK_EQ(drmIoctl(fd, DRM_IOCTL_PANTHOR_GROUP_DESTROY, &gd), 0);
	struct drm_panthor_vm_destroy vd = { .id = vc.id };
	CHECK_EQ(drmIoctl(fd, DRM_IOCTL_PANTHOR_VM_DESTROY, &vd), 0);
	CHECK_EQ(drmSyncobjDestroy(fd, so), 0);
	pt = 100;
	CHECK_EQ(panwin_syncobj_event(fd, tl, pt, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, &ev), 0);
	fd2 = panwin_dup(fd);
	CHECK(fd2 >= 0);
	CHECK_EQ(panwin_close(fd), 0);
	gc.handle = bs.handle;
	CHECK_EQ(drmIoctl(fd2, DRM_IOCTL_GEM_CLOSE, &gc), 0);
	gc.handle = bd.handle;
	CHECK_EQ(drmIoctl(fd2, DRM_IOCTL_GEM_CLOSE, &gc), 0);
	CHECK_EQ(panwin_close(fd2), 0);
	/* mappings keep the file (and the BOs) alive */
	report(&r);
	CHECK_EQ(r.device_live, 1);
	data[1] = 1;
	CHECK_EQ(data2[1], 1);
	CHECK_EQ(panwin_munmap(stream, BUF), 0);
	CHECK_EQ(panwin_munmap(data, BUF), 0);
	CHECK_EQ(panwin_munmap(data2, BUF), 0);
	CHECK_EQ(WaitForSingleObject(ev, 0), WAIT_TIMEOUT); /* cancelled, never set */
	CloseHandle(ev);

	report(&r);
	CHECK_EQ(r.device_live, 0);
	CHECK_EQ(r.devices_destroyed, r0.devices_destroyed + 1);
	CHECK_EQ(r.leaked_allocs, 0);
	CHECK_EQ(r.leaked_mems, 0);
	CHECK_EQ(r.leaked_sync, 0);
	CHECK_EQ(r.model_errors, 0);
	CHECK_EQ(r.irq_unclaimed, 0);
	CHECK_EQ(r.leaked_maps, 0);
	CHECK_EQ(r.leaked_notifies, 1);
	printf("core report: %llu DRM ioctls (%llu failed), %llu mmaps, %llu munmaps, %llu events\n",
	       (unsigned long long)r.drm_ioctls, (unsigned long long)r.drm_ioctl_errors,
	       (unsigned long long)r.mmaps, (unsigned long long)r.munmaps,
	       (unsigned long long)r.syncobj_events);

	printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "OK", checks, failures);
	return failures ? 1 : 0;
}
