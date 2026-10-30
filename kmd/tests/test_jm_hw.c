// SPDX-License-Identifier: MIT
/*
 * test_jm_hw.c - job-manager backend: backend selection, G31 probe, the per-backend ioctl
 * dispatch (jm-arch, implemented), and placeholders for the jm-hw module (GET_PARAM, power-up,
 * job slots, job IRQ, reset, AS LRU). Owner of the placeholders: jm-hw
 * (docs/status/jm-arch.md, tasks A*).
 */
#include <unistd.h>

#include "pw_jm_hw.h"
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
	mock_get_as(e.os, 0, &as);
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

/* ---- placeholders for jm-hw (tasks A1-A7 in jm-arch.md) ---- */

static void test_jm_get_param(void)
{
	struct jenv e;
	struct drm_panfrost_get_param gp;
	int ret;

	if (!jenv_init(&e))
		goto out;
	memset(&gp, 0, sizeof(gp));
	gp.param = DRM_PANFROST_PARAM_GPU_PROD_ID;
	ret = JIOC(&e, DRM_IOCTL_PANFROST_GET_PARAM, &gp);
	JM_SKIP_IF_STUB(ret, "GET_PARAM", out);
	CHECK_EQ(ret, PW_OK);
	CHECK_EQ(gp.value, MOCK_G31_GPU_ID >> 16);
	/* TODO(jm-hw): every DRM_PANFROST_PARAM_* against mock_gpu.h, pad != 0 -> EINVAL,
	 * unknown param -> EINVAL, JS_FEATURES past js_present = 0, timestamp + frequency,
	 * ALLOWED_JM_CTX_PRIORITIES = LOW|MEDIUM, SELECTED_COHERENCY = NONE. */
out:
	jenv_fini(&e);
}

static void test_jm_power_and_slots(void)
{
	struct jenv e;

	if (!jenv_init(&e))
		goto out;
	if (!pw_jm_gpu_info(e.dev)) {
		test_skip("jm-hw init not implemented yet (jm-arch stub)");
		goto out;
	}
	/* TODO(jm-hw): shader/tiler READY after init, job IRQ mask = done|err of JS0-2, quirk
	 * registers, slot submit/stop/cancel register writes (mock_jm_js_stats), job IRQ ->
	 * pw_jm_sched_job_irq event contents, pw_jm_hw_reset, AS LRU (as_present = 0x3 via
	 * mock_config.as_present: 3 VMs, eviction of idle slots only, EBUSY when all busy). */
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
	{ "jm_power_and_slots", test_jm_power_and_slots },
	{ NULL, NULL },
};
