// SPDX-License-Identifier: MIT
/*
 * test_jm_mem.c - job-manager memory: the shared pieces jm-arch added for it (pw_bo_alloc,
 * release_ext, pw_bo_get_mmap_offset on a JM file, AS bind/unbind primitives), and placeholders
 * for the jm-mem module (CREATE_BO and friends, VA rules, HEAP growth, implicit-sync fences).
 * Owner of the placeholders: jm-mem (docs/status/jm-arch.md, tasks B*).
 */
#include "pw_bo.h"
#include "pw_jm_mem.h"
#include "pw_mmu.h"
#include "test_jm.h"

/* ---- shared BO hooks ---- */

static int g_release_calls;

static void count_release(struct pw_bo *bo)
{
	CHECK(bo->ext == (void *)&g_release_calls);
	g_release_calls++;
}

static void test_jm_bo_ext_hooks(void)
{
	struct jenv e;
	struct pw_bo *bo = NULL;
	uint32_t handle = 0;
	uint64_t off = 0;

	if (!jenv_init(&e))
		goto out;

	/* unbacked (HEAP-style) BO: no memory, release_ext runs once on the last put */
	CHECK_EQ(pw_bo_alloc(e.dev, 0, 0, false, false, &bo), PW_EINVAL);
	CHECK_EQ(pw_bo_alloc(e.dev, PW_SZ_2M + 1, 0, false, false, &bo), PW_OK);
	REQUIRE(bo);
	CHECK(bo->mem == NULL);
	CHECK_EQ(bo->size, PW_SZ_2M + PW_SZ_4K);
	bo->ext = &g_release_calls;
	bo->release_ext = count_release;
	g_release_calls = 0;
	pw_bo_get(bo);
	pw_bo_put(bo);
	CHECK_EQ(g_release_calls, 0);
	pw_bo_put(bo);
	CHECK_EQ(g_release_calls, 1);

	/* backed BO in the shared GEM table of a JM file: mmap offsets and GEM_CLOSE work */
	CHECK_EQ(pw_bo_alloc(e.dev, 8192, PW_MEM_ZEROED | PW_MEM_UNCACHED, false, true, &bo),
		 PW_OK);
	REQUIRE(bo && bo->mem);
	bo->ext = &g_release_calls;
	bo->release_ext = count_release;
	CHECK_EQ(pw_handle_alloc(e.os, &e.file->bos, bo, &handle), PW_OK);
	CHECK_EQ(pw_bo_get_mmap_offset(e.file, handle, &off), PW_OK);
	CHECK(off >= PW_SZ_4G);
	CHECK_EQ(pw_bo_get_mmap_offset(e.file, handle + 1, &off), PW_ENOENT);
	{
		struct pw_mmap_target t, t2;
		struct drm_gem_close gc = { .handle = handle };

		CHECK_EQ(pw_file_mmap_lookup(e.file, off, 8192, &t), PW_OK);
		CHECK_EQ(t.kind, PW_MMAP_BO);
		CHECK(!t.cached);
		CHECK_EQ(JIOC(&e, DRM_IOCTL_GEM_CLOSE, &gc), PW_OK);
		CHECK_EQ(g_release_calls, 1);		/* the mmap target still holds it */
		CHECK_EQ(pw_file_mmap_lookup(e.file, off, 8192, &t2), PW_EINVAL);
		pw_bo_put(t.bo);
		CHECK_EQ(g_release_calls, 2);
	}
	/* NO_MMAP (HEAP BOs) -> EPERM from the shared helper; MMAP_BO turns it into EINVAL */
	CHECK_EQ(pw_bo_alloc(e.dev, 4096, 0, false, false, &bo), PW_OK);
	bo->flags = DRM_PANTHOR_BO_NO_MMAP;
	CHECK_EQ(pw_handle_alloc(e.os, &e.file->bos, bo, &handle), PW_OK);
	CHECK_EQ(pw_bo_get_mmap_offset(e.file, handle, &off), PW_EPERM);
out:
	jenv_fini(&e);
}

/* ---- placeholders for jm-mem (tasks B1-B8 in jm-arch.md) ---- */

static void test_jm_create_bo(void)
{
	struct jenv e;
	struct drm_panfrost_create_bo cb;
	int ret;

	if (!jenv_init(&e))
		goto out;
	memset(&cb, 0, sizeof(cb));
	cb.size = 4096;
	cb.flags = PANFROST_BO_NOEXEC;
	ret = JIOC(&e, DRM_IOCTL_PANFROST_CREATE_BO, &cb);
	JM_SKIP_IF_STUB(ret, "CREATE_BO", out);
	CHECK_EQ(ret, PW_OK);
	CHECK(cb.handle != 0);
	CHECK(cb.offset >= PW_JM_VA_START && cb.offset < PW_JM_VA_END);
	/* TODO(jm-mem): flag validation errnos, executable alignment / 16 MiB / 4 GiB rules,
	 * 2 MiB alignment of big NOEXEC BOs, HEAP rounding, VA exhaustion -> ENOSPC, the GPU view
	 * through mock_gpu_translate (NOEXEC -> XN), GET_BO_OFFSET, MMAP_BO (HEAP -> EINVAL),
	 * SET_LABEL_BO, MADVISE retained, QUERY_BO_INFO/SYNC_BO once the version allows them,
	 * GEM_CLOSE releases the VA, file close with live mappings, no leaks. */
out:
	jenv_fini(&e);
}

static void test_jm_wait_bo(void)
{
	struct jenv e;
	struct drm_panfrost_wait_bo wb;
	int ret;

	if (!jenv_init(&e))
		goto out;
	memset(&wb, 0, sizeof(wb));
	wb.handle = 12345;
	ret = JIOC(&e, DRM_IOCTL_PANFROST_WAIT_BO, &wb);
	JM_SKIP_IF_STUB(ret, "WAIT_BO", out);
	CHECK_EQ(ret, PW_ENOENT);
	/* TODO(jm-mem): idle BO -> OK; pending fence + deadline 0/past -> EBUSY; expiring wait ->
	 * ETIMEDOUT; signalled by another thread -> OK; pad -> EINVAL. */
out:
	jenv_fini(&e);
}

static void test_jm_heap_fault(void)
{
	struct jenv e;

	if (!jenv_init(&e))
		goto out;
	if (pw_jm_mmu_fault(e.dev, 0, 0xc1, PW_JM_VA_START) == PW_ENOENT && !pw_jm_file_vm(e.file)) {
		test_skip("HEAP growth not implemented yet (jm-arch stub)");
		goto out;
	}
	/* TODO(jm-mem): HEAP BO, bind its VM (pw_jm_as_get), mock_inject_mmu_fault inside it ->
	 * 2 MiB chunk mapped (mock_gpu_translate), AS still enabled, no "Unhandled" log; second
	 * fault in the same chunk is a no-op; fault in a non-heap BO / outside any BO -> unhandled;
	 * allocation failure -> unhandled. */
out:
	jenv_fini(&e);
}

const struct test_case jm_mem_tests[] = {
	{ "jm_bo_ext_hooks", test_jm_bo_ext_hooks },
	{ "jm_create_bo", test_jm_create_bo },
	{ "jm_wait_bo", test_jm_wait_bo },
	{ "jm_heap_fault", test_jm_heap_fault },
	{ NULL, NULL },
};
