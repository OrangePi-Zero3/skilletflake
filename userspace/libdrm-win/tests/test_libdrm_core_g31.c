/* SPDX-License-Identifier: MIT */
/*
 * test_libdrm_core_g31.c - libdrm-win against the REAL kmd/core on the mock Mali-G31 (Orange
 * Pi Zero3, job manager): transport "core" with PANWIN_CORE_MODEL=g31, run under wine.
 *
 * What a Mesa panfrost/panvk start-up and panwin-info do first, through the same ioctl encoding
 * as panwin.sys: device enumeration (H616 platform info derived from the driver name),
 * drmOpenWithType("panfrost"), GET_INFO (PANWIN_INFO_JM), VERSION ("panfrost" 1.5), GET_CAP,
 * every DRM_PANFROST_PARAM_* (compared with kmd/tests/mock_gpu.h once the JM GET_PARAM exists;
 * reported as not implemented while kmd/core still has the jm-arch stub), no user-MMIO page
 * (EINVAL), a binary syncobj, and a clean (leak-free) device teardown after the last close.
 * The JM job path (CREATE_BO, SUBMIT, ...) is other engineers' work in progress and is not
 * exercised here.
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
#include "panthor_drm.h"
#include "panwin/panwin_ioctl.h"
#include "mock_gpu.h"

static int failures, checks;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: CHECK failed: %s (errno %d)\n", __FILE__, __LINE__, #c, errno); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); checks++; \
	if (_a != _b) { failures++; fprintf(stderr, "%s:%d: %s == %lld (0x%llx), expected %s == %lld (0x%llx)\n", \
	__FILE__, __LINE__, #a, _a, (unsigned long long)_a, #b, _b, (unsigned long long)_b); } } while (0)

static panwin_core_get_report_fn get_report;

static void report(struct panwin_core_report *r)
{
	memset(r, 0, sizeof(*r));
	r->version = PANWIN_CORE_REPORT_VERSION;
	CHECK_EQ(get_report(r), 0);
}

static int get_param(int fd, uint32_t param, uint64_t *value)
{
	struct drm_panfrost_get_param gp = { .param = param };
	int ret = drmIoctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &gp);

	if (!ret)
		*value = gp.value;
	return ret ? errno : 0;
}

/* Expected values from the mock (kmd/tests/mock_gpu.h); UINT64_MAX = only "succeeds". */
static uint64_t expected(uint32_t p)
{
	if (p >= DRM_PANFROST_PARAM_JS_FEATURES3 && p <= DRM_PANFROST_PARAM_JS_FEATURES15)
		return 0;	/* past JS_PRESENT 0x7 */
	switch (p) {
	case DRM_PANFROST_PARAM_GPU_PROD_ID: return MOCK_G31_GPU_ID >> 16;
	case DRM_PANFROST_PARAM_GPU_REVISION: return MOCK_G31_GPU_ID & 0xffff;
	case DRM_PANFROST_PARAM_SHADER_PRESENT: return MOCK_G31_SHADER_PRESENT;
	case DRM_PANFROST_PARAM_TILER_PRESENT: return MOCK_G31_TILER_PRESENT;
	case DRM_PANFROST_PARAM_L2_PRESENT: return MOCK_G31_L2_PRESENT;
	case DRM_PANFROST_PARAM_STACK_PRESENT: return MOCK_G31_STACK_PRESENT;
	case DRM_PANFROST_PARAM_AS_PRESENT: return MOCK_G31_AS_PRESENT;
	case DRM_PANFROST_PARAM_JS_PRESENT: return MOCK_G31_JS_PRESENT;
	case DRM_PANFROST_PARAM_L2_FEATURES: return MOCK_G31_L2_FEATURES;
	case DRM_PANFROST_PARAM_CORE_FEATURES: return MOCK_G31_CORE_FEATURES;
	case DRM_PANFROST_PARAM_TILER_FEATURES: return MOCK_G31_TILER_FEATURES;
	case DRM_PANFROST_PARAM_MEM_FEATURES: return MOCK_G31_MEM_FEATURES;
	case DRM_PANFROST_PARAM_MMU_FEATURES: return MOCK_G31_MMU_FEATURES;
	case DRM_PANFROST_PARAM_THREAD_FEATURES: return MOCK_G31_THREAD_FEATURES;
	case DRM_PANFROST_PARAM_MAX_THREADS: return MOCK_G31_MAX_THREADS;
	case DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ: return MOCK_G31_MAX_WG_SIZE;
	case DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ: return MOCK_G31_MAX_BARRIER;
	case DRM_PANFROST_PARAM_COHERENCY_FEATURES: return MOCK_G31_COHERENCY_FEATURES;
	case DRM_PANFROST_PARAM_TEXTURE_FEATURES0: return MOCK_G31_TEXTURE_FEATURES0;
	case DRM_PANFROST_PARAM_JS_FEATURES0: return MOCK_G31_JS0_FEATURES;
	case DRM_PANFROST_PARAM_JS_FEATURES1: return MOCK_G31_JS1_FEATURES;
	case DRM_PANFROST_PARAM_JS_FEATURES2: return MOCK_G31_JS2_FEATURES;
	case DRM_PANFROST_PARAM_NR_CORE_GROUPS: return 1;
	case DRM_PANFROST_PARAM_THREAD_TLS_ALLOC: return MOCK_G31_THREAD_TLS_ALLOC;
	case DRM_PANFROST_PARAM_AFBC_FEATURES: return MOCK_G31_AFBC_FEATURES;
	case DRM_PANFROST_PARAM_ALLOWED_JM_CTX_PRIORITIES:
		return (1u << PANFROST_JM_CTX_PRIORITY_LOW) | (1u << PANFROST_JM_CTX_PRIORITY_MEDIUM);
	case DRM_PANFROST_PARAM_SELECTED_COHERENCY: return DRM_PANFROST_GPU_COHERENCY_NONE;
	default: return UINT64_MAX;	/* TEXTURE_FEATURES1-3, timestamps */
	}
}

static void test_get_params(int fd)
{
	uint64_t v = 0;
	int ok = 0, notty = 0, other = 0;
	int e = get_param(fd, DRM_PANFROST_PARAM_GPU_PROD_ID, &v);

	if (e == ENOTTY) {
		/* kmd/core still has the jm-arch stub (owner jm-hw): report, don't fail. */
		printf("GET_PARAM: not implemented by kmd/core yet (ENOTTY), values not checked\n");
		return;
	}
	for (uint32_t p = 0; p <= DRM_PANFROST_PARAM_SELECTED_COHERENCY; p++) {
		uint64_t want = expected(p);

		v = 0;
		e = get_param(fd, p, &v);
		if (e) {
			fprintf(stderr, "GET_PARAM %u: errno %d\n", p, e);
			e == ENOTTY ? notty++ : other++;
			continue;
		}
		ok++;
		if (want != UINT64_MAX)
			CHECK_EQ(v, want);
	}
	CHECK_EQ(notty + other, 0);
	printf("GET_PARAM: %d params answered, %d ENOTTY, %d other errors\n", ok, notty, other);
	CHECK_EQ(get_param(fd, DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP_FREQUENCY, &v), 0);
	CHECK(v != 0);
	{
		struct drm_panfrost_get_param gp = { .param = 0, .pad = 1 };

		errno = 0;
		CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &gp) == -1 && errno == EINVAL);
	}
	errno = 0;
	CHECK_EQ(get_param(fd, DRM_PANFROST_PARAM_SELECTED_COHERENCY + 1, &v), EINVAL);
}

int main(void)
{
	struct panwin_core_report r0, r;
	struct panwin_info info;
	drmDevicePtr devs[4], d = NULL;
	drmVersionPtr v;
	char path[64] = "";
	uint64_t cap = 7;
	uint32_t so = 0;
	int n, fd;

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
	CHECK_EQ(panwin_device_path(0, path, sizeof(path)), 0);
	CHECK(!strcmp(path, "core:mock-g31"));

	/* ---- enumeration: the driver name decides the platform info ---- */
	n = drmGetDevices2(0, devs, 4);
	CHECK_EQ(n, 1);
	if (n == 1) {
		CHECK(devs[0]->bustype == DRM_BUS_PLATFORM);
		CHECK(!strcmp(devs[0]->businfo.platform->fullname, "/gpu@1800000"));
		CHECK(!strcmp(devs[0]->deviceinfo.platform->compatible[0], "allwinner,sun50i-h616-mali"));
		CHECK(!strcmp(devs[0]->deviceinfo.platform->compatible[1], "arm,mali-bifrost"));
		CHECK(devs[0]->deviceinfo.platform->compatible[2] == NULL);
		printf("device: %s %s (%s)\n", devs[0]->nodes[DRM_NODE_RENDER],
		       devs[0]->businfo.platform->fullname, devs[0]->deviceinfo.platform->compatible[0]);
	}
	drmFreeDevices(devs, n > 0 ? n : 0);
	report(&r);
	CHECK_EQ(r.devices_created, r0.devices_created + 1);	/* opened once to ask */
	CHECK_EQ(r.device_live, 0);
	CHECK_EQ(r.bringup_failures, r0.bringup_failures);

	errno = 0;
	CHECK(drmOpenWithType("panthor", NULL, DRM_NODE_RENDER) == -1 && errno == ENODEV);
	fd = drmOpenWithType("panfrost", NULL, DRM_NODE_RENDER);
	CHECK(fd >= 0);
	if (fd < 0)
		return 1;
	report(&r);
	CHECK_EQ(r.device_live, 1);

	/* ---- GET_INFO / VERSION / caps ---- */
	memset(&info, 0, sizeof(info));
	CHECK_EQ(panwin_private_ioctl(fd, PANWIN_IOCTL_GET_INFO, &info, sizeof(info)), 0);
	CHECK_EQ(info.abi_version, PANWIN_ABI_VERSION);
	CHECK(info.flags & PANWIN_INFO_JM);
	CHECK(!(info.flags & PANWIN_INFO_FW_BOOTED));
	CHECK_EQ(info.drm_version_major, 1);
	CHECK_EQ(info.drm_version_minor, 5);
	info.description[sizeof(info.description) - 1] = 0;
	printf("GET_INFO: flags 0x%x, DRM %u.%u, \"%s\"\n", info.flags, info.drm_version_major,
	       info.drm_version_minor, info.description);

	v = drmGetVersion(fd);
	CHECK(v && !strcmp(v->name, "panfrost"));
	if (v) {
		CHECK_EQ(v->version_major, 1);
		CHECK_EQ(v->version_minor, 5);
		printf("DRM %s %d.%d.%d (%s)\n", v->name, v->version_major, v->version_minor,
		       v->version_patchlevel, v->desc);
	}
	drmFreeVersion(v);
	CHECK_EQ(drmGetDevice2(fd, 0, &d), 0);
	CHECK(d && !strcmp(d->businfo.platform->fullname, "/gpu@1800000"));
	drmFreeDevice(&d);
	CHECK_EQ(drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap), 0);
	CHECK_EQ(cap, 0);	/* panfrost: DRIVER_SYNCOBJ without _TIMELINE */
	CHECK_EQ(drmGetCap(fd, DRM_CAP_SYNCOBJ, &cap), 0);
	CHECK_EQ(cap, 1);

	/* ---- GET_PARAM ---- */
	test_get_params(fd);

	/* ---- no user-MMIO page on JM: EINVAL through PANWIN_IOCTL_MMAP ---- */
	errno = 0;
	CHECK(panwin_mmap(fd, DRM_PANTHOR_USER_MMIO_OFFSET_32BIT, 4096, 1) == NULL && errno == EINVAL);
	errno = 0;
	CHECK(panwin_mmap(fd, DRM_PANTHOR_USER_MMIO_OFFSET_64BIT, 4096, 1) == NULL && errno == EINVAL);

	/* ---- shared DRM core ioctls ---- */
	CHECK_EQ(drmSyncobjCreate(fd, DRM_SYNCOBJ_CREATE_SIGNALED, &so), 0);
	CHECK_EQ(drmSyncobjWait(fd, &so, 1, 0, 0, NULL), 0);
	CHECK_EQ(drmSyncobjDestroy(fd, so), 0);

	CHECK_EQ(drmClose(fd), 0);
	report(&r);
	CHECK_EQ(r.device_live, 0);
	/* One bring-up per name query (drmGetDevices2, each drmOpenWithType) plus the open. */
	CHECK_EQ(r.devices_created, r0.devices_created + 4);
	CHECK_EQ(r.devices_destroyed, r.devices_created);
	CHECK_EQ(r.leaked_allocs, 0);
	CHECK_EQ(r.leaked_mems, 0);
	CHECK_EQ(r.leaked_sync, 0);
	CHECK_EQ(r.model_errors, 0);
	CHECK_EQ(r.irq_unclaimed, 0);
	CHECK_EQ(r.leaked_maps, 0);
	CHECK_EQ(r.leaked_notifies, 0);
	CHECK_EQ(r.cs_instrs, 0);

	printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "OK", checks, failures);
	return failures ? 1 : 0;
}
