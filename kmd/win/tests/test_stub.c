// SPDX-License-Identifier: MIT
/*
 * Host test of the -Dcore=stub build (kmd/win/pw_core_stub.c) behind the real glue
 * (pw_win_ioctl.c, pw_os_win.c) on the fake WDK: the Z1 bring-up path on the Zero3 uses exactly
 * this pair. A static Mali-G31 register file stands in for the GPU; the stub only reads it (and
 * writes the interrupt MASK/CLEAR registers). Checked against the behaviour of Linux
 * panfrost_drv.c panfrost_ioctl_get_param() / panfrost_gpu.c panfrost_gpu_init_features()
 * (pad != 0 and unknown params are EINVAL, only popcount(JS_PRESENT) JS_FEATURES are read,
 * NR_CORE_GROUPS = popcount(L2_PRESENT), GPU_PROD_ID = GPU_ID >> 16, GPU_REVISION = low 16 bits).
 * Logic only: whether the registers read like this on the board is untested on hardware.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <unistd.h>

#include "pw_win.h"
#include "fake_wdk.h"

#include "panwin/panwin_ioctl.h"
#include "drm.h"
#include "panthor_drm.h"
#include "panfrost_drm.h"

long fake_wdk_misuse_count(void);

static int checks, failures;

#define CHECK_EQ(a, b)                                                                     \
	do {                                                                               \
		long long _a = (long long)(a), _b = (long long)(b);                        \
		checks++;                                                                  \
		if (_a != _b) {                                                            \
			failures++;                                                        \
			fprintf(stderr, "FAIL %s:%d: %s == %lld, expected %s == %lld\n", __FILE__, \
				__LINE__, #a, _a, #b, _b);                                 \
		}                                                                          \
	} while (0)
#define CHECK(c) CHECK_EQ(!!(c), 1)

static uint32_t regs[0x40000 / 4];
static WDFDEVICE g_dev;
static PW_DEVICE_CTX *g_ctx;

static void regs_g31(void)
{
	memset(regs, 0, sizeof(regs));
	regs[0x000 / 4] = 0x70930000u;	/* GPU_ID */
	regs[0x004 / 4] = 0x07110206u;	/* L2_FEATURES */
	regs[0x008 / 4] = 0x2u;		/* CORE_FEATURES */
	regs[0x00c / 4] = 0x209u;	/* TILER_FEATURES */
	regs[0x010 / 4] = 0x1u;		/* MEM_FEATURES */
	regs[0x014 / 4] = 0x2830u;	/* MMU_FEATURES */
	regs[0x018 / 4] = 0xffu;	/* AS_PRESENT */
	regs[0x01c / 4] = 0x7u;		/* JS_PRESENT (CSF_ID on CSF) */
	regs[0x04c / 4] = 0x0u;		/* AFBC_FEATURES */
	regs[0x098 / 4] = 0x89abcdefu;	/* TIMESTAMP_LO */
	regs[0x09c / 4] = 0x01234567u;	/* TIMESTAMP_HI */
	regs[0x0a0 / 4] = 512;		/* THREAD_MAX_THREADS */
	regs[0x0a4 / 4] = 256;		/* THREAD_MAX_WORKGROUP_SIZE */
	regs[0x0a8 / 4] = 128;		/* THREAD_MAX_BARRIER_SIZE */
	regs[0x0ac / 4] = 0x02004000u;	/* THREAD_FEATURES */
	for (int i = 0; i < 4; i++)
		regs[(0x0b0 / 4) + i] = 0x100u + i;	/* TEXTURE_FEATURES0-3 */
	for (int i = 0; i < 16; i++)
		regs[(0x0c0 / 4) + i] = 0x200u + i;	/* JS_FEATURES: only 0-2 exist */
	regs[0x100 / 4] = 0x3u;		/* SHADER_PRESENT_LO */
	regs[0x110 / 4] = 0x1u;		/* TILER_PRESENT_LO */
	regs[0x120 / 4] = 0x1u;		/* L2_PRESENT_LO */
	regs[0x300 / 4] = 0x1u;		/* COHERENCY_FEATURES */
	regs[0x310 / 4] = 512;		/* THREAD_TLS_ALLOC */
	regs[0xe00 / 4] = 0x1u;		/* STACK_PRESENT_LO */
	regs[0x1008 / 4] = 0xffffffffu;	/* JOB_IRQ_MASK as left by someone else */
	regs[0x2008 / 4] = 0xffffffffu;	/* MMU_IRQ_MASK */
	regs[0x0028 / 4] = 0xffffffffu;	/* GPU_IRQ_MASK */
}

static int device_up(void)
{
	struct pw_device_params p = { .timer_hz = 24000000u };
	struct pw_device *pw = NULL;
	int ret;

	g_dev = fake_wdf_object_new(sizeof(PW_DEVICE_CTX));
	g_ctx = PwDeviceCtx(g_dev);
	g_ctx->device = g_dev;
	pw_os_win_init(&g_ctx->os, g_dev);
	pw_win_files_init(g_ctx);
	g_ctx->os.mmio = (volatile UCHAR *)regs;
	g_ctx->os.mmio_len = sizeof(regs);
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
	if (NT_SUCCESS(st) && req->info)
		memcpy(arg, req->sysbuf, req->info);
	fake_wdf_object_free(req);
	return st;
}

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

static int get_param(WDFFILEOBJECT fo, uint32_t param, uint64_t *value)
{
	struct drm_panfrost_get_param gp = { .param = param, .value = 0xdeadbeef };
	int ret = drm(fo, DRM_IOCTL_PANFROST_GET_PARAM, &gp);

	*value = gp.value;
	return ret;
}

static void test_g31(void)
{
	struct panwin_info info;
	WDFFILEOBJECT fo;
	uint64_t v;

	regs_g31();
	CHECK_EQ(device_up(), 0);
	CHECK_EQ(g_ctx->backend, PW_BACKEND_JM);
	/* Probe masks all three lines (the stub never unmasks anything). */
	CHECK_EQ(regs[0x1008 / 4], 0);
	CHECK_EQ(regs[0x2008 / 4], 0);
	CHECK_EQ(regs[0x0028 / 4], 0);
	fo = file_open();

	memset(&info, 0, sizeof(info));
	CHECK_EQ(ioctl_raw(fo, PANWIN_IOCTL_GET_INFO, &info, 0, sizeof(info)), STATUS_SUCCESS);
	CHECK(info.flags & PANWIN_INFO_JM);
	CHECK(!(info.flags & PANWIN_INFO_FW_BOOTED));
	CHECK(strstr(info.description, "job manager") != NULL);

	{
		char name[16] = { 0 };
		struct drm_version ver = { .name = name, .name_len = sizeof(name) - 1 };

		CHECK_EQ(drm(fo, DRM_IOCTL_VERSION, &ver), 0);
		CHECK_EQ(ver.name_len, 8);
		CHECK(!strcmp(name, "panfrost"));
	}

	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_GPU_PROD_ID, &v), 0);
	CHECK_EQ(v, 0x7093);
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_GPU_REVISION, &v), 0);
	CHECK_EQ(v, 0);
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_SHADER_PRESENT, &v), 0);
	CHECK_EQ(v, 0x3);
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_STACK_PRESENT, &v), 0);
	CHECK_EQ(v, 0x1);
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_JS_PRESENT, &v), 0);
	CHECK_EQ(v, 0x7);
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_MMU_FEATURES, &v), 0);
	CHECK_EQ(v, 0x2830);
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_THREAD_FEATURES, &v), 0);
	CHECK_EQ(v, 0x02004000u);
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_MAX_THREADS, &v), 0);
	CHECK_EQ(v, 512);
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ, &v), 0);
	CHECK_EQ(v, 256);
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ, &v), 0);
	CHECK_EQ(v, 128);
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_TEXTURE_FEATURES3, &v), 0);
	CHECK_EQ(v, 0x103);
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_JS_FEATURES2, &v), 0);
	CHECK_EQ(v, 0x202);
	/* Slots beyond popcount(JS_PRESENT) are never read: 0. */
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_JS_FEATURES3, &v), 0);
	CHECK_EQ(v, 0);
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_JS_FEATURES15, &v), 0);
	CHECK_EQ(v, 0);
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_NR_CORE_GROUPS, &v), 0);
	CHECK_EQ(v, 1);
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_COHERENCY_FEATURES, &v), 0);
	CHECK_EQ(v, 0x1);
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_THREAD_TLS_ALLOC, &v), 0);
	CHECK_EQ(v, 512);
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP, &v), 0);
	CHECK_EQ(v, 0x0123456789abcdefull);
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP_FREQUENCY, &v), 0);
	CHECK_EQ(v, 24000000);
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_ALLOWED_JM_CTX_PRIORITIES, &v), 0);
	CHECK_EQ(v, (1u << PANFROST_JM_CTX_PRIORITY_LOW) | (1u << PANFROST_JM_CTX_PRIORITY_MEDIUM));
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_SELECTED_COHERENCY, &v), 0);
	CHECK_EQ(v, DRM_PANFROST_GPU_COHERENCY_NONE);
	/* Every param panwin-info asks for is answered. */
	for (uint32_t p = 0; p <= DRM_PANFROST_PARAM_SELECTED_COHERENCY; p++)
		CHECK_EQ(get_param(fo, p, &v), 0);
	/* Unknown param and pad != 0: EINVAL (panfrost_ioctl_get_param). */
	CHECK_EQ(get_param(fo, DRM_PANFROST_PARAM_SELECTED_COHERENCY + 1, &v), -EINVAL);
	{
		struct drm_panfrost_get_param gp = { .param = 0, .pad = 1 };

		CHECK_EQ(drm(fo, DRM_IOCTL_PANFROST_GET_PARAM, &gp), -EINVAL);
	}
	/* Other panfrost ioctls are not in the stub; panthor's DEV_QUERY is not there on JM. */
	{
		struct drm_panfrost_create_bo cb = { .size = 4096 };
		struct drm_panthor_dev_query q = { .type = DRM_PANTHOR_DEV_QUERY_GPU_INFO };

		CHECK_EQ(drm(fo, DRM_IOCTL_PANFROST_CREATE_BO, &cb), -ENOTTY);
		CHECK_EQ(drm(fo, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), -ENOTTY);
	}
	/* No user-MMIO page on JM. */
	{
		struct panwin_mmap m = { .offset = DRM_PANTHOR_USER_MMIO_OFFSET_64BIT, .size = 4096,
					 .prot = PANWIN_PROT_READ };

		CHECK_EQ(st_errno(ioctl_raw(fo, PANWIN_IOCTL_MMAP, &m, sizeof(m), sizeof(m))),
			 -EINVAL);
		m.offset = DRM_PANTHOR_USER_MMIO_OFFSET_32BIT;
		CHECK_EQ(st_errno(ioctl_raw(fo, PANWIN_IOCTL_MMAP, &m, sizeof(m), sizeof(m))),
			 -EINVAL);
	}
	file_close(fo);
	device_down();
}

/* The pre-JM behaviour on a CSF GPU_ID stays: panthor name, user-MMIO page mappable. */
static void test_g610(void)
{
	WDFFILEOBJECT fo;
	struct panwin_info info;

	memset(regs, 0, sizeof(regs));
	regs[0] = 0xa8670405u;	/* arch 10.8 */
	CHECK_EQ(device_up(), 0);
	CHECK_EQ(g_ctx->backend, PW_BACKEND_CSF);
	fo = file_open();
	memset(&info, 0, sizeof(info));
	CHECK_EQ(ioctl_raw(fo, PANWIN_IOCTL_GET_INFO, &info, 0, sizeof(info)), STATUS_SUCCESS);
	CHECK(!(info.flags & PANWIN_INFO_JM));
	{
		char name[16] = { 0 };
		struct drm_version ver = { .name = name, .name_len = sizeof(name) - 1 };

		CHECK_EQ(drm(fo, DRM_IOCTL_VERSION, &ver), 0);
		CHECK(!strcmp(name, "panthor"));
	}
	{
		struct panwin_mmap m = { .offset = DRM_PANTHOR_USER_MMIO_OFFSET_64BIT, .size = 4096,
					 .prot = PANWIN_PROT_READ };

		CHECK_EQ(ioctl_raw(fo, PANWIN_IOCTL_MMAP, &m, sizeof(m), sizeof(m)), STATUS_SUCCESS);
		CHECK(m.user_va != 0);
	}
	file_close(fo);
	device_down();
}

int main(void)
{
	alarm(60);
	test_g31();
	test_g610();
	CHECK_EQ(fake_wdk_misuse_count(), 0);
	printf("test_stub: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
