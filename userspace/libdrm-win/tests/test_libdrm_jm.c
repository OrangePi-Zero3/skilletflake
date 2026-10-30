/* SPDX-License-Identifier: MIT */
/*
 * test_libdrm_jm.c - (jm-e2e) the Mali-G31 job-manager stack below Mesa, end to end:
 *   this test -> libdrm.dll (libdrm-win, transport "core") -> the REAL kmd/core (JM backend:
 *   pw_jm_hw / pw_jm_mem / pw_jm_sched) -> the mock G31 (kmd/tests/pw_os_mock.c + mock_jm.c),
 * x86_64 under wine, PANWIN_TRANSPORT=core PANWIN_CORE_MODEL=g31.
 *
 * Mesa cannot drive the G31 on Windows yet (patch 0010 leaves panfrost_kmod.c out of the Windows
 * build; docs/status/jm-arch.md open question 1), so this does what Mesa would, with the same
 * libdrm calls and uAPI structs:
 *  - Mesa src/panfrost/lib/kmod/panfrost_kmod.c: drmGetVersion ("panfrost" picks the panfrost
 *    kmod), every GET_PARAM panfrost_dev_query_props() reads, CREATE_BO (exec / NOEXEC / HEAP |
 *    NOEXEC as to_panfrost_bo_flags() builds them), GET_BO_OFFSET, MMAP_BO + mmap of the offset,
 *    WAIT_BO (bo_wait), MADVISE (make_evictable / make_unevictable), SET_LABEL_BO (bo_label),
 *    drmCloseBufferHandle, the [32 MiB, 4 GiB) VA range it assumes;
 *  - Mesa src/panfrost/vulkan/jm/panvk_vX_gpu_queue.c and src/gallium/drivers/panfrost/pan_jm.c:
 *    SUBMIT with bo_handles, in_syncs and out_sync, REQ_FS on the fragment chain, the
 *    in_sync == out_sync chaining of panvk, JM_CTX_CREATE/DESTROY of Gallium, drmSyncobjWait.
 * The job chains are Bifrost v7 descriptors written by the CPU into BO memory (layouts from Mesa
 * src/panfrost/genxml/v7.xml, MIT: Job Header, Write Value Job, Compute Job). The mock executes
 * WRITE_VALUE / NULL / CACHE_FLUSH and completes COMPUTE / FRAGMENT without shaders, so the data
 * checks are on WRITE_VALUE results and on the descriptors' written-back exception status.
 *
 * Fence errors are invisible through SYNCOBJ_WAIT (as on Linux) and Windows has no sync_file, so
 * the core transport's test hook panwin_core_syncobj_status() reads them (panwin_core.h).
 *
 * Environment: PANWIN_JM_STRESS=<n> submits in the stress step (default 1000, 0 skips it).
 * Hardware-free: nothing here says anything about a real G31 (untested on hardware).
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
#include "panfrost_drm.h"

static int failures, checks;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: CHECK failed: %s (errno %d)\n", __FILE__, __LINE__, #c, errno); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); checks++; \
	if (_a != _b) { failures++; fprintf(stderr, "%s:%d: %s == %lld (0x%llx), expected %s == %lld (0x%llx)\n", \
	__FILE__, __LINE__, #a, _a, (unsigned long long)_a, #b, _b, (unsigned long long)_b); } } while (0)

#define PROT_R 1	/* panwin_mmap prot: PROT_READ | PROT_WRITE values (libdrm_win.h) */
#define PROT_RW 3
#define SZ_1M (1u << 20)
#define SZ_2M (2u << 20)
/* Mesa panfrost_kmod.c PANFROST_KMOD_VA_START / _END */
#define VA_START 0x2000000ull
#define VA_END (1ull << 32)

/* v7.xml "Job Type" / "Write Value Type" */
enum { JT_NOT_STARTED = 0, JT_NULL = 1, JT_WRITE_VALUE = 2, JT_CACHE_FLUSH = 3, JT_COMPUTE = 4,
       JT_FRAGMENT = 9 };
enum { WV_CYCLE_COUNTER = 1, WV_SYSTEM_TIMESTAMP = 2, WV_ZERO = 3, WV_IMM8 = 4, WV_IMM16 = 5,
       WV_IMM32 = 6, WV_IMM64 = 7 };
#define EXC_DONE 0x01u		/* exception status written back by a finished job */
#define EXC_CONFIG_FAULT 0x40u	/* JOB_CONFIG_FAULT */

static panwin_core_get_report_fn get_report;
static panwin_core_syncobj_status_fn syncobj_status;

static void report(struct panwin_core_report *r)
{
	memset(r, 0, sizeof(*r));
	r->version = PANWIN_CORE_REPORT_VERSION;
	CHECK_EQ(get_report(r), 0);
}

/* Mesa's os_time_get_nano() on Windows and the core's pw_os_time_ns(): QPC in ns. */
static int64_t now_ns(void)
{
	LARGE_INTEGER f, c;
	int64_t sec;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	sec = c.QuadPart / f.QuadPart;
	return sec * 1000000000ll + (c.QuadPart - sec * f.QuadPart) * 1000000000ll / f.QuadPart;
}

static int ioc(int fd, unsigned long req, void *arg)
{
	errno = 0;
	return drmIoctl(fd, req, arg) ? errno : 0;
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static uint32_t get32(const volatile uint8_t *p) { uint32_t v; memcpy(&v, (const void *)p, 4); return v; }
static uint64_t get64(const volatile uint8_t *p) { uint64_t v; memcpy(&v, (const void *)p, 8); return v; }

/* ---- params ---- */

static int get_param(int fd, uint32_t param, uint64_t *value)
{
	struct drm_panfrost_get_param gp = { .param = param };
	int e = ioc(fd, DRM_IOCTL_PANFROST_GET_PARAM, &gp);

	if (!e)
		*value = gp.value;
	return e;
}

/* ---- BOs ---- */

struct bo {
	uint32_t handle, size, flags;
	uint64_t va;		/* GPU VA (CREATE_BO offset) */
	uint8_t *cpu;		/* panwin_mmap of MMAP_BO's offset, or NULL */
	uint32_t used;		/* job-descriptor allocator */
};

static int bo_create(int fd, struct bo *b, uint32_t size, uint32_t flags, int map)
{
	struct drm_panfrost_create_bo c = { .size = size, .flags = flags };
	struct drm_panfrost_get_bo_offset o = { 0 };
	int e;

	memset(b, 0, sizeof(*b));
	e = ioc(fd, DRM_IOCTL_PANFROST_CREATE_BO, &c);
	if (e)
		return e;
	b->handle = c.handle;
	b->size = size;
	b->flags = flags;
	b->va = c.offset;
	CHECK(b->va >= VA_START && b->va + size <= VA_END);
	o.handle = b->handle;
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_GET_BO_OFFSET, &o), 0);
	CHECK_EQ(o.offset, b->va);
	if (map) {
		struct drm_panfrost_mmap_bo m = { .handle = b->handle };

		CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_MMAP_BO, &m), 0);
		CHECK(m.offset != 0 && !(m.offset & 4095));
		b->cpu = panwin_mmap(fd, m.offset, size, PROT_RW);
		CHECK(b->cpu != NULL);
		if (!b->cpu)
			return errno ? errno : EINVAL;
	}
	return 0;
}

static void bo_unmap(struct bo *b)
{
	if (b->cpu)
		CHECK_EQ(panwin_munmap(b->cpu, b->size), 0);
	b->cpu = NULL;
}

static void bo_close(int fd, struct bo *b)
{
	if (b->handle)
		CHECK_EQ(drmCloseBufferHandle(fd, b->handle), 0);
	b->handle = 0;
}

static void bo_free(int fd, struct bo *b)
{
	bo_unmap(b);
	bo_close(fd, b);
}

/* ---- v7 job chains, CPU-written into a descriptor BO ---- */

struct chain {
	struct bo *desc;
	uint64_t first;		/* jc */
	uint8_t *last;
	uint32_t n;		/* jobs (= last index; Mesa numbers jobs from 1) */
};

static void chain_init(struct chain *c, struct bo *desc)
{
	memset(c, 0, sizeof(*c));
	c->desc = desc;
}

/*
 * Append a job of @type with @bytes of descriptor (Job Header + payload, 64-byte aligned) that
 * depends on job indices @dep1/@dep2 (0 = none). Job Header (v7.xml): word 0 exception status,
 * 1 first incomplete task, 2-3 fault pointer, 4 = Is 64b (bit 0) | type (bits 1-7) | index
 * (bits 16-31), 5 = dependency 1 | dependency 2 << 16, 6-7 next. Returns the CPU pointer.
 */
static uint8_t *chain_add(struct chain *c, uint32_t type, uint32_t bytes, uint32_t dep1,
			  uint32_t dep2)
{
	struct bo *d = c->desc;
	uint32_t off = (d->used + 63u) & ~63u;
	uint8_t *p;
	uint64_t va;

	bytes = (bytes + 63u) & ~63u;
	if (off + bytes > d->size) {
		fprintf(stderr, "descriptor BO full\n");
		abort();
	}
	d->used = off + bytes;
	p = d->cpu + off;
	va = d->va + off;
	memset(p, 0, bytes);
	c->n++;
	put32(p + 16, 1u | (type << 1) | (c->n << 16));
	put32(p + 20, dep1 | (dep2 << 16));
	if (c->last)
		put64(c->last + 24, va);
	else
		c->first = va;
	c->last = p;
	return p;
}

/* Write Value Job (v7.xml): header, then at 32 address, 40 type, 48 immediate value. */
static uint8_t *chain_wv(struct chain *c, uint64_t addr, uint32_t wv_type, uint64_t imm,
			 uint32_t dep1)
{
	uint8_t *p = chain_add(c, JT_WRITE_VALUE, 64, dep1, 0);

	put64(p + 32, addr);
	put32(p + 40, wv_type);
	put64(p + 48, imm);
	return p;
}

static uint32_t exc_status(const uint8_t *job)
{
	return get32((const volatile uint8_t *)job);
}

/* ---- submit / sync ---- */

struct sub {
	uint64_t jc;
	const uint32_t *bos;
	uint32_t nbos;
	const uint32_t *in;
	uint32_t nin;
	uint32_t out, req, ctx;
};

static int submit(int fd, const struct sub *s)
{
	struct drm_panfrost_submit a = {
		.jc = s->jc,
		.in_syncs = (uintptr_t)s->in,
		.in_sync_count = s->nin,
		.out_sync = s->out,
		.bo_handles = (uintptr_t)s->bos,
		.bo_handle_count = s->nbos,
		.requirements = s->req,
		.jm_ctx_handle = s->ctx,
	};

	return ioc(fd, DRM_IOCTL_PANFROST_SUBMIT, &a);
}

static uint32_t syncobj(int fd, int signalled)
{
	uint32_t h = 0;

	CHECK_EQ(drmSyncobjCreate(fd, signalled ? DRM_SYNCOBJ_CREATE_SIGNALED : 0, &h), 0);
	return h;
}

/* drmSyncobjWait(INT64_MAX) (what panvk's PANVK_DEBUG=sync path does), then the fence status. */
static int32_t wait_status(int fd, uint32_t so)
{
	int32_t st = 99;

	CHECK_EQ(drmSyncobjWait(fd, &so, 1, INT64_MAX, 0, NULL), 0);
	CHECK_EQ(syncobj_status(fd, so, &st), 0);
	return st;
}

static int wait_bo(int fd, uint32_t handle, int64_t abs_timeout_ns)
{
	struct drm_panfrost_wait_bo w = { .handle = handle, .timeout_ns = abs_timeout_ns };

	return ioc(fd, DRM_IOCTL_PANFROST_WAIT_BO, &w);
}

static uint32_t ctx_create(int fd, uint32_t prio, int *err)
{
	struct drm_panfrost_jm_ctx_create c = { .priority = prio };

	*err = ioc(fd, DRM_IOCTL_PANFROST_JM_CTX_CREATE, &c);
	return c.handle;
}

static int ctx_destroy(int fd, uint32_t h)
{
	struct drm_panfrost_jm_ctx_destroy d = { .handle = h };

	return ioc(fd, DRM_IOCTL_PANFROST_JM_CTX_DESTROY, &d);
}

/* =========================================================================================== */

static int open_device(drmVersionPtr *vout)
{
	drmDevicePtr devs[4];
	drmVersionPtr v;
	char node[64] = "";
	int n, fd;
	uint64_t cap = 7;

	/* Mesa's enumeration (panvk_enumerate_devices / loader): count first, then the list. */
	n = drmGetDevices2(0, NULL, 0);
	CHECK_EQ(n, 1);
	n = drmGetDevices2(0, devs, 4);
	CHECK_EQ(n, 1);
	if (n != 1)
		return -1;
	CHECK(devs[0]->available_nodes & (1 << DRM_NODE_RENDER));
	CHECK(devs[0]->bustype == DRM_BUS_PLATFORM);
	CHECK(!strcmp(devs[0]->deviceinfo.platform->compatible[1], "arm,mali-bifrost"));
	snprintf(node, sizeof(node), "%s", devs[0]->nodes[DRM_NODE_RENDER]);
	printf("device: %s %s (%s, %s)\n", node, devs[0]->businfo.platform->fullname,
	       devs[0]->deviceinfo.platform->compatible[0],
	       devs[0]->deviceinfo.platform->compatible[1]);
	drmFreeDevices(devs, n);

	/* Mesa's open() of the render node is routed to panwin_open() on Windows. */
	fd = panwin_open(node, 2 /* O_RDWR */);
	CHECK(fd >= 0);
	if (fd < 0)
		return -1;
	v = drmGetVersion(fd);
	CHECK(v != NULL);
	if (!v)
		return -1;
	/* pan_kmod_dev_create() picks panfrost_kmod_ops for "panfrost". */
	CHECK(!strcmp(v->name, "panfrost"));
	CHECK_EQ(v->version_major, 1);
	CHECK(v->version_minor >= 5);
	printf("DRM %s %d.%d.%d (%s)\n", v->name, v->version_major, v->version_minor,
	       v->version_patchlevel, v->desc);
	CHECK_EQ(drmGetCap(fd, DRM_CAP_SYNCOBJ, &cap), 0);
	CHECK_EQ(cap, 1);
	CHECK_EQ(drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap), 0);
	CHECK_EQ(cap, 0);
	*vout = v;
	return fd;
}

/* Every DRM_PANFROST_PARAM_* panfrost_kmod.c reads (panfrost_dev_query_props,
 * panfrost_dev_query_thread_props, panfrost_kmod_query_timestamp), with Mesa's sanity rules. */
static void test_params(int fd, const drmVersionPtr v)
{
	static const struct { uint32_t p; const char *name; } req[] = {
		{ DRM_PANFROST_PARAM_GPU_PROD_ID, "GPU_PROD_ID" },
		{ DRM_PANFROST_PARAM_GPU_REVISION, "GPU_REVISION" },
		{ DRM_PANFROST_PARAM_SHADER_PRESENT, "SHADER_PRESENT" },
		{ DRM_PANFROST_PARAM_TILER_FEATURES, "TILER_FEATURES" },
		{ DRM_PANFROST_PARAM_MEM_FEATURES, "MEM_FEATURES" },
		{ DRM_PANFROST_PARAM_MMU_FEATURES, "MMU_FEATURES" },
		{ DRM_PANFROST_PARAM_L2_FEATURES, "L2_FEATURES" },
		{ DRM_PANFROST_PARAM_TEXTURE_FEATURES0, "TEXTURE_FEATURES0" },
		{ DRM_PANFROST_PARAM_TEXTURE_FEATURES1, "TEXTURE_FEATURES1" },
		{ DRM_PANFROST_PARAM_TEXTURE_FEATURES2, "TEXTURE_FEATURES2" },
		{ DRM_PANFROST_PARAM_TEXTURE_FEATURES3, "TEXTURE_FEATURES3" },
		{ DRM_PANFROST_PARAM_AFBC_FEATURES, "AFBC_FEATURES" },
		{ DRM_PANFROST_PARAM_MAX_THREADS, "MAX_THREADS" },
		{ DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ, "THREAD_MAX_WORKGROUP_SZ" },
		{ DRM_PANFROST_PARAM_THREAD_FEATURES, "THREAD_FEATURES" },
		{ DRM_PANFROST_PARAM_THREAD_TLS_ALLOC, "THREAD_TLS_ALLOC" },
		{ DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP_FREQUENCY, "SYSTEM_TIMESTAMP_FREQUENCY" },
		{ DRM_PANFROST_PARAM_ALLOWED_JM_CTX_PRIORITIES, "ALLOWED_JM_CTX_PRIORITIES" },
		{ DRM_PANFROST_PARAM_SELECTED_COHERENCY, "SELECTED_COHERENCY" },
		{ DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP, "SYSTEM_TIMESTAMP" },
	};
	uint64_t val[sizeof(req) / sizeof(req[0])] = { 0 };
	uint64_t gpu_id, ts0 = 0, ts1 = 0, x;
	struct drm_panfrost_get_param bad = { .param = 0, .pad = 1 };

	for (unsigned i = 0; i < sizeof(req) / sizeof(req[0]); i++) {
		int e = get_param(fd, req[i].p, &val[i]);

		CHECK_EQ(e, 0);	/* Mesa: required = true asserts success */
		printf("  %-27s 0x%llx\n", req[i].name, (unsigned long long)val[i]);
	}
	gpu_id = (val[0] << 16) | val[1];
	CHECK_EQ(gpu_id >> 28, 7);	/* pan_arch(): Bifrost v7 */
	CHECK(val[2] != 0);		/* shader cores */
	CHECK((val[5] & 0xff) >= 33);	/* MMU VA bits: the 4 GiB user range needs > 32 */
	CHECK(val[12] != 0 && val[13] != 0 && val[15] != 0);
	CHECK(val[16] != 0);		/* Mesa >= 1.3: timestamp_cycles_to_ns_factor */
	CHECK(val[17] & (1u << PANFROST_JM_CTX_PRIORITY_MEDIUM));
	CHECK(!(val[17] & (1u << PANFROST_JM_CTX_PRIORITY_HIGH)));	/* EACCES on Windows */
	CHECK_EQ(val[18], DRM_PANFROST_GPU_COHERENCY_NONE);
	printf("GET_PARAM: %u params; gpu_id 0x%llx arch %llu, %d cores, timestamp %llu Hz\n",
	       (unsigned)(sizeof(req) / sizeof(req[0])), (unsigned long long)gpu_id,
	       (unsigned long long)(gpu_id >> 28), __builtin_popcountll(val[2]),
	       (unsigned long long)val[16]);
	CHECK_EQ(get_param(fd, DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP, &ts0), 0);
	Sleep(2);
	CHECK_EQ(get_param(fd, DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP, &ts1), 0);
	CHECK(ts1 > ts0);
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_GET_PARAM, &bad), EINVAL);
	CHECK_EQ(get_param(fd, DRM_PANFROST_PARAM_SELECTED_COHERENCY + 1, &x), EINVAL);
	(void)v;
}

/* CREATE_BO flavours, GET_BO_OFFSET, MMAP_BO, CPU mappings, errnos. */
static void test_bos(int fd, const drmVersionPtr v)
{
	struct bo ex, nx, heap, ro;
	struct drm_panfrost_create_bo c;
	struct drm_panfrost_mmap_bo m;
	struct drm_panfrost_get_bo_offset o;
	uint8_t *alias;

	/* Executable (Mesa: PAN_KMOD_BO_FLAG_EXECUTABLE -> no NOEXEC), size-aligned VA. */
	CHECK_EQ(bo_create(fd, &ex, 64 << 10, 0, 1), 0);
	CHECK_EQ(ex.va & ((64 << 10) - 1), 0);
	/* NOEXEC >= 2 MiB: 2 MiB-aligned VA. */
	CHECK_EQ(bo_create(fd, &nx, 4 * SZ_1M, PANFROST_BO_NOEXEC, 1), 0);
	CHECK_EQ(nx.va & (SZ_2M - 1), 0);
	CHECK(nx.va + nx.size <= ex.va || ex.va + ex.size <= nx.va);
	/* Fresh BOs read zero (PW_MEM_ZEROED). */
	{
		int nz = 0;

		for (uint32_t i = 0; i < nx.size; i += 4096)
			nz |= nx.cpu[i] | nx.cpu[i + 4095];
		CHECK_EQ(nz, 0);
	}
	/* Two mappings of one BO alias the same pages (Mesa may map a BO twice). */
	m = (struct drm_panfrost_mmap_bo){ .handle = nx.handle };
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_MMAP_BO, &m), 0);
	alias = panwin_mmap(fd, m.offset, nx.size, PROT_R);
	CHECK(alias != NULL && alias != nx.cpu);
	if (alias) {
		put32(nx.cpu + 12345 * 4, 0xc0ffee11u);
		CHECK_EQ(get32(alias + 12345 * 4), 0xc0ffee11u);
		CHECK_EQ(panwin_munmap(alias, nx.size), 0);
	}
	/* HEAP | NOEXEC (Mesa: ALLOC_ON_FAULT): rounded to 2 MiB, no CPU mapping. */
	CHECK_EQ(bo_create(fd, &heap, SZ_1M, PANFROST_BO_HEAP | PANFROST_BO_NOEXEC, 0), 0);
	CHECK_EQ(heap.va & (SZ_2M - 1), 0);
	m = (struct drm_panfrost_mmap_bo){ .handle = heap.handle };
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_MMAP_BO, &m), EINVAL);
	/* errnos (panfrost_ioctl_create_bo / mmap_bo / get_bo_offset) */
	c = (struct drm_panfrost_create_bo){ .size = 0 };
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_CREATE_BO, &c), EINVAL);
	c = (struct drm_panfrost_create_bo){ .size = 4096, .flags = PANFROST_BO_HEAP };
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_CREATE_BO, &c), EINVAL);	/* heap must be NOEXEC */
	c = (struct drm_panfrost_create_bo){ .size = 4096, .flags = 0x80 };
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_CREATE_BO, &c), EINVAL);
	c = (struct drm_panfrost_create_bo){ .size = 4096, .pad = 1 };
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_CREATE_BO, &c), EINVAL);
	if (v->version_minor < 6) {	/* WB_MMAP is a 1.6 flag */
		c = (struct drm_panfrost_create_bo){ .size = 4096, .flags = PANFROST_BO_WB_MMAP };
		CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_CREATE_BO, &c), EINVAL);
	}
	m = (struct drm_panfrost_mmap_bo){ .handle = 0xdead };
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_MMAP_BO, &m), ENOENT);
	m = (struct drm_panfrost_mmap_bo){ .handle = ex.handle, .flags = 1 };
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_MMAP_BO, &m), EINVAL);
	o = (struct drm_panfrost_get_bo_offset){ .handle = 0xdead };
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_GET_BO_OFFSET, &o), ENOENT);
	/* A read-only mapping (PROT_READ only) reads the zeroed BO; a mapping larger than the BO is
	 * refused. */
	CHECK_EQ(bo_create(fd, &ro, 8192, PANFROST_BO_NOEXEC, 0), 0);
	m = (struct drm_panfrost_mmap_bo){ .handle = ro.handle };
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_MMAP_BO, &m), 0);
	ro.cpu = panwin_mmap(fd, m.offset, ro.size, PROT_R);
	CHECK(ro.cpu && ro.cpu[100] == 0);
	errno = 0;
	CHECK(panwin_mmap(fd, m.offset, ro.size * 2, PROT_R) == NULL && errno == EINVAL);
	/* The 1.6 ioctls are not there at 1.5 (jm-arch: the dispatcher gates them). */
	if (v->version_minor < 6) {
		struct drm_panfrost_query_bo_info q = { .handle = ex.handle };
		struct drm_panfrost_sync_bo s = { 0 };

		CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_QUERY_BO_INFO, &q), ENOTTY);
		CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_SYNC_BO, &s), ENOTTY);
	}
	/* GEM_CLOSE (drmCloseBufferHandle): the handle is gone, a second close is EINVAL
	 * (drm_gem_close_ioctl); an existing mapping keeps the pages. */
	put32(nx.cpu, 0x5a5a5a5au);
	bo_close(fd, &nx);
	CHECK_EQ(get32(nx.cpu), 0x5a5a5a5au);
	CHECK_EQ(drmCloseBufferHandle(fd, ex.handle), 0);
	errno = 0;
	CHECK(drmCloseBufferHandle(fd, ex.handle) != 0 && errno == EINVAL);
	o = (struct drm_panfrost_get_bo_offset){ .handle = ex.handle };
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_GET_BO_OFFSET, &o), ENOENT);
	m = (struct drm_panfrost_mmap_bo){ .handle = ex.handle };
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_MMAP_BO, &m), ENOENT);
	ex.handle = 0;
	bo_free(fd, &ex);
	bo_free(fd, &nx);
	bo_free(fd, &heap);
	bo_free(fd, &ro);
	printf("BOs: exec / NOEXEC / HEAP created, mapped, aliased, closed\n");
}

/*
 * The basic JM submit: a dependent v7 chain on JS1 (NULL, WRITE_VALUE x6, COMPUTE), then a
 * fragment chain on JS0 (REQ_FS) chained through panvk's in_sync == out_sync, with
 * bo_handles, in_syncs and out_sync, drmSyncobjWait, CPU readback of the values and of every
 * descriptor's exception status. CYCLE_COUNT requirement on the first one.
 */
static void test_submit(int fd)
{
	struct bo desc, data;
	struct chain c, f;
	uint8_t *jobs[16];
	uint32_t pre = syncobj(fd, 1), out = syncobj(fd, 0), bos[2];
	uint64_t t0 = 0, t1 = 0, gpu_ts;
	unsigned nj = 0;
	int32_t st;
	struct sub s;

	CHECK_EQ(bo_create(fd, &desc, 64 << 10, PANFROST_BO_NOEXEC, 1), 0);
	CHECK_EQ(bo_create(fd, &data, 4096, PANFROST_BO_NOEXEC, 1), 0);
	if (!desc.cpu || !data.cpu)
		return;
	bos[0] = desc.handle;
	bos[1] = data.handle;
	memset(data.cpu, 0xee, 64);	/* CPU writes the GPU will overwrite (ZERO) */

	chain_init(&c, &desc);
	jobs[nj++] = chain_add(&c, JT_NULL, 32, 0, 0);					/* 1 */
	jobs[nj++] = chain_wv(&c, data.va + 0, WV_IMM32, 0x11223344u, 1);		/* 2 */
	jobs[nj++] = chain_wv(&c, data.va + 8, WV_IMM64, 0x8877665544332211ull, 2);	/* 3 */
	/* Compute Job (v7.xml: header, Invocation at 32, Parameters at 40, Draw at 64): no
	 * shader in the mock, it just completes; depends on 3 and 1. */
	jobs[nj++] = chain_add(&c, JT_COMPUTE, 192, 3, 1);				/* 4 */
	jobs[nj++] = chain_wv(&c, data.va + 16, WV_IMM16, 0xbeefu, 4);			/* 5 */
	jobs[nj++] = chain_wv(&c, data.va + 24, WV_ZERO, 0, 5);				/* 6 */
	jobs[nj++] = chain_wv(&c, data.va + 32, WV_SYSTEM_TIMESTAMP, 0, 6);		/* 7 */
	jobs[nj++] = chain_wv(&c, data.va + 40, WV_CYCLE_COUNTER, 0, 7);		/* 8 */
	jobs[nj++] = chain_wv(&c, data.va + 48, WV_IMM8, 0x5au, 8);			/* 9 */
	/* The descriptors are CPU writes through the write-combined mapping: the GPU walking
	 * them is the "CPU writes are visible to the GPU" check. */

	CHECK_EQ(get_param(fd, DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP, &t0), 0);
	s = (struct sub){ .jc = c.first, .bos = bos, .nbos = 2, .in = &pre, .nin = 1, .out = out,
			  .req = PANFROST_JD_REQ_CYCLE_COUNT };
	CHECK_EQ(submit(fd, &s), 0);

	/* Fragment chain on JS0, waiting for the vertex/compute chain like panvk does. */
	chain_init(&f, &desc);
	desc.used = (desc.used + 4095u) & ~4095u;
	jobs[nj++] = chain_add(&f, JT_FRAGMENT, 64, 0, 0);
	jobs[nj++] = chain_wv(&f, data.va + 56, WV_IMM32, 0xf4a6u, 1);
	s = (struct sub){ .jc = f.first, .bos = bos, .nbos = 2, .in = &out, .nin = 1, .out = out,
			  .req = PANFROST_JD_REQ_FS };
	CHECK_EQ(submit(fd, &s), 0);

	st = wait_status(fd, out);
	CHECK_EQ(st, 1);
	CHECK_EQ(get_param(fd, DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP, &t1), 0);
	CHECK_EQ(get32(data.cpu + 0), 0x11223344u);
	CHECK_EQ(get64(data.cpu + 8), 0x8877665544332211ull);
	CHECK_EQ(get32(data.cpu + 16), 0xeeeebeefu);	/* 16-bit write */
	CHECK_EQ(get64(data.cpu + 24), 0);
	gpu_ts = get64(data.cpu + 32);
	CHECK(gpu_ts >= t0 && gpu_ts <= t1);		/* device-coherent timestamps */
	CHECK(get64(data.cpu + 40) != 0 && get64(data.cpu + 40) != 0xeeeeeeeeeeeeeeeeull);
	CHECK_EQ(data.cpu[48], 0x5a);
	CHECK_EQ(data.cpu[49], 0xee);
	CHECK_EQ(get32(data.cpu + 56), 0xf4a6u);
	for (unsigned i = 0; i < nj; i++)
		CHECK_EQ(exc_status(jobs[i]), EXC_DONE);
	CHECK_EQ(wait_bo(fd, data.handle, 0), 0);
	printf("submit: %u-job JS1 chain + %u-job JS0 (FS) chain done; GPU timestamp %llu in "
	       "[%llu, %llu], cycle counter %llu\n", c.n, f.n, (unsigned long long)gpu_ts,
	       (unsigned long long)t0, (unsigned long long)t1,
	       (unsigned long long)get64(data.cpu + 40));

	/* SUBMIT errnos (panfrost_ioctl_submit order) */
	s = (struct sub){ .jc = 0, .bos = bos, .nbos = 2 };
	CHECK_EQ(submit(fd, &s), EINVAL);
	s = (struct sub){ .jc = c.first, .req = 1u << 5 };
	CHECK_EQ(submit(fd, &s), EINVAL);
	s = (struct sub){ .jc = c.first, .out = 0xdead };
	CHECK_EQ(submit(fd, &s), ENODEV);
	s = (struct sub){ .jc = c.first, .ctx = 63 };
	CHECK_EQ(submit(fd, &s), EINVAL);
	{
		uint32_t bad = 0xdead, nofence = syncobj(fd, 0);

		s = (struct sub){ .jc = c.first, .in = &bad, .nin = 1 };
		CHECK_EQ(submit(fd, &s), ENOENT);
		s = (struct sub){ .jc = c.first, .in = &nofence, .nin = 1 };
		CHECK_EQ(submit(fd, &s), EINVAL);
		s = (struct sub){ .jc = c.first, .bos = &bad, .nbos = 1 };
		CHECK_EQ(submit(fd, &s), ENOENT);
		CHECK_EQ(drmSyncobjDestroy(fd, nofence), 0);
	}
	CHECK_EQ(drmSyncobjDestroy(fd, pre), 0);
	CHECK_EQ(drmSyncobjDestroy(fd, out), 0);
	bo_free(fd, &desc);
	bo_free(fd, &data);
}

/*
 * Implicit sync: A (JS1) and B (JS0, FS) share only BO "shared"; A waits (in_sync) for P, NPC
 * long chains on JS1 (chained by in_sync == out_sync) that are still running when B is
 * submitted, so B is ready long before A. Panfrost treats every BO of a job as written, so B
 * must still run after A: the final value is B's and B's GPU timestamp is not older than A's.
 * (Without the implicit dependency B runs first: checked by breaking it on purpose, see
 * docs/status/jm-e2e.md.) WAIT_BO on the shared BO: poll while busy (EBUSY), then idle. P's
 * BOs are closed right after its submits (the jobs keep them alive).
 */
static void test_implicit_sync(int fd)
{
	enum { NP = 4000, NPC = 8 };	/* jobs per chain < MOCK_JM_MAX_JOBS */
	struct bo dp, scratch, da, db, shared;
	struct chain p, a, b;
	uint32_t sp = syncobj(fd, 0), sa = syncobj(fd, 0), sb = syncobj(fd, 0);
	uint32_t bp[2], ba[2], bb[2];
	uint8_t *plast;
	uint64_t jc[NPC];
	int64_t t0, t1;
	struct sub s;
	int e0, e1;

	CHECK_EQ(bo_create(fd, &dp, 2 * SZ_1M, PANFROST_BO_NOEXEC, 1), 0);	/* NPC * NP * 64 */
	CHECK_EQ(bo_create(fd, &scratch, 8192, PANFROST_BO_NOEXEC, 1), 0);
	CHECK_EQ(bo_create(fd, &da, 4096, PANFROST_BO_NOEXEC, 1), 0);
	CHECK_EQ(bo_create(fd, &db, 4096, PANFROST_BO_NOEXEC, 1), 0);
	CHECK_EQ(bo_create(fd, &shared, 4096, PANFROST_BO_NOEXEC, 1), 0);
	if (!dp.cpu || !scratch.cpu || !da.cpu || !db.cpu || !shared.cpu)
		return;
	bp[0] = dp.handle; bp[1] = scratch.handle;
	ba[0] = da.handle; ba[1] = shared.handle;
	bb[0] = db.handle; bb[1] = shared.handle;
	/* P: NPC chains, each with its own descriptors (a finished job keeps its DONE status;
	 * panvk resets headers before re-issuing, we just never reuse them). The last job of the
	 * last chain writes a timestamp. All chains are written first, then submitted back to back,
	 * so the queue is long when A and B arrive. */
	for (unsigned k = 0; k < NPC; k++) {
		chain_init(&p, &dp);
		for (unsigned i = 0; i < NP - 1; i++)
			chain_wv(&p, scratch.va + (i % 512) * 8, WV_IMM64, k * NP + i, i);
		chain_wv(&p, scratch.va + 4096, WV_SYSTEM_TIMESTAMP, 0, NP - 1);
		jc[k] = p.first;
	}
	t0 = now_ns();
	for (unsigned k = 0; k < NPC; k++) {
		s = (struct sub){ .jc = jc[k], .bos = bp, .nbos = 2, .out = sp,
				  .in = k ? &sp : NULL, .nin = k ? 1 : 0 };
		CHECK_EQ(submit(fd, &s), 0);
	}
	plast = p.last;
	bo_close(fd, &dp);		/* the jobs hold the BOs; the mappings stay */
	bo_close(fd, &scratch);
	chain_init(&a, &da);
	chain_wv(&a, shared.va, WV_IMM32, 0xaaaa0001u, 0);
	chain_wv(&a, shared.va + 8, WV_SYSTEM_TIMESTAMP, 0, 1);
	s = (struct sub){ .jc = a.first, .bos = ba, .nbos = 2, .in = &sp, .nin = 1, .out = sa };
	CHECK_EQ(submit(fd, &s), 0);
	chain_init(&b, &db);
	chain_wv(&b, shared.va, WV_IMM32, 0xbbbb0002u, 0);
	chain_wv(&b, shared.va + 16, WV_SYSTEM_TIMESTAMP, 0, 1);
	s = (struct sub){ .jc = b.first, .bos = bb, .nbos = 2, .out = sb, .req = PANFROST_JD_REQ_FS };
	CHECK_EQ(submit(fd, &s), 0);
	e0 = wait_bo(fd, shared.handle, 0);			/* poll */
	CHECK(e0 == 0 || e0 == EBUSY);
	e1 = wait_bo(fd, shared.handle, now_ns() + 100000);	/* 100 us */
	CHECK(e1 == 0 || e1 == ETIMEDOUT);			/* Mesa asserts these two */
	CHECK_EQ(wait_bo(fd, shared.handle, INT64_MAX), 0);
	t1 = now_ns();
	/* WAIT_BO idle => the last job on the BO (B) is done, and so is everything before it. */
	CHECK_EQ(get32(shared.cpu), 0xbbbb0002u);
	CHECK(get64(shared.cpu + 8) >= get64(scratch.cpu + 4096));	/* A after P (in_sync) */
	CHECK(get64(shared.cpu + 16) >= get64(shared.cpu + 8));	/* B after A (implicit) */
	CHECK(get64(shared.cpu + 8) != 0);
	CHECK_EQ(wait_status(fd, sb), 1);
	CHECK_EQ(wait_status(fd, sa), 1);
	CHECK_EQ(wait_status(fd, sp), 1);
	CHECK_EQ(exc_status(plast), EXC_DONE);
	CHECK_EQ(get64(scratch.cpu + ((NP - 2) % 512) * 8), (NPC - 1) * NP + NP - 2);
	CHECK_EQ(wait_bo(fd, shared.handle, 0), 0);
	CHECK_EQ(wait_bo(fd, 0xdead, 0), ENOENT);
	{
		struct drm_panfrost_wait_bo w = { .handle = shared.handle, .pad = 1 };

		CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_WAIT_BO, &w), EINVAL);
	}
	printf("implicit sync: P (%u chains x %u jobs) -> A (in_sync) -> B (shared BO only, JS0) in "
	       "order, %.1f ms; WAIT_BO poll %s, 100us %s\n", (unsigned)NPC, (unsigned)NP,
	       (t1 - t0) / 1e6, e0 ? "EBUSY" : "idle", e1 ? "ETIMEDOUT" : "idle");
	CHECK_EQ(drmSyncobjDestroy(fd, sp), 0);
	CHECK_EQ(drmSyncobjDestroy(fd, sa), 0);
	CHECK_EQ(drmSyncobjDestroy(fd, sb), 0);
	bo_free(fd, &dp);
	bo_free(fd, &scratch);
	bo_free(fd, &da);
	bo_free(fd, &db);
	bo_free(fd, &shared);
}

/*
 * Failing jobs: a JOB_CONFIG_FAULT (type 0 descriptor) and an unhandled GPU page fault (write
 * outside every BO). drmSyncobjWait still succeeds (Linux semantics), the fence carries -EINVAL
 * (panfrost_job_handle_err), the faulting descriptor's exception status is written back, and the
 * file keeps working afterwards (the AS disabled by the fault is re-enabled for the next job).
 */
static void test_faults(int fd)
{
	struct bo desc, data;
	struct chain c;
	uint32_t so = syncobj(fd, 0), bos[2];
	uint8_t *j1, *j2;
	struct sub s;
	int32_t st;

	CHECK_EQ(bo_create(fd, &desc, 16384, PANFROST_BO_NOEXEC, 1), 0);
	CHECK_EQ(bo_create(fd, &data, 4096, PANFROST_BO_NOEXEC, 1), 0);
	if (!desc.cpu || !data.cpu)
		return;
	bos[0] = desc.handle;
	bos[1] = data.handle;

	/* 1. config fault on the second job: the first one ran. */
	chain_init(&c, &desc);
	j1 = chain_wv(&c, data.va, WV_IMM32, 0x1234u, 0);
	j2 = chain_add(&c, JT_NOT_STARTED, 64, 1, 0);
	chain_wv(&c, data.va + 4, WV_IMM32, 0x5678u, 2);
	s = (struct sub){ .jc = c.first, .bos = bos, .nbos = 2, .out = so };
	CHECK_EQ(submit(fd, &s), 0);
	st = wait_status(fd, so);
	CHECK_EQ(st, -EINVAL);
	CHECK_EQ(exc_status(j1), EXC_DONE);
	CHECK_EQ(exc_status(j2), EXC_CONFIG_FAULT);
	CHECK_EQ(get32(data.cpu), 0x1234u);
	CHECK_EQ(get32(data.cpu + 4), 0);
	printf("faults: config fault -> fence %d, descriptor status 0x%x\n", st, exc_status(j2));

	/* 2. unhandled page fault: write far above every BO of this file. */
	desc.used = 4096;
	chain_init(&c, &desc);
	j1 = chain_wv(&c, 0xF0000000ull, WV_IMM32, 0xdeadu, 0);
	s = (struct sub){ .jc = c.first, .bos = bos, .nbos = 2, .out = so };
	CHECK_EQ(submit(fd, &s), 0);
	st = wait_status(fd, so);
	CHECK_EQ(st, -EINVAL);
	CHECK(exc_status(j1) != EXC_DONE);
	printf("faults: page fault at 0xf0000000 -> fence %d, descriptor status 0x%x\n", st,
	       exc_status(j1));

	/* 3. the same file runs jobs again */
	desc.used = 8192;
	chain_init(&c, &desc);
	j1 = chain_wv(&c, data.va + 8, WV_IMM32, 0x600du, 0);
	s = (struct sub){ .jc = c.first, .bos = bos, .nbos = 2, .out = so };
	CHECK_EQ(submit(fd, &s), 0);
	CHECK_EQ(wait_status(fd, so), 1);
	CHECK_EQ(get32(data.cpu + 8), 0x600du);
	CHECK_EQ(exc_status(j1), EXC_DONE);
	CHECK_EQ(drmSyncobjDestroy(fd, so), 0);
	bo_free(fd, &desc);
	bo_free(fd, &data);
}

/*
 * HEAP growth (Mesa's tiler heap: PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT): WRITE_VALUE jobs into two
 * untouched 2 MiB chunks of a HEAP BO fault, the core maps the chunk from the MMU IRQ
 * (panfrost_mmu.c:panfrost_mmu_map_fault_addr behaviour) and the jobs complete. A HEAP BO has no
 * CPU mapping, so success is the fence and the written-back DONE status of every job.
 */
static void test_heap(int fd)
{
	struct bo desc, heap;
	struct chain c;
	uint8_t *j[4];
	uint32_t so = syncobj(fd, 0), bos[2];
	struct sub s;

	CHECK_EQ(bo_create(fd, &desc, 4096, PANFROST_BO_NOEXEC, 1), 0);
	CHECK_EQ(bo_create(fd, &heap, 4 * SZ_1M, PANFROST_BO_HEAP | PANFROST_BO_NOEXEC, 0), 0);
	if (!desc.cpu)
		return;
	bos[0] = desc.handle;
	bos[1] = heap.handle;
	chain_init(&c, &desc);
	j[0] = chain_wv(&c, heap.va + 0x100, WV_IMM32, 1, 0);			/* fault, chunk 0 */
	j[1] = chain_wv(&c, heap.va + SZ_2M + 0x200, WV_IMM64, 2, 1);		/* fault, chunk 1 */
	j[2] = chain_wv(&c, heap.va + 0x104, WV_IMM32, 3, 2);			/* mapped now */
	j[3] = chain_wv(&c, heap.va + 4 * SZ_1M - 8, WV_IMM64, 4, 3);		/* end of chunk 1 */
	s = (struct sub){ .jc = c.first, .bos = bos, .nbos = 2, .out = so };
	CHECK_EQ(submit(fd, &s), 0);
	CHECK_EQ(wait_status(fd, so), 1);
	for (unsigned i = 0; i < 4; i++)
		CHECK_EQ(exc_status(j[i]), EXC_DONE);
	/* A write past the heap's end is not grown: it faults the job. */
	desc.used = 2048;
	chain_init(&c, &desc);
	j[0] = chain_wv(&c, heap.va + 4 * SZ_1M + 0x10, WV_IMM32, 5, 0);
	s = (struct sub){ .jc = c.first, .bos = bos, .nbos = 2, .out = so };
	CHECK_EQ(submit(fd, &s), 0);
	CHECK_EQ(wait_status(fd, so), -EINVAL);
	printf("heap: 2 chunks grown on GPU fault, 4 jobs done; write past the heap -> -EINVAL\n");
	CHECK_EQ(drmSyncobjDestroy(fd, so), 0);
	bo_free(fd, &desc);
	bo_free(fd, &heap);
}

/* JM contexts (Gallium pan_jm.c), MADVISE (panfrost_kmod make_(un)evictable), SET_LABEL_BO. */
static void test_ctx_madvise_label(int fd)
{
	struct bo desc, data;
	struct chain c;
	uint32_t so = syncobj(fd, 0), bos[2], lo, med, keep;
	struct drm_panfrost_madvise mv;
	struct drm_panfrost_set_label_bo lb;
	static char big[4097];
	struct sub s;
	int e;

	CHECK_EQ(bo_create(fd, &desc, 8192, PANFROST_BO_NOEXEC, 1), 0);
	CHECK_EQ(bo_create(fd, &data, 4096, PANFROST_BO_NOEXEC, 1), 0);
	if (!desc.cpu || !data.cpu)
		return;
	bos[0] = desc.handle;
	bos[1] = data.handle;

	lo = ctx_create(fd, PANFROST_JM_CTX_PRIORITY_LOW, &e);
	CHECK_EQ(e, 0);
	CHECK(lo >= 1);
	med = ctx_create(fd, PANFROST_JM_CTX_PRIORITY_MEDIUM, &e);
	CHECK_EQ(e, 0);
	CHECK(med >= 1 && med != lo);
	keep = ctx_create(fd, PANFROST_JM_CTX_PRIORITY_MEDIUM, &e);	/* left for file close */
	CHECK_EQ(e, 0);
	ctx_create(fd, PANFROST_JM_CTX_PRIORITY_HIGH, &e);
	CHECK_EQ(e, EACCES);
	ctx_create(fd, 7, &e);
	CHECK_EQ(e, EINVAL);
	chain_init(&c, &desc);
	chain_wv(&c, data.va, WV_IMM32, 0x10u, 0);
	s = (struct sub){ .jc = c.first, .bos = bos, .nbos = 2, .out = so, .ctx = lo };
	CHECK_EQ(submit(fd, &s), 0);
	CHECK_EQ(wait_status(fd, so), 1);
	desc.used = 4096;
	chain_init(&c, &desc);
	chain_wv(&c, data.va + 4, WV_IMM32, 0x20u, 0);
	s = (struct sub){ .jc = c.first, .bos = bos, .nbos = 2, .out = so, .ctx = keep,
			  .req = PANFROST_JD_REQ_FS };
	CHECK_EQ(submit(fd, &s), 0);
	CHECK_EQ(wait_status(fd, so), 1);
	CHECK_EQ(get32(data.cpu), 0x10u);
	CHECK_EQ(get32(data.cpu + 4), 0x20u);
	CHECK_EQ(ctx_destroy(fd, lo), 0);
	CHECK_EQ(ctx_destroy(fd, lo), EINVAL);
	CHECK_EQ(ctx_destroy(fd, 0), EINVAL);		/* the default context */
	s.ctx = lo;
	CHECK_EQ(submit(fd, &s), EINVAL);
	CHECK_EQ(ctx_destroy(fd, med), 0);
	{
		struct drm_panfrost_jm_ctx_destroy d = { .handle = keep, .pad = 1 };

		CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_JM_CTX_DESTROY, &d), EINVAL);
	}

	/* MADVISE: no shrinker on Windows, pages always retained; contents and GPU use survive. */
	mv = (struct drm_panfrost_madvise){ .handle = data.handle, .madv = PANFROST_MADV_DONTNEED };
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_MADVISE, &mv), 0);
	mv = (struct drm_panfrost_madvise){ .handle = data.handle, .madv = PANFROST_MADV_WILLNEED };
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_MADVISE, &mv), 0);
	CHECK_EQ(mv.retained, 1);	/* make_unevictable() == true */
	CHECK_EQ(get32(data.cpu), 0x10u);
	mv = (struct drm_panfrost_madvise){ .handle = 0xdead, .madv = PANFROST_MADV_WILLNEED };
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_MADVISE, &mv), ENOENT);

	/* SET_LABEL_BO (Mesa >= 1.4, labels truncated to 4095 chars) */
	lb = (struct drm_panfrost_set_label_bo){ .handle = desc.handle,
		.label = (uintptr_t)"jm-e2e descriptor pool" };
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_SET_LABEL_BO, &lb), 0);
	memset(big, 'x', 4095);
	big[4095] = 0;
	lb.label = (uintptr_t)big;
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_SET_LABEL_BO, &lb), 0);
	big[4095] = 'y';
	big[4096] = 0;
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_SET_LABEL_BO, &lb), E2BIG);
	lb.label = 0;					/* clear */
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_SET_LABEL_BO, &lb), 0);
	lb.pad = 1;
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_SET_LABEL_BO, &lb), EINVAL);
	lb = (struct drm_panfrost_set_label_bo){ .handle = 0xdead };
	CHECK_EQ(ioc(fd, DRM_IOCTL_PANFROST_SET_LABEL_BO, &lb), ENOENT);
	printf("contexts LOW/MEDIUM (HIGH EACCES), MADVISE retained, SET_LABEL_BO: ok\n");
	CHECK_EQ(drmSyncobjDestroy(fd, so), 0);
	bo_free(fd, &desc);
	bo_free(fd, &data);
}

/*
 * Stress: @n submits over two files (two address spaces), alternating JS1 / JS0, the default
 * and a LOW context, in_syncs on the previous submit every 5th time, out_syncs round robin over
 * 4 syncobjs per file, waits every 100 submits. Each submit writes its number and a GPU
 * timestamp into its own slot, and its number into a per-file "last" word shared by all of that
 * file's jobs: implicit sync orders a file's jobs across both slots, so the timestamps never go
 * backwards in submit order and the final "last" is the file's last submit.
 */
static void test_stress(int fd0, int fd1, unsigned n)
{
	enum { NSO = 4 };
	struct per {
		int fd;
		struct bo desc, data, last;
		uint32_t so[NSO], ctx, prev, count, last_i;
	} pf[2];
	int64_t t0 = now_ns(), dt;
	unsigned bad = 0, failed = 0, backwards = 0;

	for (int k = 0; k < 2; k++) {
		struct per *p = &pf[k];
		int e;

		memset(p, 0, sizeof(*p));
		p->fd = k ? fd1 : fd0;
		CHECK_EQ(bo_create(p->fd, &p->desc, (n / 2 + 1) * 256u, PANFROST_BO_NOEXEC, 1), 0);
		CHECK_EQ(bo_create(p->fd, &p->data, ((n / 2 + 1) * 16u + 4095u) & ~4095u,
				   PANFROST_BO_NOEXEC, 1), 0);
		CHECK_EQ(bo_create(p->fd, &p->last, 4096, PANFROST_BO_NOEXEC, 1), 0);
		if (!p->desc.cpu || !p->data.cpu || !p->last.cpu)
			return;
		for (int i = 0; i < NSO; i++)
			p->so[i] = syncobj(p->fd, 0);
		p->ctx = ctx_create(p->fd, PANFROST_JM_CTX_PRIORITY_LOW, &e);
		CHECK_EQ(e, 0);
	}
	for (unsigned i = 0; i < n; i++) {
		struct per *p = &pf[i & 1];
		uint32_t slot = p->count++, bos[3] = { p->desc.handle, p->data.handle, p->last.handle };
		uint32_t out = p->so[slot % NSO];
		struct chain c;
		struct sub s;
		int e;

		chain_init(&c, &p->desc);
		p->desc.used = slot * 256u;
		chain_wv(&c, p->data.va + slot * 16u, WV_IMM32, i + 1, 0);
		chain_wv(&c, p->data.va + slot * 16u + 8, WV_SYSTEM_TIMESTAMP, 0, 1);
		chain_wv(&c, p->last.va, WV_IMM32, i + 1, 2);
		s = (struct sub){ .jc = c.first, .bos = bos, .nbos = 3, .out = out,
				  .req = (i % 3 == 2) ? PANFROST_JD_REQ_FS : 0,
				  .ctx = (i % 4 < 2) ? 0 : p->ctx };
		if (p->prev && i % 5 == 0) {
			s.in = &p->prev;
			s.nin = 1;
		}
		e = submit(p->fd, &s);
		if (e) {
			bad++;
			fprintf(stderr, "stress submit %u: errno %d\n", i, e);
		}
		p->prev = out;
		p->last_i = i + 1;
		if (i % 100 == 99) {
			for (int k = 0; k < 2; k++)
				CHECK_EQ(drmSyncobjWait(pf[k].fd, pf[k].so, NSO, INT64_MAX,
							DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL), 0);
		}
	}
	for (int k = 0; k < 2; k++) {
		struct per *p = &pf[k];

		CHECK_EQ(wait_bo(p->fd, p->last.handle, INT64_MAX), 0);
		CHECK_EQ(wait_bo(p->fd, p->data.handle, INT64_MAX), 0);
		for (uint32_t s = 0; s < p->count; s++) {
			uint32_t want = s * 2 + (uint32_t)k + 1, got = get32(p->data.cpu + s * 16u);
			uint64_t ts = get64(p->data.cpu + s * 16u + 8);

			if (s && ts < get64(p->data.cpu + (s - 1) * 16u + 8))
				backwards++;
			if (got != want) {
				if (bad < 10)
					fprintf(stderr, "stress file %d slot %u: %u != %u\n", k, s,
						got, want);
				bad++;
			}
			for (uint32_t j = 0; j < 3; j++)
				if (exc_status(p->desc.cpu + s * 256u + j * 64u) != EXC_DONE)
					failed++;
		}
		CHECK_EQ(get32(p->last.cpu), p->last_i);
		for (int i = 0; i < NSO; i++) {
			int32_t st = 0;

			CHECK_EQ(syncobj_status(p->fd, p->so[i], &st), 0);
			CHECK_EQ(st, 1);
		}
	}
	dt = now_ns() - t0;
	CHECK_EQ(bad, 0);
	CHECK_EQ(failed, 0);
	CHECK_EQ(backwards, 0);
	printf("stress: %u submits over 2 files, JS0+JS1, 2 contexts each: all values and "
	       "descriptors right, %.1f ms (%.0f submits/s)\n", n, dt / 1e6,
	       n / (dt > 0 ? dt / 1e9 : 1.0));
	for (int k = 0; k < 2; k++) {
		struct per *p = &pf[k];

		for (int i = 0; i < NSO; i++)
			CHECK_EQ(drmSyncobjDestroy(p->fd, p->so[i]), 0);
		CHECK_EQ(ctx_destroy(p->fd, p->ctx), 0);
		bo_free(p->fd, &p->desc);
		bo_free(p->fd, &p->data);
		bo_free(p->fd, &p->last);
	}
}

/* File close with work in flight and objects left: a pending long chain, live BOs, a mapping,
 * a syncobj and a context. Everything must be released by the core (leak check at the end). */
static void test_close_inflight(int fd)
{
	struct bo desc, data;
	struct chain c;
	uint32_t so = syncobj(fd, 0), bos[2];
	struct sub s;
	int e;

	CHECK_EQ(bo_create(fd, &desc, 2000 * 64, PANFROST_BO_NOEXEC, 1), 0);
	CHECK_EQ(bo_create(fd, &data, 4096, PANFROST_BO_NOEXEC, 1), 0);
	if (!desc.cpu || !data.cpu)
		return;
	bos[0] = desc.handle;
	bos[1] = data.handle;
	chain_init(&c, &desc);
	for (unsigned i = 0; i < 2000; i++)
		chain_wv(&c, data.va + (i % 64) * 8, WV_IMM64, i, i);
	ctx_create(fd, PANFROST_JM_CTX_PRIORITY_LOW, &e);
	CHECK_EQ(e, 0);
	s = (struct sub){ .jc = c.first, .bos = bos, .nbos = 2, .out = so };
	CHECK_EQ(submit(fd, &s), 0);
	/* The mapping must go before the file (libdrm-win keeps the file alive for it anyway). */
	bo_unmap(&data);
	bo_unmap(&desc);
}

int main(void)
{
	struct panwin_core_report r0, r;
	drmVersionPtr v = NULL;
	char path[64] = "";
	const char *stress = getenv("PANWIN_JM_STRESS");
	unsigned n_stress = stress ? (unsigned)strtoul(stress, NULL, 0) : 1000;
	int fd, fd2, fd3;

	setvbuf(stdout, NULL, _IONBF, 0);
	get_report = (panwin_core_get_report_fn)(void (*)(void))GetProcAddress(
		GetModuleHandleA("libdrm.dll"), PANWIN_CORE_GET_REPORT);
	syncobj_status = (panwin_core_syncobj_status_fn)(void (*)(void))GetProcAddress(
		GetModuleHandleA("libdrm.dll"), PANWIN_CORE_SYNCOBJ_STATUS);
	if (!get_report || !syncobj_status) {
		fprintf(stderr, "libdrm.dll has no core transport (build with -Dcore_transport=true)\n");
		return 1;
	}
	if (strcmp(panwin_transport_name(), "core")) {
		fprintf(stderr, "PANWIN_TRANSPORT=core expected, transport is %s\n",
			panwin_transport_name());
		return 1;
	}
	CHECK_EQ(panwin_device_path(0, path, sizeof(path)), 0);
	if (strcmp(path, "core:mock-g31")) {
		fprintf(stderr, "PANWIN_CORE_MODEL=g31 expected, device is %s\n", path);
		return 1;
	}
	report(&r0);
	CHECK_EQ(r0.device_live, 0);

	fd = open_device(&v);
	if (fd < 0) {
		fprintf(stderr, "FAIL: cannot open the G31\n");
		return 1;
	}
	test_params(fd, v);
	test_bos(fd, v);
	test_submit(fd);
	test_implicit_sync(fd);
	test_faults(fd);
	test_heap(fd);
	test_ctx_madvise_label(fd);
	fd2 = panwin_open("/dev/dri/renderD128", 2);
	CHECK(fd2 >= 0);
	if (fd2 >= 0) {
		if (n_stress)
			test_stress(fd, fd2, n_stress < 8 ? 8 : n_stress);	/* >= NSO per file */
		CHECK_EQ(drmClose(fd2), 0);
	}
	/* a third file closed with a job in flight and objects left behind */
	fd3 = panwin_open("/dev/dri/renderD128", 2);
	CHECK(fd3 >= 0);
	if (fd3 >= 0) {
		test_close_inflight(fd3);
		CHECK_EQ(drmClose(fd3), 0);
	}
	report(&r);
	CHECK_EQ(r.device_live, 1);
	CHECK_EQ(r.open_files, 1);
	CHECK_EQ(r.bringup_failures, r0.bringup_failures);
	drmFreeVersion(v);
	CHECK_EQ(drmClose(fd), 0);

	/* teardown: the last close destroys the device and runs the core's leak check */
	report(&r);
	CHECK_EQ(r.device_live, 0);
	CHECK_EQ(r.open_files, 0);
	CHECK_EQ(r.devices_destroyed, r.devices_created);
	CHECK_EQ(r.leaked_allocs, 0);
	CHECK_EQ(r.leaked_mems, 0);
	CHECK_EQ(r.leaked_sync, 0);
	CHECK_EQ(r.model_errors, 0);
	CHECK_EQ(r.irq_unclaimed, 0);
	CHECK_EQ(r.leaked_maps, 0);
	CHECK_EQ(r.leaked_notifies, 0);
	CHECK_EQ(r.cs_instrs, 0);	/* no CSF on the G31 */
	printf("teardown: device live %u, devices %u up/%u down; leaks: allocs %lld mems %lld "
	       "sync %lld maps %u events %u; mock model errors %u, unclaimed irqs %u; %llu DRM "
	       "ioctls (%llu failed, as tested), %llu mmaps\n", r.device_live, r.devices_created,
	       r.devices_destroyed, (long long)r.leaked_allocs, (long long)r.leaked_mems,
	       (long long)r.leaked_sync, r.leaked_maps, r.leaked_notifies, r.model_errors,
	       r.irq_unclaimed, (unsigned long long)r.drm_ioctls,
	       (unsigned long long)r.drm_ioctl_errors, (unsigned long long)r.mmaps);

	printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "OK", checks, failures);
	return failures ? 1 : 0;
}
