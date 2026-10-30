// SPDX-License-Identifier: MIT
/*
 * test_jm_hw.c - job-manager backend: backend selection, G31 probe, the per-backend ioctl
 * dispatch (jm-arch), and the jm-hw module (docs/status/jm-arch.md tasks A1-A8): GET_PARAM,
 * shader/tiler power-up, kernel-built job chains run by the mock G31 executor through the
 * pw_jm_slot_* helpers, the job IRQ events, page faults, stops, GPU reset and the AS LRU.
 * Owner of the jm-hw part: jm-hw.
 */
#include <pthread.h>
#include <unistd.h>

#include "pw_jm_hw.h"
#include "pw_jm_mem.h"
#include "pw_mmu.h"
#include "test_jm.h"

/* ---- backend selection (pw_backend_for_gpu_id, enum pw_backend) ---- */

static void test_jm_backend_select(void)
{
	static const struct {
		uint32_t gpu_id;
		enum pw_backend want;
	} cases[] = {
		{ 0, PW_BACKEND_NONE },
		{ 0xffffffffu, PW_BACKEND_NONE },
		{ 0x08600000u, PW_BACKEND_NONE },	/* Midgard T860 (arch field 0) */
		{ 0x50000000u, PW_BACKEND_NONE },	/* Midgard arch 5 */
		{ 0x60000000u, PW_BACKEND_JM },		/* G71 */
		{ MOCK_G31_GPU_ID, PW_BACKEND_JM },	/* G31 */
		{ 0x72120000u, PW_BACKEND_JM },		/* G52 */
		{ 0x92220000u, PW_BACKEND_NONE },	/* Valhall JM (G57/G68): out of scope */
		{ MOCK_G610_GPU_ID, PW_BACKEND_CSF },
		{ 0xd0000000u, PW_BACKEND_CSF },	/* arch 13 */
		{ 0xe8000000u, PW_BACKEND_NONE },	/* arch 14: PWR_CONTROL, not done */
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (pw_backend_for_gpu_id(cases[i].gpu_id) != cases[i].want) {
			fprintf(stderr, "    GPU_ID 0x%08x\n", cases[i].gpu_id);
			CHECK_EQ(pw_backend_for_gpu_id(cases[i].gpu_id), cases[i].want);
		}
	}
	CHECK_EQ(pw_device_backend(NULL), PW_BACKEND_NONE);
}

/* ---- G31 probe through the shared bring-up ---- */

static void test_jm_probe_g31(void)
{
	struct jenv e;
	struct drm_panthor_gpu_info gi;
	struct drm_panthor_csif_info ci;
	uint32_t major = 0, minor = 0, patch = 1;
	const char *name = NULL;
	char desc[160];

	if (!jenv_init(&e))
		goto out;
	CHECK_EQ(pw_device_backend(e.dev), PW_BACKEND_JM);
	CHECK(!pw_device_fw_booted(e.dev));
	CHECK(mock_log_contains(e.os, "Mali-G31 id 0x7093"));
	CHECK(mock_log_contains(e.os, "job manager initialised"));
	/* JM soft reset encoding (GPU_CMD 0x01); a CSF 0x101 would be a model error */
	CHECK(mock_soft_resets(e.os) >= 1);
	CHECK(e.dev->gpu.l2_on);

	pw_device_get_gpu_info(e.dev, &gi);
	CHECK_EQ(gi.gpu_id, MOCK_G31_GPU_ID);
	CHECK_EQ(gi.csf_id, 0);			/* 0x1C is JS_PRESENT on JM */
	CHECK_EQ(gi.gpu_features, 0);
	CHECK_EQ(gi.shader_present, MOCK_G31_SHADER_PRESENT);
	CHECK_EQ(gi.as_present, MOCK_G31_AS_PRESENT);
	CHECK_EQ(gi.selected_coherency, GPU_COHERENCY_NONE);
	CHECK_EQ(pw_device_get_csif_info(e.dev, &ci), PW_ENOENT);

	pw_device_drm_version(e.dev, &major, &minor, &patch, &name);
	CHECK_EQ(major, PW_JM_DRM_MAJOR);
	CHECK_EQ(minor, PW_JM_DRM_MINOR);
	CHECK_EQ(patch, PW_JM_DRM_PATCH);
	CHECK(name && !strcmp(name, "panfrost"));

	pw_device_describe(e.dev, desc, sizeof(desc));
	CHECK(strstr(desc, "Mali-G31 r0p0 arch7.0") != NULL);
	CHECK(strstr(desc, "2 cores") != NULL);
	CHECK(strstr(desc, "job manager") != NULL);

	/* GPU cache flush uses the JM command (CLEAN_INV_CACHES = 8) */
	CHECK_EQ(pw_gpu_flush_caches(e.dev, CACHE_CLEAN | CACHE_INV, CACHE_CLEAN | CACHE_INV,
				     CACHE_INV), PW_OK);
	CHECK_EQ(mock_last_flush_cmd(e.os), 8);
	CHECK_EQ(pw_gpu_flush_caches(e.dev, CACHE_CLEAN, CACHE_CLEAN, 0), PW_OK);
	CHECK_EQ(mock_last_flush_cmd(e.os), 7);
out:
	jenv_fini(&e);
}

/* The CSF device keeps reporting panthor (pw_device_drm_version on CSF == pw_core_drm_version). */
static void test_jm_csf_version_unchanged(void)
{
	struct mock_config cfg = { 0 };
	struct pw_device_params p = { 0 };
	struct pw_device *dev;
	struct pw_os *os;
	uint32_t maj, min, pat, cmaj, cmin, cpat;
	const char *name = NULL;

	os = mock_create(&cfg);
	p.os = os;
	p.boot_firmware = false;	/* M1 mode is enough: the backend is chosen from GPU_ID */
	CHECK_EQ(pw_device_create(&p, &dev), PW_OK);
	CHECK_EQ(pw_device_backend(dev), PW_BACKEND_CSF);
	pw_device_drm_version(dev, &maj, &min, &pat, &name);
	pw_core_drm_version(&cmaj, &cmin, &cpat);
	CHECK_EQ(maj, cmaj);
	CHECK_EQ(min, cmin);
	CHECK_EQ(pat, cpat);
	CHECK(name && !strcmp(name, "panthor"));
	pw_device_destroy(dev);
	check_no_leaks(os);
	CHECK_EQ(mock_model_errors(os), 0);
	mock_destroy(os);
}

/* ---- per-backend dispatch on a JM file ---- */

static void test_jm_dispatch(void)
{
	struct jenv e;
	struct drm_version v;
	struct drm_get_cap cap;
	struct drm_syncobj_create sc;
	struct drm_syncobj_destroy sd;
	struct drm_syncobj_timeline_array ta;
	struct drm_gem_close gc;
	struct pw_mmap_target t;
	char name[16], desc[64];
	uint8_t junk[64];

	if (!jenv_init(&e))
		goto out;

	/* VERSION: "panfrost" with the emulated panfrost version */
	memset(&v, 0, sizeof(v));
	memset(name, 'x', sizeof(name));
	memset(desc, 0, sizeof(desc));
	v.name = name;
	v.name_len = sizeof(name);
	v.desc = desc;
	v.desc_len = sizeof(desc) - 1;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_VERSION, &v), PW_OK);
	CHECK_EQ(v.version_major, PW_JM_DRM_MAJOR);
	CHECK_EQ(v.version_minor, PW_JM_DRM_MINOR);
	CHECK_EQ(v.name_len, 8);
	CHECK(!memcmp(name, "panfrostx", 9));
	CHECK(!strncmp(desc, "panfrost DRM", 12));

	/* GET_CAP: panfrost is DRIVER_SYNCOBJ without DRIVER_SYNCOBJ_TIMELINE */
	memset(&cap, 0, sizeof(cap));
	cap.capability = DRM_CAP_SYNCOBJ;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_GET_CAP, &cap), PW_OK);
	CHECK_EQ(cap.value, 1);
	cap.capability = DRM_CAP_SYNCOBJ_TIMELINE;
	cap.value = 77;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_GET_CAP, &cap), PW_OK);
	CHECK_EQ(cap.value, 0);

	/* binary syncobjs are shared code and work; timeline ioctls are EOPNOTSUPP */
	memset(&sc, 0, sizeof(sc));
	CHECK_EQ(JIOC(&e, DRM_IOCTL_SYNCOBJ_CREATE, &sc), PW_OK);
	CHECK(sc.handle != 0);
	memset(&ta, 0, sizeof(ta));
	CHECK_EQ(JIOC(&e, DRM_IOCTL_SYNCOBJ_QUERY, &ta), PW_EOPNOTSUPP);
	CHECK_EQ(JIOC(&e, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &ta), PW_EOPNOTSUPP);
	CHECK_EQ(pw_file_ioctl(e.file, _IOC_NR(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT), junk,
			       sizeof(struct drm_syncobj_timeline_wait)), PW_EOPNOTSUPP);
	CHECK_EQ(pw_file_ioctl(e.file, _IOC_NR(DRM_IOCTL_SYNCOBJ_TRANSFER), junk,
			       sizeof(struct drm_syncobj_transfer)), PW_EOPNOTSUPP);
	memset(&sd, 0, sizeof(sd));
	sd.handle = sc.handle;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_SYNCOBJ_DESTROY, &sd), PW_OK);

	/* GEM_CLOSE is shared: unknown handle -> EINVAL */
	memset(&gc, 0, sizeof(gc));
	gc.handle = 1;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_GEM_CLOSE, &gc), PW_EINVAL);

	/* panthor driver ioctls do not exist on a panfrost file: DEV_QUERY (0x40) is SUBMIT
	 * here, VM_CREATE (0x41) is WAIT_BO, ... and 0x4e+ are unknown */
	CHECK_EQ(pw_file_ioctl(e.file, 0x40 + 0x0e, junk, 8), PW_ENOTTY);
	CHECK_EQ(pw_file_ioctl(e.file, 0x40 + 0x20, junk, 8), PW_ENOTTY);
	/* unstable perf-counter ioctls: ENOSYS like Linux without unstable_ioctls */
	CHECK_EQ(pw_file_ioctl(e.file, _IOC_NR(DRM_IOCTL_PANFROST_PERFCNT_ENABLE), junk, 8),
		 PW_ENOSYS);
	CHECK_EQ(pw_file_ioctl(e.file, _IOC_NR(DRM_IOCTL_PANFROST_PERFCNT_DUMP), junk, 8),
		 PW_ENOSYS);
	/* 1.6 ioctls are gated off at the reported 1.5 */
	CHECK_EQ(pw_file_ioctl(e.file, _IOC_NR(DRM_IOCTL_PANFROST_SYNC_BO), junk,
			       sizeof(struct drm_panfrost_sync_bo)), PW_ENOTTY);
	CHECK_EQ(pw_file_ioctl(e.file, _IOC_NR(DRM_IOCTL_PANFROST_QUERY_BO_INFO), junk,
			       sizeof(struct drm_panfrost_query_bo_info)), PW_ENOTTY);

	/* no user-MMIO window on JM (panthor's LATEST_FLUSH_ID page) */
	CHECK_EQ(pw_file_mmap_lookup(e.file, DRM_PANTHOR_USER_MMIO_OFFSET_64BIT, 4096, &t),
		 PW_EINVAL);
	CHECK_EQ(pw_file_mmap_lookup(e.file, DRM_PANTHOR_USER_MMIO_OFFSET_32BIT, 4096, &t),
		 PW_EINVAL);
out:
	jenv_fini(&e);
}

/* ---- the MMU fault path reaches the JM hook, then the generic unhandled path ---- */

static void test_jm_unhandled_fault(void)
{
	struct jenv e;
	struct pw_vm *vm = NULL;
	struct mock_as_state as;

	if (!jenv_init(&e))
		goto out;
	CHECK_EQ(pw_vm_create(e.dev, false, &vm), PW_OK);
	pw_os_mutex_lock(vm->op_lock);
	pw_os_mutex_lock(e.dev->mmu.slots_lock);
	CHECK_EQ(pw_mmu_as_bind_locked(vm, 0), PW_OK);	/* AS0 is a normal slot on JM */
	pw_os_mutex_unlock(e.dev->mmu.slots_lock);
	pw_os_mutex_unlock(vm->op_lock);
	mock_get_as(e.os, 0, &as);
	CHECK(as.enabled);

	mock_inject_mmu_fault(e.os, 0, 0x000002c1u, 0x2001000ull);	/* translation, read */
	for (int i = 0; i < 200 && !mock_log_contains(e.os, "Unhandled page fault in AS0"); i++)
		usleep(5000);
	CHECK(mock_log_contains(e.os, "Unhandled page fault in AS0 at VA 0x0000000002001000"));
	/* (jm-hw) the message is logged before the handler disables the AS: wait for that */
	for (int i = 0; i < 200; i++) {
		mock_get_as(e.os, 0, &as);
		if (!as.enabled)
			break;
		usleep(1000);
	}
	CHECK(!as.enabled);
	CHECK_EQ(e.dev->mmu.faulty_mask, 1);

	pw_os_mutex_lock(vm->op_lock);
	pw_os_mutex_lock(e.dev->mmu.slots_lock);
	CHECK_EQ(pw_mmu_as_bind_locked(vm, 0), PW_OK);	/* re-enables the faulty slot */
	CHECK_EQ(e.dev->mmu.faulty_mask, 0);
	pw_mmu_as_unbind_locked(vm);
	CHECK_EQ(pw_vm_as(vm), -1);
	pw_os_mutex_unlock(e.dev->mmu.slots_lock);
	pw_os_mutex_unlock(vm->op_lock);
	mock_get_as(e.os, 0, &as);
	CHECK(!as.enabled);
	pw_vm_destroy(vm);
out:
	jenv_fini(&e);
}

/* ---- jm-hw (tasks A1-A8 in jm-arch.md) ---- */

/* Event capture: jobs here are submitted with the pw_jm_slot_* helpers directly, so their
 * events go to a test hook instead of the scheduler (pw_jm_hw_set_irq_hook). */
struct evlog {
	pthread_mutex_t m;
	unsigned int n;
	struct pw_jm_irq_event ev[64];
};

static void evlog_hook(struct pw_device *dev, const struct pw_jm_irq_event *ev, void *ctx)
{
	struct evlog *l = ctx;

	(void)dev;
	pthread_mutex_lock(&l->m);
	if (l->n < PW_ARRAY_SIZE(l->ev))
		l->ev[l->n] = *ev;
	l->n++;
	pthread_mutex_unlock(&l->m);
}

static unsigned int evlog_count(struct evlog *l)
{
	unsigned int n;

	pthread_mutex_lock(&l->m);
	n = l->n;
	pthread_mutex_unlock(&l->m);
	return n;
}

/* Wait for the event number @idx (0-based); copies it to *out. */
static bool evlog_wait(struct evlog *l, unsigned int idx, struct pw_jm_irq_event *out)
{
	for (int i = 0; i < 2000; i++) {
		pthread_mutex_lock(&l->m);
		if (l->n > idx) {
			*out = l->ev[idx];
			pthread_mutex_unlock(&l->m);
			return true;
		}
		pthread_mutex_unlock(&l->m);
		usleep(1000);
	}
	CHECK(!"timeout waiting for a job event");
	memset(out, 0, sizeof(*out));
	return false;
}

static void evlog_attach(struct jenv *e, struct evlog *l)
{
	memset(l, 0, sizeof(*l));
	pthread_mutex_init(&l->m, NULL);
	pw_jm_hw_set_irq_hook(e->dev, evlog_hook, l);
}

static void evlog_detach(struct jenv *e, struct evlog *l)
{
	if (e->dev)
		pw_jm_hw_set_irq_hook(e->dev, NULL, NULL);
	pthread_mutex_destroy(&l->m);
}

/* A kernel-built address space: a bare pw_vm with one uncached buffer mapped at @va, bound
 * through the LRU with pw_jm_as_get_vm (the key is the struct's address). */
struct kctx {
	struct pw_vm *vm;
	struct pw_os_mem *mem;
	uint8_t *cpu;
	uint64_t va;
	size_t size;
};

#define KCTX_VA		0x10000000ull
#define KCTX_SIZE	0x10000u

static bool kctx_init(struct jenv *e, struct kctx *k)
{
	memset(k, 0, sizeof(*k));
	k->va = KCTX_VA;
	k->size = KCTX_SIZE;
	if (pw_vm_create(e->dev, false, &k->vm) ||
	    pw_os_mem_alloc(e->os, k->size, PW_MEM_KMAP | PW_MEM_UNCACHED, &k->mem)) {
		CHECK(!"kctx alloc failed");
		return false;
	}
	k->cpu = pw_os_mem_kva(k->mem);
	memset(k->cpu, 0, k->size);
	if (pw_vm_map_mem(k->vm, k->va, k->mem, 0, k->size, 0)) {
		CHECK(!"kctx map failed");
		return false;
	}
	return true;
}

static struct pw_jm_vm *kkey(struct kctx *k)
{
	return (struct pw_jm_vm *)(void *)k;	/* never dereferenced by pw_jm_hw.c */
}

static void kctx_fini(struct jenv *e, struct kctx *k)
{
	pw_jm_as_release(e->dev, kkey(k));
	if (k->vm)
		pw_vm_destroy(k->vm);
	if (k->mem)
		pw_os_mem_free(e->os, k->mem);
	memset(k, 0, sizeof(*k));
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

static uint64_t get64(const uint8_t *p)
{
	uint64_t v;

	memcpy(&v, p, 8);
	return v;
}

/* Mesa genxml v7.xml "Job Header" (32 bytes) and the WRITE_VALUE payload behind it. */
enum { JT_NULL = 1, JT_WRITE_VALUE = 2, JT_CACHE_FLUSH = 3, JT_COMPUTE = 4, JT_VERTEX = 5,
       JT_TILER = 7, JT_FRAGMENT = 9, JT_INDEXED_VERTEX = 10 };
enum { WV_CYCLE = 1, WV_TIMESTAMP = 2, WV_ZERO = 3, WV_IMM8 = 4, WV_IMM16 = 5, WV_IMM32 = 6,
       WV_IMM64 = 7 };

/* Job @slot of the context: CPU pointer and GPU VA (128 bytes per job). */
static uint8_t *kjob(struct kctx *k, unsigned int slot, uint64_t *va)
{
	if (va)
		*va = k->va + slot * 128u;
	return k->cpu + slot * 128u;
}

static uint64_t kjob_va(struct kctx *k, unsigned int slot)
{
	return k->va + slot * 128u;
}

static void job_hdr(uint8_t *p, uint32_t type, uint32_t index, uint32_t dep1, uint32_t dep2,
		    uint64_t next)
{
	memset(p, 0, 32);
	put32(p + 16, 1u | (type << 1) | (index << 16));	/* Is 64b, Type, Index */
	put32(p + 20, dep1 | (dep2 << 16));
	put64(p + 24, next);
}

static void job_wv(uint8_t *p, uint32_t index, uint64_t next, uint64_t addr, uint32_t wtype,
		   uint64_t imm)
{
	job_hdr(p, JT_WRITE_VALUE, index, 0, 0, next);
	memset(p + 32, 0, 32);
	put64(p + 32, addr);
	put32(p + 40, wtype);
	put64(p + 48, imm);
}

/* Data area of the context, after 64 job slots. */
#define KDATA_OFF	0x2000u

static void test_jm_get_param(void)
{
	static const struct {
		uint32_t param;
		uint64_t want;
	} cases[] = {
		{ DRM_PANFROST_PARAM_GPU_PROD_ID, MOCK_G31_GPU_ID >> 16 },
		{ DRM_PANFROST_PARAM_GPU_REVISION, MOCK_G31_GPU_ID & 0xffff },
		{ DRM_PANFROST_PARAM_SHADER_PRESENT, MOCK_G31_SHADER_PRESENT },
		{ DRM_PANFROST_PARAM_TILER_PRESENT, MOCK_G31_TILER_PRESENT },
		{ DRM_PANFROST_PARAM_L2_PRESENT, MOCK_G31_L2_PRESENT },
		{ DRM_PANFROST_PARAM_STACK_PRESENT, MOCK_G31_STACK_PRESENT },
		{ DRM_PANFROST_PARAM_AS_PRESENT, MOCK_G31_AS_PRESENT },
		{ DRM_PANFROST_PARAM_JS_PRESENT, MOCK_G31_JS_PRESENT },
		{ DRM_PANFROST_PARAM_L2_FEATURES, MOCK_G31_L2_FEATURES },
		{ DRM_PANFROST_PARAM_CORE_FEATURES, MOCK_G31_CORE_FEATURES },
		{ DRM_PANFROST_PARAM_TILER_FEATURES, MOCK_G31_TILER_FEATURES },
		{ DRM_PANFROST_PARAM_MEM_FEATURES, MOCK_G31_MEM_FEATURES },
		{ DRM_PANFROST_PARAM_MMU_FEATURES, MOCK_G31_MMU_FEATURES },
		{ DRM_PANFROST_PARAM_THREAD_FEATURES, MOCK_G31_THREAD_FEATURES },
		{ DRM_PANFROST_PARAM_MAX_THREADS, MOCK_G31_MAX_THREADS },
		{ DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ, MOCK_G31_MAX_WG_SIZE },
		{ DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ, MOCK_G31_MAX_BARRIER },
		{ DRM_PANFROST_PARAM_COHERENCY_FEATURES, MOCK_G31_COHERENCY_FEATURES },
		{ DRM_PANFROST_PARAM_TEXTURE_FEATURES0, MOCK_G31_TEXTURE_FEATURES0 },
		{ DRM_PANFROST_PARAM_TEXTURE_FEATURES1, 0 },
		{ DRM_PANFROST_PARAM_TEXTURE_FEATURES3, 0 },
		{ DRM_PANFROST_PARAM_JS_FEATURES0, MOCK_G31_JS0_FEATURES },
		{ DRM_PANFROST_PARAM_JS_FEATURES1, MOCK_G31_JS1_FEATURES },
		{ DRM_PANFROST_PARAM_JS_FEATURES2, MOCK_G31_JS2_FEATURES },
		{ DRM_PANFROST_PARAM_JS_FEATURES3, 0 },	/* past js_present */
		{ DRM_PANFROST_PARAM_JS_FEATURES15, 0 },
		{ DRM_PANFROST_PARAM_NR_CORE_GROUPS, 1 },
		{ DRM_PANFROST_PARAM_THREAD_TLS_ALLOC, MOCK_G31_THREAD_TLS_ALLOC },
		{ DRM_PANFROST_PARAM_AFBC_FEATURES, MOCK_G31_AFBC_FEATURES },
		{ DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP_FREQUENCY, 24000000 },
		{ DRM_PANFROST_PARAM_ALLOWED_JM_CTX_PRIORITIES,
		  (1u << PANFROST_JM_CTX_PRIORITY_LOW) | (1u << PANFROST_JM_CTX_PRIORITY_MEDIUM) },
		{ DRM_PANFROST_PARAM_SELECTED_COHERENCY, DRM_PANFROST_GPU_COHERENCY_NONE },
	};
	struct jenv e;
	struct drm_panfrost_get_param gp;
	uint64_t t1, t2;

	if (!jenv_init(&e))
		goto out;
	for (size_t i = 0; i < PW_ARRAY_SIZE(cases); i++) {
		memset(&gp, 0, sizeof(gp));
		gp.param = cases[i].param;
		gp.value = 0xbadbadull;
		CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_GET_PARAM, &gp), PW_OK);
		if (gp.value != cases[i].want) {
			fprintf(stderr, "    param %u\n", cases[i].param);
			CHECK_EQ(gp.value, cases[i].want);
		}
	}
	/* every param of the uAPI enum is known */
	for (uint32_t p = 0; p <= DRM_PANFROST_PARAM_SELECTED_COHERENCY; p++) {
		memset(&gp, 0, sizeof(gp));
		gp.param = p;
		if (JIOC(&e, DRM_IOCTL_PANFROST_GET_PARAM, &gp) != PW_OK) {
			fprintf(stderr, "    param %u\n", p);
			CHECK(!"param refused");
		}
	}

	/* SYSTEM_TIMESTAMP: GPU_TIMESTAMP with the cycle counter held only around the read */
	memset(&gp, 0, sizeof(gp));
	gp.param = DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_GET_PARAM, &gp), PW_OK);
	t1 = gp.value;
	usleep(2000);
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_GET_PARAM, &gp), PW_OK);
	t2 = gp.value;
	CHECK(t1 > 0);
	CHECK(t2 > t1 + 24000);		/* >= 1 ms at 24 MHz */
	CHECK_EQ(mock_reg(e.os, GPU_STATUS) & GPU_STATUS_CYCLE_COUNT_ACTIVE, 0);

	/* errors */
	memset(&gp, 0, sizeof(gp));
	gp.param = DRM_PANFROST_PARAM_GPU_PROD_ID;
	gp.pad = 1;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_GET_PARAM, &gp), PW_EINVAL);
	gp.pad = 0;
	gp.param = DRM_PANFROST_PARAM_SELECTED_COHERENCY + 1;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_GET_PARAM, &gp), PW_EINVAL);
	gp.param = 0xffffffffu;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_GET_PARAM, &gp), PW_EINVAL);
out:
	jenv_fini(&e);
}

static void test_jm_power_up(void)
{
	struct jenv e;
	const struct pw_jm_gpu_info *info;

	if (!jenv_init(&e))
		goto out;
	info = pw_jm_gpu_info(e.dev);
	REQUIRE(info);
	CHECK_EQ(info->gpu_id, MOCK_G31_GPU_ID);
	CHECK_EQ(info->js_present, MOCK_G31_JS_PRESENT);
	CHECK_EQ(info->js_features[1], MOCK_G31_JS1_FEATURES);
	CHECK_EQ(info->stack_present, MOCK_G31_STACK_PRESENT);
	CHECK_EQ(info->nr_core_groups, 1);
	CHECK(mock_log_contains(e.os, "mali-g31 id 0x7093 major 0x0 minor 0x0 status 0x0"));
	CHECK(mock_log_contains(e.os, "JS:0x7"));

	/* panfrost_gpu_power_on: L2 (shared code), shader cores, tiler */
	CHECK_EQ(gpu_read(e.dev, L2_READY), MOCK_G31_L2_PRESENT);
	CHECK_EQ(mock_reg(e.os, SHADER_READY), MOCK_G31_SHADER_PRESENT);
	CHECK_EQ(mock_reg(e.os, TILER_READY), MOCK_G31_TILER_PRESENT);
	/* G31 quirk: TLS hashing */
	CHECK_EQ(mock_reg(e.os, GPU_SHADER_CONFIG), SC_TLS_HASH_ENABLE);
	CHECK_EQ(mock_reg(e.os, GPU_JM_CONFIG), 0);
	/* job IRQ: done | err of JS0-JS2 */
	CHECK_EQ(mock_int_mask(e.os, 0), 0x00070007u);
	CHECK_EQ(pw_jm_slot_affinity(e.dev), MOCK_G31_SHADER_PRESENT);
	for (uint32_t js = 0; js < PW_JM_NUM_SLOTS; js++)
		CHECK(pw_jm_slot_next_free(e.dev, js));
	CHECK(!pw_jm_slot_next_free(e.dev, 3));

	/* fini powers off and masks; twice is fine; destroy runs it a third time */
	pw_jm_hw_fini(e.dev);
	pw_jm_hw_fini(e.dev);
	CHECK_EQ(mock_reg(e.os, SHADER_READY), 0);
	CHECK_EQ(mock_reg(e.os, TILER_READY), 0);
	CHECK_EQ(mock_int_mask(e.os, 0), 0);
out:
	jenv_fini(&e);
}

static void jm_probe_fails(const struct mock_config *cfg, int want, const char *log)
{
	struct pw_device_params p = { 0 };
	struct mock_config c = *cfg;
	struct pw_device *dev;
	struct pw_os *os;

	c.model = MOCK_MODEL_G31;
	os = mock_create(&c);
	p.os = os;
	CHECK_EQ(pw_device_alloc(&p, &dev), PW_OK);
	mock_attach_irq(os, dev);
	CHECK_EQ(pw_device_probe(dev), want);
	CHECK(mock_log_contains(os, log));
	/* (review) cores powered before the failing block are off again */
	CHECK_EQ(mock_reg(os, SHADER_READY), 0);
	CHECK_EQ(mock_reg(os, TILER_READY), 0);
	mock_detach_irq(os);
	pw_device_destroy(dev);
	check_no_leaks(os);
	CHECK_EQ(mock_model_errors(os), 0);
	mock_destroy(os);
}

static void test_jm_init_failures(void)
{
	struct mock_config cfg = { 0 };

	cfg.no_shader_ready = true;
	jm_probe_fails(&cfg, PW_ETIMEDOUT, "error powering up gpu shader");
	cfg.no_shader_ready = false;
	cfg.no_tiler_ready = true;
	jm_probe_fails(&cfg, PW_ETIMEDOUT, "error powering up gpu tiler");
}

/* Submit @jc on @js through the LRU binding of @k; returns the AS. */
static int ksubmit(struct jenv *e, struct kctx *k, uint32_t js, uint64_t jc, bool chain_flag)
{
	int as = pw_jm_as_get_vm(e->dev, kkey(k), k->vm);

	CHECK(as >= 0);
	if (as < 0)
		return as;
	pw_jm_slot_submit(e->dev, js, jc, (uint32_t)as, pw_jm_latest_flush_id(e->dev), chain_flag);
	return as;
}

static void test_jm_write_value_job(void)
{
	struct jenv e;
	struct evlog l;
	struct kctx k;
	struct pw_jm_irq_event ev;
	struct mock_js_stats st;
	uint8_t *d;
	uint64_t dva, prev_ts;
	uint32_t flush_id;
	int as;

	if (!jenv_init(&e))
		goto out;
	evlog_attach(&e, &l);
	if (!kctx_init(&e, &k))
		goto out_k;
	d = k.cpu + KDATA_OFF;
	dva = k.va + KDATA_OFF;
	memset(d, 0xff, 64);

	job_wv(kjob(&k, 0, NULL), 1, kjob_va(&k, 1), dva + 0, WV_IMM32, 0x1122334455667788ull);
	job_wv(kjob(&k, 1, NULL), 2, kjob_va(&k, 2), dva + 8, WV_IMM64, 0x0102030405060708ull);
	job_wv(kjob(&k, 2, NULL), 3, kjob_va(&k, 3), dva + 16, WV_IMM8, 0xa5);
	job_wv(kjob(&k, 3, NULL), 4, kjob_va(&k, 4), dva + 24, WV_IMM16, 0xbeef);
	job_wv(kjob(&k, 4, NULL), 5, kjob_va(&k, 5), dva + 32, WV_ZERO, 0x77);
	job_wv(kjob(&k, 5, NULL), 6, kjob_va(&k, 6), dva + 40, WV_TIMESTAMP, 0);
	job_wv(kjob(&k, 6, NULL), 7, 0, dva + 48, WV_CYCLE, 0);

	pw_jm_cycle_counter_get(e.dev);
	CHECK(mock_reg(e.os, GPU_STATUS) & GPU_STATUS_CYCLE_COUNT_ACTIVE);
	flush_id = pw_jm_latest_flush_id(e.dev);
	as = ksubmit(&e, &k, 1, k.va, false);
	REQUIRE(as >= 0);
	evlog_wait(&l, 0, &ev);
	CHECK_EQ(ev.done, PW_BIT(1));
	CHECK_EQ(ev.failed, 0);
	CHECK_EQ(ev.js_state, 0);
	pw_jm_as_put(e.dev, kkey(&k));
	pw_jm_cycle_counter_put(e.dev);
	CHECK_EQ(mock_reg(e.os, GPU_STATUS) & GPU_STATUS_CYCLE_COUNT_ACTIVE, 0);

	/* the GPU wrote memory (seen through "DRAM") */
	CHECK_EQ(get32(d + 0), 0x55667788u);
	CHECK_EQ(get32(d + 4), 0xffffffffu);
	CHECK_EQ(get64(d + 8), 0x0102030405060708ull);
	CHECK_EQ(d[16], 0xa5);
	CHECK_EQ(d[17], 0xff);
	CHECK_EQ(get32(d + 24), 0xffffbeefu);
	CHECK_EQ(get64(d + 32), 0);
	prev_ts = get64(d + 40);
	CHECK(prev_ts != 0 && prev_ts != ~0ull);
	CHECK(get64(d + 48) != 0 && get64(d + 48) != ~0ull);
	/* each descriptor got exception status DONE */
	for (unsigned int i = 0; i < 7; i++)
		CHECK_EQ(get32(kjob(&k, i, NULL)), 1);

	/* the registers the helper programmed (panfrost_job_hw_submit) */
	mock_jm_js_stats(e.os, 1, &st);
	CHECK_EQ(st.starts, 1);
	CHECK_EQ(st.runs, 1);
	CHECK_EQ(st.done, 1);
	CHECK_EQ(st.jobs, 7);
	CHECK_EQ(st.write_values, 7);
	CHECK_EQ(st.last_head, k.va);
	CHECK_EQ(st.last_config, JS_CONFIG_ADDRESS_SPACE(as) | JS_CONFIG_THREAD_PRI(8) |
				 JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE |
				 JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE |
				 JS_CONFIG_ENABLE_FLUSH_REDUCTION);
	CHECK_EQ(st.last_affinity, MOCK_G31_SHADER_PRESENT);
	CHECK_EQ(st.last_flush_id, flush_id);
	CHECK_EQ(st.start_flushes_skipped, 1);	/* nothing flushed since the id was sampled */
	CHECK_EQ(st.end_flushes, 1);
	CHECK(pw_jm_latest_flush_id(e.dev) != flush_id);
	CHECK_EQ(pw_jm_slot_status(e.dev, 1), PW_JM_EXC_DONE);
	CHECK(pw_jm_slot_next_free(e.dev, 1));
	CHECK(!st.running);

	/* JOB_CHAIN_FLAG; a second run of the same chain */
	job_wv(kjob(&k, 0, NULL), 1, 0, dva + 0, WV_IMM32, 0xcafe);
	as = ksubmit(&e, &k, 1, k.va, true);
	evlog_wait(&l, 1, &ev);
	CHECK_EQ(ev.done, PW_BIT(1));
	pw_jm_as_put(e.dev, kkey(&k));
	mock_jm_js_stats(e.os, 1, &st);
	CHECK(st.last_config & JS_CONFIG_JOB_CHAIN_FLAG);
	CHECK_EQ(get32(d), 0xcafe);
out_k:
	kctx_fini(&e, &k);
	evlog_detach(&e, &l);
out:
	jenv_fini(&e);
}

static void test_jm_chain_deps(void)
{
	struct jenv e;
	struct evlog l;
	struct kctx k;
	struct pw_jm_irq_event ev;
	struct mock_js_stats st;
	struct mock_jm_job_rec rec[16];
	uint64_t dva;
	unsigned int n;

	if (!jenv_init(&e))
		goto out;
	evlog_attach(&e, &l);
	if (!kctx_init(&e, &k))
		goto out_k;
	dva = k.va + KDATA_OFF;

	/* JS1: null -> write value (dep 1) -> cache flush (deps 1, 2) -> compute (dep 3) ->
	 * vertex -> tiler (dep 5) -> indexed vertex (deps 6, 2) */
	job_hdr(kjob(&k, 0, NULL), JT_NULL, 1, 0, 0, kjob_va(&k, 1));
	job_wv(kjob(&k, 1, NULL), 2, kjob_va(&k, 2), dva, WV_IMM32, 42);
	put32(kjob(&k, 1, NULL) + 20, 1);	/* dependency 1 = job 1 */
	job_hdr(kjob(&k, 2, NULL), JT_CACHE_FLUSH, 3, 1, 2, kjob_va(&k, 3));
	put32(kjob(&k, 2, NULL) + 32, 0x00030003u);	/* clean+invalidate LS and L2 */
	job_hdr(kjob(&k, 3, NULL), JT_COMPUTE, 4, 3, 0, kjob_va(&k, 4));
	job_hdr(kjob(&k, 4, NULL), JT_VERTEX, 5, 0, 0, kjob_va(&k, 5));
	job_hdr(kjob(&k, 5, NULL), JT_TILER, 6, 5, 0, kjob_va(&k, 6));
	job_hdr(kjob(&k, 6, NULL), JT_INDEXED_VERTEX, 7, 6, 2, 0);
	mock_jm_job_log_clear(e.os, 1);
	ksubmit(&e, &k, 1, k.va, false);
	evlog_wait(&l, 0, &ev);
	CHECK_EQ(ev.done, PW_BIT(1));
	CHECK_EQ(ev.failed, 0);
	pw_jm_as_put(e.dev, kkey(&k));
	n = mock_jm_job_log(e.os, 1, rec, 16);
	CHECK_EQ(n, 7);
	for (unsigned int i = 0; i < n; i++) {
		CHECK_EQ(rec[i].va, kjob_va(&k, i));
		CHECK_EQ(rec[i].index, i + 1);
		CHECK_EQ(rec[i].status, 1);
		CHECK_EQ(get32(kjob(&k, i, NULL)), 1);
	}
	CHECK_EQ(get32(k.cpu + KDATA_OFF), 42);
	mock_jm_js_stats(e.os, 1, &st);
	CHECK_EQ(st.null_jobs, 1);
	CHECK_EQ(st.write_values, 1);
	CHECK_EQ(st.cache_flushes, 1);
	CHECK_EQ(st.noop_jobs, 4);

	/* JS0: a fragment job */
	job_hdr(kjob(&k, 8, NULL), JT_FRAGMENT, 1, 0, 0, 0);
	ksubmit(&e, &k, 0, kjob_va(&k, 8), false);
	evlog_wait(&l, 1, &ev);
	CHECK_EQ(ev.done, PW_BIT(0));
	pw_jm_as_put(e.dev, kkey(&k));

	/* a dependency on a job later in the chain would deadlock the JM: config fault */
	job_hdr(kjob(&k, 10, NULL), JT_NULL, 1, 2, 0, kjob_va(&k, 11));
	job_hdr(kjob(&k, 11, NULL), JT_NULL, 2, 0, 0, 0);
	ksubmit(&e, &k, 1, kjob_va(&k, 10), false);
	evlog_wait(&l, 2, &ev);
	CHECK_EQ(ev.done, 0);
	CHECK_EQ(ev.failed, PW_BIT(1));
	CHECK_EQ(ev.status[1], PW_JM_EXC_JOB_CONFIG_FAULT);
	CHECK_EQ(ev.head[1], kjob_va(&k, 10));
	CHECK_EQ(get32(kjob(&k, 10, NULL)), PW_JM_EXC_JOB_CONFIG_FAULT);
	CHECK(mock_log_contains(e.os, "js fault, js=1, status=JOB_CONFIG_FAULT"));
	pw_jm_as_put(e.dev, kkey(&k));

	/* a fragment job on JS1 (not in JS1's JS_FEATURES), a type-0 header: config faults */
	job_hdr(kjob(&k, 12, NULL), JT_FRAGMENT, 1, 0, 0, 0);
	ksubmit(&e, &k, 1, kjob_va(&k, 12), false);
	evlog_wait(&l, 3, &ev);
	CHECK_EQ(ev.status[1], PW_JM_EXC_JOB_CONFIG_FAULT);
	pw_jm_as_put(e.dev, kkey(&k));
	job_hdr(kjob(&k, 13, NULL), 0, 1, 0, 0, 0);
	ksubmit(&e, &k, 1, kjob_va(&k, 13), false);
	evlog_wait(&l, 4, &ev);
	CHECK_EQ(ev.failed, PW_BIT(1));
	CHECK_EQ(ev.status[1], PW_JM_EXC_JOB_CONFIG_FAULT);
	pw_jm_as_put(e.dev, kkey(&k));
	mock_jm_js_stats(e.os, 1, &st);
	CHECK_EQ(st.dep_violations, 1);
	CHECK_EQ(st.config_faults, 3);

	/* the slot is usable again after a failure */
	ksubmit(&e, &k, 1, kjob_va(&k, 11), false);
	evlog_wait(&l, 5, &ev);
	CHECK_EQ(ev.done, PW_BIT(1));
	pw_jm_as_put(e.dev, kkey(&k));
out_k:
	kctx_fini(&e, &k);
	evlog_detach(&e, &l);
out:
	jenv_fini(&e);
}

static void test_jm_page_fault(void)
{
	struct jenv e;
	struct evlog l;
	struct kctx k;
	struct pw_jm_irq_event ev;
	struct mock_js_stats st;
	struct mock_as_state ast;
	uint64_t bad = KCTX_VA + 0x01000000ull;	/* not mapped */
	uint32_t fs;
	int as;

	if (!jenv_init(&e))
		goto out;
	evlog_attach(&e, &l);
	if (!kctx_init(&e, &k))
		goto out_k;

	job_wv(kjob(&k, 0, NULL), 1, 0, bad + 0x10, WV_IMM32, 1);
	as = ksubmit(&e, &k, 1, k.va, false);
	REQUIRE(as >= 0);
	/* MMU fault IRQ -> not a heap fault -> unhandled path disables the AS -> job fails */
	evlog_wait(&l, 0, &ev);
	CHECK_EQ(ev.failed, PW_BIT(1));
	CHECK((ev.status[1] & 0xf8) == 0xc0);	/* TRANSLATION_FAULT_n */
	CHECK_EQ(ev.head[1], k.va);
	pw_jm_as_put(e.dev, kkey(&k));
	CHECK(mock_log_contains(e.os, "Unhandled page fault in AS"));
	CHECK(mock_log_contains(e.os, "at VA 0x0000000011000010"));
	/* the AS fault registers as the hardware reports them */
	fs = mock_reg(e.os, AS_FAULTSTATUS(as));
	CHECK_EQ(fs & 0xf8, 0xc0);
	CHECK_EQ(fs & AS_FAULTSTATUS_ACCESS_TYPE_MASK, AS_FAULTSTATUS_ACCESS_TYPE_WRITE);
	CHECK_EQ(mock_reg(e.os, AS_FAULTADDRESS(as)), (uint32_t)(bad + 0x10));
	CHECK_EQ(mock_reg(e.os, AS_FAULTADDRESS(as) + 4), (uint32_t)((bad + 0x10) >> 32));
	mock_jm_js_stats(e.os, 1, &st);
	CHECK(st.page_faults >= 1);
	CHECK_EQ(st.last_fault_va, bad + 0x10);
	mock_get_as(e.os, (unsigned int)as, &ast);
	CHECK(!ast.enabled);

	/* the job manager could not write the descriptor back: its AS is disabled */
	CHECK_EQ(get32(kjob(&k, 0, NULL)), 0);
	/* the next pw_jm_as_get re-enables the faulty AS */
	job_wv(kjob(&k, 0, NULL), 1, 0, k.va + KDATA_OFF, WV_IMM32, 7);
	CHECK_EQ(ksubmit(&e, &k, 1, k.va, false), as);
	mock_get_as(e.os, (unsigned int)as, &ast);
	CHECK(ast.enabled);
	CHECK_EQ(e.dev->mmu.faulty_mask & PW_BIT(as), 0);
	evlog_wait(&l, 1, &ev);
	CHECK_EQ(ev.done, PW_BIT(1));
	CHECK_EQ(get32(k.cpu + KDATA_OFF), 7);
	pw_jm_as_put(e.dev, kkey(&k));

	/* heap growth: a fault stalls the job until the page is mapped and the fault cleared */
	{
		struct pw_os_mem *grow;

		CHECK_EQ(pw_os_mem_alloc(e.os, 0x1000, PW_MEM_KMAP | PW_MEM_UNCACHED, &grow),
			 PW_OK);
		job_wv(kjob(&k, 1, NULL), 1, 0, bad, WV_IMM32, 0x600d);
		pw_irq_suspend(e.dev, PW_IRQ_MMU);	/* play the fault handler ourselves */
		as = ksubmit(&e, &k, 1, kjob_va(&k, 1), false);
		for (int i = 0; i < 500; i++) {
			mock_jm_js_stats(e.os, 1, &st);
			if (st.mmu_stalled)
				break;
			usleep(1000);
		}
		CHECK(st.mmu_stalled);
		CHECK(mock_rawstat(e.os, 1) & PW_BIT(as));
		CHECK_EQ(evlog_count(&l), 2);
		CHECK_EQ(pw_vm_map_mem(k.vm, bad, grow, 0, 0x1000, 0), PW_OK);
		gpu_write(e.dev, MMU_INT_BASE + INT_CLEAR, PW_BIT(as));
		evlog_wait(&l, 2, &ev);
		CHECK_EQ(ev.done, PW_BIT(1));
		CHECK_EQ(get32(pw_os_mem_kva(grow)), 0x600d);
		mock_jm_js_stats(e.os, 1, &st);
		CHECK_EQ(st.fault_retries, 1);
		pw_jm_as_put(e.dev, kkey(&k));
		pw_irq_resume(e.dev, PW_IRQ_MMU);
		CHECK_EQ(pw_vm_unmap(k.vm, bad, 0x1000), PW_OK);
		pw_os_mem_free(e.os, grow);
	}

	/* injected failure status */
	mock_jm_inject(e.os, 1, MOCK_JM_INJ_FAIL, PW_EXC_DATA_INVALID_FAULT, 1);
	ksubmit(&e, &k, 1, k.va, false);
	evlog_wait(&l, 3, &ev);
	CHECK_EQ(ev.failed, PW_BIT(1));
	CHECK_EQ(ev.status[1], PW_EXC_DATA_INVALID_FAULT);
	CHECK(mock_log_contains(e.os, "status=DATA_INVALID_FAULT"));
	pw_jm_as_put(e.dev, kkey(&k));
out_k:
	kctx_fini(&e, &k);
	evlog_detach(&e, &l);
out:
	jenv_fini(&e);
}

static void wait_running(struct jenv *e, uint32_t js, bool want)
{
	struct mock_js_stats st;

	for (int i = 0; i < 1000; i++) {
		mock_jm_js_stats(e->os, js, &st);
		if (st.running == want)
			return;
		usleep(1000);
	}
	CHECK(!"slot state timeout");
}

/* Slot registers, NEXT handling, soft/hard stops, the _0/_1 variants. */
static void test_jm_slots_and_stops(void)
{
	struct jenv e;
	struct evlog l;
	struct kctx k;
	struct pw_jm_irq_event ev;
	struct mock_js_stats st;
	uint64_t dva;
	int as;

	if (!jenv_init(&e))
		goto out;
	evlog_attach(&e, &l);
	if (!kctx_init(&e, &k))
		goto out_k;
	dva = k.va + KDATA_OFF;
	job_wv(kjob(&k, 0, NULL), 1, 0, dva, WV_IMM32, 0xa);	/* A */
	job_wv(kjob(&k, 1, NULL), 1, 0, dva + 4, WV_IMM32, 0xb);	/* B */

	/* A stalls in the current slot, B waits in NEXT */
	mock_jm_inject(e.os, 1, MOCK_JM_INJ_STALL, 0, 1);
	as = ksubmit(&e, &k, 1, kjob_va(&k, 0), false);
	REQUIRE(as >= 0);
	wait_running(&e, 1, true);
	CHECK(pw_jm_slot_next_free(e.dev, 1));
	CHECK_EQ(pw_jm_slot_head(e.dev, 1), kjob_va(&k, 0));
	CHECK_EQ(pw_jm_slot_status(e.dev, 1), PW_EXC_ACTIVE);
	ksubmit(&e, &k, 1, kjob_va(&k, 1), true);
	CHECK(!pw_jm_slot_next_free(e.dev, 1));
	CHECK_EQ(gpu_read(e.dev, JOB_INT_JS_STATE), PW_BIT(1) | PW_BIT(17));

	/* cancel_next drops B */
	pw_jm_slot_cancel_next(e.dev, 1);
	pw_jm_as_put(e.dev, kkey(&k));
	CHECK(pw_jm_slot_next_free(e.dev, 1));
	CHECK_EQ(gpu_read(e.dev, JOB_INT_JS_STATE), PW_BIT(1));

	/* re-queue B; soft-stop A with the wrong flag variant: ignored; then the right one */
	ksubmit(&e, &k, 1, kjob_va(&k, 1), true);
	pw_jm_slot_stop(e.dev, 1, false, true);		/* A has no chain flag */
	usleep(2000);
	mock_jm_js_stats(e.os, 1, &st);
	CHECK(st.running);
	CHECK_EQ(st.ignored_stops, 1);
	pw_jm_slot_stop(e.dev, 1, false, false);
	evlog_wait(&l, 0, &ev);
	CHECK_EQ(ev.failed, PW_BIT(1));
	CHECK_EQ(ev.done, 0);
	CHECK_EQ(ev.status[1], PW_JM_EXC_STOPPED);
	CHECK_EQ(ev.tail[1], kjob_va(&k, 0));		/* resume point */
	/* the handler cancelled B before clearing the error: it never started */
	usleep(2000);
	mock_jm_js_stats(e.os, 1, &st);
	CHECK_EQ(st.runs, 1);
	CHECK_EQ(st.stopped, 1);
	CHECK(!st.running);
	CHECK(!st.next_valid);
	CHECK(pw_jm_slot_next_free(e.dev, 1));
	CHECK_EQ(st.next_cancels, 2);
	CHECK_EQ(get32(k.cpu + KDATA_OFF), 0);

	/* resume A from JS_TAIL, then B (each held an AS user reference) */
	pw_jm_slot_submit(e.dev, 1, ev.tail[1], (uint32_t)as, 0, false);
	evlog_wait(&l, 1, &ev);
	CHECK_EQ(ev.done, PW_BIT(1));
	pw_jm_as_put(e.dev, kkey(&k));
	pw_jm_slot_submit(e.dev, 1, kjob_va(&k, 1), (uint32_t)as, 0, true);
	evlog_wait(&l, 2, &ev);
	CHECK_EQ(ev.done, PW_BIT(1));
	pw_jm_as_put(e.dev, kkey(&k));
	CHECK_EQ(get32(k.cpu + KDATA_OFF), 0xa);
	CHECK_EQ(get32(k.cpu + KDATA_OFF + 4), 0xb);

	/* a hung chain ignores soft stops; the matching hard stop terminates it */
	mock_jm_inject(e.os, 1, MOCK_JM_INJ_HANG, 0, 1);
	ksubmit(&e, &k, 1, kjob_va(&k, 0), true);
	wait_running(&e, 1, true);
	pw_jm_slot_stop(e.dev, 1, false, true);
	pw_jm_slot_stop(e.dev, 1, true, false);		/* wrong variant */
	usleep(2000);
	mock_jm_js_stats(e.os, 1, &st);
	CHECK(st.running && st.hung);
	CHECK_EQ(evlog_count(&l), 3);
	pw_jm_slot_stop(e.dev, 1, true, true);
	evlog_wait(&l, 3, &ev);
	CHECK_EQ(ev.failed, PW_BIT(1));
	CHECK_EQ(ev.status[1], PW_EXC_TERMINATED);
	CHECK(mock_log_contains(e.os, "js event, js=1, status=STOPPED"));
	CHECK(mock_log_contains(e.os, "js event, js=1, status=TERMINATED"));
	pw_jm_as_put(e.dev, kkey(&k));

	/* slots outside JS0-JS2 are refused without touching registers */
	pw_jm_slot_submit(e.dev, 5, k.va, 0, 0, false);
	CHECK(mock_log_contains(e.os, "submit on JS5: slot not present"));
	CHECK_EQ(pw_jm_slot_status(e.dev, 5), 0);
out_k:
	kctx_fini(&e, &k);
	evlog_detach(&e, &l);
out:
	jenv_fini(&e);
}

/* One IRQ pass that sees DONE and then ERR of the same slot: the current job completed, the
 * one that followed from NEXT failed (panfrost_jm_handle_irq's clear+re-read loop). */
static void test_jm_irq_done_then_failed(void)
{
	struct jenv e;
	struct evlog l;
	struct kctx k;
	struct pw_jm_irq_event ev;
	struct mock_js_stats st;
	int as;

	if (!jenv_init(&e))
		goto out;
	evlog_attach(&e, &l);
	if (!kctx_init(&e, &k))
		goto out_k;
	job_hdr(kjob(&k, 0, NULL), JT_NULL, 1, 0, 0, 0);
	job_hdr(kjob(&k, 1, NULL), JT_NULL, 1, 0, 0, 0);

	pw_irq_suspend(e.dev, PW_IRQ_JOB);	/* collect both, hand them over in one pass */
	mock_jm_inject(e.os, 1, MOCK_JM_INJ_STALL, 0, 1);
	as = ksubmit(&e, &k, 1, kjob_va(&k, 0), false);
	REQUIRE(as >= 0);
	wait_running(&e, 1, true);
	ksubmit(&e, &k, 1, kjob_va(&k, 1), true);
	mock_jm_inject(e.os, 1, MOCK_JM_INJ_FAIL, PW_JM_EXC_JOB_BUS_FAULT, 1);
	mock_jm_kick(e.os, 1);
	for (int i = 0; i < 1000; i++) {
		mock_jm_js_stats(e.os, 1, &st);
		if (st.done == 1 && st.failed == 1)
			break;
		usleep(1000);
	}
	CHECK_EQ(st.done, 1);
	CHECK_EQ(st.failed, 1);
	CHECK_EQ(mock_rawstat(e.os, 0), JOB_INT_DONE(1) | JOB_INT_ERR(1));
	pw_jm_job_irq_handler(e.dev, mock_rawstat(e.os, 0));
	CHECK_EQ(evlog_count(&l), 1);
	evlog_wait(&l, 0, &ev);
	CHECK_EQ(ev.done, PW_BIT(1));
	CHECK_EQ(ev.failed, PW_BIT(1));
	CHECK_EQ(ev.status[1], PW_JM_EXC_JOB_BUS_FAULT);
	CHECK_EQ(ev.head[1], kjob_va(&k, 1));
	CHECK_EQ(ev.js_state, 0);
	CHECK_EQ(mock_rawstat(e.os, 0), 0);
	pw_jm_as_put(e.dev, kkey(&k));
	pw_jm_as_put(e.dev, kkey(&k));
	pw_irq_resume(e.dev, PW_IRQ_JOB);
	CHECK_EQ(mock_int_mask(e.os, 0), 0x00070007u);
out_k:
	kctx_fini(&e, &k);
	evlog_detach(&e, &l);
out:
	jenv_fini(&e);
}

static void test_jm_reset(void)
{
	struct jenv e;
	struct evlog l;
	struct kctx k;
	struct pw_jm_irq_event ev;
	struct mock_js_stats st;
	struct mock_as_state ast;
	uint32_t resets;
	int as;

	if (!jenv_init(&e))
		goto out;
	evlog_attach(&e, &l);
	if (!kctx_init(&e, &k))
		goto out_k;
	job_wv(kjob(&k, 0, NULL), 1, 0, k.va + KDATA_OFF, WV_IMM32, 0x5e7);

	/* a job that never completes, holding the cycle counter and its AS */
	pw_jm_cycle_counter_get(e.dev);
	mock_jm_inject(e.os, 1, MOCK_JM_INJ_HANG, 0, 1);
	as = ksubmit(&e, &k, 1, k.va, false);
	REQUIRE(as >= 0);
	wait_running(&e, 1, true);
	resets = mock_soft_resets(e.os);

	CHECK_EQ(pw_jm_hw_reset(e.dev), PW_OK);
	CHECK_EQ(mock_soft_resets(e.os), resets + 1);
	mock_jm_js_stats(e.os, 1, &st);
	CHECK(!st.running);
	CHECK_EQ(st.terminated, 1);		/* the hard stop before the reset */
	/* re-powered, IRQs re-armed */
	CHECK_EQ(gpu_read(e.dev, L2_READY), MOCK_G31_L2_PRESENT);
	CHECK_EQ(mock_reg(e.os, SHADER_READY), MOCK_G31_SHADER_PRESENT);
	CHECK_EQ(mock_reg(e.os, TILER_READY), MOCK_G31_TILER_PRESENT);
	CHECK_EQ(mock_reg(e.os, GPU_SHADER_CONFIG), SC_TLS_HASH_ENABLE);
	CHECK_EQ(mock_int_mask(e.os, 0), 0x00070007u);
	CHECK(mock_int_mask(e.os, 1) != 0);
	CHECK(mock_int_mask(e.os, 2) != 0);
	CHECK(mock_reg(e.os, GPU_STATUS) & GPU_STATUS_CYCLE_COUNT_ACTIVE);
	/* the AS binding is forgotten */
	CHECK_EQ(pw_vm_as(k.vm), -1);
	CHECK_EQ(e.dev->mmu.alloc_mask, 0);
	mock_get_as(e.os, (unsigned int)as, &ast);
	CHECK(!ast.enabled);
	/* nothing is reported for the killed job */
	usleep(20000);
	CHECK_EQ(evlog_count(&l), 0);

	/* the caller drops the killed job's references: no complaint */
	mock_log_clear(e.os);
	pw_jm_as_put(e.dev, kkey(&k));
	pw_jm_cycle_counter_put(e.dev);
	CHECK(!mock_log_contains(e.os, "without a get"));
	CHECK_EQ(mock_reg(e.os, GPU_STATUS) & GPU_STATUS_CYCLE_COUNT_ACTIVE, 0);

	/* and the GPU runs jobs again */
	as = ksubmit(&e, &k, 1, k.va, false);
	CHECK(as >= 0);
	evlog_wait(&l, 0, &ev);
	CHECK_EQ(ev.done, PW_BIT(1));
	CHECK_EQ(get32(k.cpu + KDATA_OFF), 0x5e7);
	pw_jm_as_put(e.dev, kkey(&k));

	/* a reset of an idle GPU with a bound idle VM, and the reset re-binding path */
	CHECK_EQ(pw_jm_hw_reset(e.dev), PW_OK);
	CHECK_EQ(pw_vm_as(k.vm), -1);
	CHECK(ksubmit(&e, &k, 1, k.va, false) >= 0);
	evlog_wait(&l, 1, &ev);
	CHECK_EQ(ev.done, PW_BIT(1));
	pw_jm_as_put(e.dev, kkey(&k));
out_k:
	kctx_fini(&e, &k);
	evlog_detach(&e, &l);
out:
	jenv_fini(&e);
}

/* (review) A job bottom half that runs late (on Windows the ISR only queues a work item) must
 * not process JOB_INT while pw_jm_hw_reset() has the line quiesced: the hard stop's ERR bit
 * would reach the scheduler in the middle of the reset. */
struct late_bh {
	struct jenv *e;
	volatile bool stop;
	unsigned int runs;
};

static void *late_bh_fn(void *arg)
{
	struct late_bh *b = arg;

	while (!b->stop) {
		pw_irq_bottom(b->e->dev, PW_IRQ_JOB);
		b->runs++;
	}
	return NULL;
}

static void test_jm_reset_late_bottom_half(void)
{
	struct jenv e;
	struct evlog l;
	struct kctx k;
	struct pw_jm_irq_event ev;
	struct late_bh b;
	struct mock_config cfg = { 0 };
	pthread_t thr;
	int as;

	cfg.completion_delay_us = 2000;	/* a wider window between the hard stop and the reset */
	if (!jenv_init_cfg(&e, &cfg))
		goto out;
	evlog_attach(&e, &l);
	if (!kctx_init(&e, &k))
		goto out_k;
	job_hdr(kjob(&k, 0, NULL), JT_NULL, 1, 0, 0, 0);

	for (int round = 0; round < 40; round++) {
		mock_jm_inject(e.os, 1, MOCK_JM_INJ_HANG, 0, 1);
		as = ksubmit(&e, &k, 1, k.va, false);
		REQUIRE(as >= 0);
		wait_running(&e, 1, true);

		memset(&b, 0, sizeof(b));
		b.e = &e;
		REQUIRE(pthread_create(&thr, NULL, late_bh_fn, &b) == 0);
		CHECK_EQ(pw_jm_hw_reset(e.dev), PW_OK);
		b.stop = true;
		pthread_join(thr, NULL);
		CHECK(b.runs > 0);
		/* the TERMINATED of the hung job was never handed over */
		CHECK_EQ(evlog_count(&l), 0);
		pw_jm_as_put(e.dev, kkey(&k));
	}

	/* the line works normally afterwards */
	as = ksubmit(&e, &k, 1, k.va, false);
	CHECK(as >= 0);
	evlog_wait(&l, 0, &ev);
	CHECK_EQ(ev.done, PW_BIT(1));
	CHECK_EQ(ev.failed, 0);
	pw_jm_as_put(e.dev, kkey(&k));
out_k:
	kctx_fini(&e, &k);
	evlog_detach(&e, &l);
out:
	jenv_fini(&e);
}

/* (review) pw_jm_hw_reset_collect: a chain whose DONE is still pending when the reset starts
 * is reported as done (not lost and resubmitted), the hung one as TERMINATED. */
static void test_jm_reset_collect(void)
{
	struct jenv e;
	struct evlog l;
	struct kctx k;
	struct pw_jm_irq_event ev;
	int as0, as1;

	if (!jenv_init(&e))
		goto out;
	evlog_attach(&e, &l);
	if (!kctx_init(&e, &k))
		goto out_k;
	job_hdr(kjob(&k, 0, NULL), JT_FRAGMENT, 1, 0, 0, 0);
	job_wv(kjob(&k, 1, NULL), 1, 0, k.va + KDATA_OFF, WV_IMM32, 0xd0e);

	/* The ISR has masked the line and its bottom half has not run yet. */
	pw_irq_suspend(e.dev, PW_IRQ_JOB);
	as0 = ksubmit(&e, &k, 0, kjob_va(&k, 0), false);
	REQUIRE(as0 >= 0);
	for (int i = 0; i < 1000 && !(mock_rawstat(e.os, 0) & JOB_INT_DONE(0)); i++)
		usleep(1000);
	CHECK_EQ(mock_rawstat(e.os, 0), JOB_INT_DONE(0));
	mock_jm_inject(e.os, 1, MOCK_JM_INJ_HANG, 0, 1);
	as1 = ksubmit(&e, &k, 1, kjob_va(&k, 1), false);
	REQUIRE(as1 >= 0);
	wait_running(&e, 1, true);

	CHECK_EQ(pw_jm_hw_reset_collect(e.dev, &ev), PW_OK);
	CHECK_EQ(ev.done, PW_BIT(0));
	CHECK_EQ(ev.failed, PW_BIT(1));
	CHECK_EQ(ev.status[0], 0);
	CHECK_EQ(ev.status[1], PW_EXC_TERMINATED);
	CHECK_EQ(ev.head[1], kjob_va(&k, 1));
	CHECK_EQ(ev.js_state, 0);
	CHECK_EQ(mock_rawstat(e.os, 0), 0);
	CHECK_EQ(mock_int_mask(e.os, 0), 0x00070007u);	/* resumed */
	usleep(20000);
	CHECK_EQ(evlog_count(&l), 0);			/* reported to the caller only */
	pw_jm_as_put(e.dev, kkey(&k));
	pw_jm_as_put(e.dev, kkey(&k));

	/* the caller resubmits only the killed chain; NULL ev = pw_jm_hw_reset */
	as1 = ksubmit(&e, &k, 1, kjob_va(&k, 1), false);
	CHECK(as1 >= 0);
	evlog_wait(&l, 0, &ev);
	CHECK_EQ(ev.done, PW_BIT(1));
	CHECK_EQ(get32(k.cpu + KDATA_OFF), 0xd0e);
	pw_jm_as_put(e.dev, kkey(&k));
	CHECK_EQ(pw_jm_hw_reset_collect(e.dev, NULL), PW_OK);
out_k:
	kctx_fini(&e, &k);
	evlog_detach(&e, &l);
out:
	jenv_fini(&e);
}

/* AS LRU (panfrost_mmu.c:panfrost_mmu_as_get) with more users than the 2 slots of
 * as_present = 0x3. */
static void test_jm_as_lru(void)
{
	struct mock_config cfg = { 0 };
	struct jenv e;
	struct kctx k[4];
	struct mock_as_state ast;
	bool ok = true;

	cfg.as_present = 0x3;
	memset(k, 0, sizeof(k));
	if (!jenv_init_cfg(&e, &cfg))
		goto out;
	for (int i = 0; i < 4; i++)
		ok = ok && kctx_init(&e, &k[i]);
	if (!ok)
		goto out_k;

	/* free slots first, lowest number (AS0 is a normal slot on JM) */
	CHECK_EQ(pw_jm_as_get_vm(e.dev, kkey(&k[0]), k[0].vm), 0);
	CHECK_EQ(pw_jm_as_get_vm(e.dev, kkey(&k[1]), k[1].vm), 1);
	/* reuse: same slot, another user */
	CHECK_EQ(pw_jm_as_get_vm(e.dev, kkey(&k[0]), k[0].vm), 0);
	/* all slots busy */
	CHECK_EQ(pw_jm_as_get_vm(e.dev, kkey(&k[2]), k[2].vm), PW_EBUSY);
	CHECK_EQ(pw_vm_as(k[2].vm), -1);

	/* k1 idle -> evicted for k2 (k0 still busy) */
	pw_jm_as_put(e.dev, kkey(&k[1]));
	CHECK_EQ(pw_jm_as_get_vm(e.dev, kkey(&k[2]), k[2].vm), 1);
	CHECK_EQ(pw_vm_as(k[1].vm), -1);
	CHECK_EQ(pw_vm_as(k[2].vm), 1);

	/* k0 has 2 users: one put keeps it busy */
	pw_jm_as_put(e.dev, kkey(&k[0]));
	CHECK_EQ(pw_jm_as_get_vm(e.dev, kkey(&k[3]), k[3].vm), PW_EBUSY);
	/* both idle: the least recently used (k0, last get before k2's) goes */
	pw_jm_as_put(e.dev, kkey(&k[0]));
	pw_jm_as_put(e.dev, kkey(&k[2]));
	CHECK_EQ(pw_jm_as_get_vm(e.dev, kkey(&k[3]), k[3].vm), 0);
	CHECK_EQ(pw_vm_as(k[0].vm), -1);
	CHECK_EQ(pw_vm_as(k[2].vm), 1);
	/* touching k2 makes k3 the LRU victim */
	pw_jm_as_put(e.dev, kkey(&k[3]));
	CHECK_EQ(pw_jm_as_get_vm(e.dev, kkey(&k[2]), k[2].vm), 1);
	pw_jm_as_put(e.dev, kkey(&k[2]));
	CHECK_EQ(pw_jm_as_get_vm(e.dev, kkey(&k[1]), k[1].vm), 0);
	CHECK_EQ(pw_vm_as(k[3].vm), -1);
	mock_get_as(e.os, 0, &ast);
	CHECK(ast.enabled);
	CHECK_EQ(ast.transtab, pw_vm_root_pa(k[1].vm));

	/* an unhandled fault disables the AS; the next get of its VM re-enables it */
	pw_jm_as_put(e.dev, kkey(&k[1]));
	mock_inject_mmu_fault(e.os, 0, 0x000002c1u, k[1].va);
	for (int i = 0; i < 500; i++) {
		mock_get_as(e.os, 0, &ast);
		if (!ast.enabled)
			break;
		usleep(1000);
	}
	CHECK(!ast.enabled);
	CHECK_EQ(e.dev->mmu.faulty_mask, 1);	/* set (under slots_lock) before the disable */
	CHECK_EQ(pw_jm_as_get_vm(e.dev, kkey(&k[1]), k[1].vm), 0);
	CHECK_EQ(e.dev->mmu.faulty_mask, 0);
	mock_get_as(e.os, 0, &ast);
	CHECK(ast.enabled);
	pw_jm_as_put(e.dev, kkey(&k[1]));

	/* foreign keys are never handed to the fault path */
	CHECK(pw_jm_as_vm_get(e.dev, 0) == NULL);
	CHECK(pw_jm_as_vm_get(e.dev, 1) == NULL);
	CHECK(pw_jm_as_vm_get(e.dev, 7) == NULL);
	CHECK_EQ(pw_jm_as_get_vm(e.dev, kkey(&k[1]), NULL), PW_EINVAL);

	/* a put without a get is reported, not fatal */
	pw_jm_as_put(e.dev, kkey(&k[3]));
	CHECK(mock_log_contains(e.os, "AS put without a get"));

	/* release unbinds */
	pw_jm_as_release(e.dev, kkey(&k[1]));
	CHECK_EQ(pw_vm_as(k[1].vm), -1);
	mock_get_as(e.os, 0, &ast);
	CHECK(!ast.enabled);
out_k:
	for (int i = 0; i < 4; i++)
		kctx_fini(&e, &k[i]);
out:
	jenv_fini(&e);
}

/* The same over real per-file address spaces, once jm-mem provides them (pw_jm_file_vm). */
static void test_jm_as_lru_files(void)
{
	struct mock_config cfg = { 0 };
	struct jenv e;
	struct pw_file *f[3] = { NULL, NULL, NULL };
	struct pw_jm_vm *v[3];
	struct pw_jm_vm *got;

	cfg.as_present = 0x3;
	if (!jenv_init_cfg(&e, &cfg))
		goto out;
	if (!pw_jm_file_vm(e.file)) {
		test_skip("per-file JM VMs not implemented yet (jm-mem B1)");
		goto out;
	}
	for (int i = 0; i < 3; i++) {
		if (pw_file_open(e.dev, &f[i])) {
			CHECK(!"file open");
			goto out_f;
		}
		v[i] = pw_jm_file_vm(f[i]);
	}
	CHECK_EQ(pw_jm_as_get(e.dev, v[0]), 0);
	CHECK_EQ(pw_jm_as_get(e.dev, v[1]), 1);
	CHECK_EQ(pw_jm_as_get(e.dev, v[2]), PW_EBUSY);
	/* the fault path finds the VM bound to an AS, with a reference */
	got = pw_jm_as_vm_get(e.dev, 1);
	CHECK(got == v[1]);
	pw_jm_vm_put(got);
	pw_jm_as_put(e.dev, v[0]);
	CHECK_EQ(pw_jm_as_get(e.dev, v[2]), 0);
	got = pw_jm_as_vm_get(e.dev, 0);
	CHECK(got == v[2]);
	pw_jm_vm_put(got);
	pw_jm_as_put(e.dev, v[1]);
	pw_jm_as_put(e.dev, v[2]);
out_f:
	/* closing the files releases their VMs, which unbinds the slots */
	for (int i = 0; i < 3; i++) {
		if (f[i])
			pw_file_close(f[i]);
	}
	CHECK_EQ(e.dev->mmu.alloc_mask, 0);
out:
	jenv_fini(&e);
}

static void test_jm_misc(void)
{
	struct jenv e;
	uint32_t id;

	CHECK(!strcmp(pw_jm_exception_name(PW_JM_EXC_DONE), "DONE"));
	CHECK(!strcmp(pw_jm_exception_name(PW_JM_EXC_STOPPED), "STOPPED"));
	CHECK(!strcmp(pw_jm_exception_name(PW_EXC_TERMINATED), "TERMINATED"));
	CHECK(!strcmp(pw_jm_exception_name(PW_JM_EXC_JOB_CONFIG_FAULT), "JOB_CONFIG_FAULT"));
	CHECK(!strcmp(pw_jm_exception_name(PW_JM_EXC_JOB_BUS_FAULT), "JOB_BUS_FAULT"));
	CHECK(!strcmp(pw_jm_exception_name(0x49), "Unknown exception type"));	/* CSF-only */
	CHECK(!strcmp(pw_jm_exception_name(PW_EXC_TRANSLATION_FAULT_1), "TRANSLATION_FAULT_1"));
	CHECK(!strcmp(pw_jm_exception_name(PW_JM_EXC_TRANSTAB_BUS_FAULT_2),
		      "TRANSTAB_BUS_FAULT_2"));

	if (!jenv_init(&e))
		goto out;
	/* LATEST_FLUSH_ID moves with every GPU cache flush */
	id = pw_jm_latest_flush_id(e.dev);
	CHECK_EQ(pw_gpu_flush_caches(e.dev, CACHE_CLEAN | CACHE_INV, CACHE_CLEAN, 0), PW_OK);
	CHECK(pw_jm_latest_flush_id(e.dev) != id);

	/* cycle counter refcount */
	pw_jm_cycle_counter_get(e.dev);
	pw_jm_cycle_counter_get(e.dev);
	CHECK(mock_reg(e.os, GPU_STATUS) & GPU_STATUS_CYCLE_COUNT_ACTIVE);
	pw_jm_cycle_counter_put(e.dev);
	CHECK(mock_reg(e.os, GPU_STATUS) & GPU_STATUS_CYCLE_COUNT_ACTIVE);
	pw_jm_cycle_counter_put(e.dev);
	CHECK_EQ(mock_reg(e.os, GPU_STATUS) & GPU_STATUS_CYCLE_COUNT_ACTIVE, 0);
	pw_jm_cycle_counter_put(e.dev);
	CHECK(mock_log_contains(e.os, "cycle counter put without a get"));
	CHECK_EQ(mock_reg(e.os, GPU_STATUS) & GPU_STATUS_CYCLE_COUNT_ACTIVE, 0);
out:
	jenv_fini(&e);
}

const struct test_case jm_hw_tests[] = {
	{ "jm_backend_select", test_jm_backend_select },
	{ "jm_probe_g31", test_jm_probe_g31 },
	{ "jm_csf_version_unchanged", test_jm_csf_version_unchanged },
	{ "jm_dispatch", test_jm_dispatch },
	{ "jm_unhandled_fault", test_jm_unhandled_fault },
	{ "jm_get_param", test_jm_get_param },
	{ "jm_power_up", test_jm_power_up },
	{ "jm_init_failures", test_jm_init_failures },
	{ "jm_write_value_job", test_jm_write_value_job },
	{ "jm_chain_deps", test_jm_chain_deps },
	{ "jm_page_fault", test_jm_page_fault },
	{ "jm_slots_and_stops", test_jm_slots_and_stops },
	{ "jm_irq_done_then_failed", test_jm_irq_done_then_failed },
	{ "jm_reset", test_jm_reset },
	{ "jm_reset_late_bottom_half", test_jm_reset_late_bottom_half },
	{ "jm_reset_collect", test_jm_reset_collect },
	{ "jm_as_lru", test_jm_as_lru },
	{ "jm_as_lru_files", test_jm_as_lru_files },
	{ "jm_hw_misc", test_jm_misc },
	{ NULL, NULL },
};
