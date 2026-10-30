// SPDX-License-Identifier: MIT
/*
 * test_jm_mem.c - job-manager memory (pw_jm_mem.c, docs/status/jm-mem.md): the shared pieces
 * jm-arch added for it (pw_bo_alloc, release_ext, pw_bo_get_mmap_offset on a JM file), every
 * panfrost BO ioctl and its errnos through pw_file_ioctl() with the real panfrost_drm.h structs
 * (the 1.6 ones through their handlers, the dispatcher gates them at 1.5), panfrost's VA rules,
 * the GPU view through the mock's independent page-table walker in both page-table formats
 * (AArch64 4K and legacy Mali LPAE), the JM TLB-maintenance sequence, WAIT_BO on fences
 * signalled by another thread, GEM_CLOSE while a job still holds the BO, SUBMIT's BO lookup and
 * HEAP growth on injected MMU faults. Owner: jm-mem.
 *
 * AS binding: pw_jm_as_get() (jm-hw's LRU) when it is implemented, otherwise the VM is bound
 * directly with pw_mmu_as_bind_locked() (bind_as() below), so these tests do not depend on the
 * state of the jm-hw module.
 */
#include <pthread.h>
#include <unistd.h>

#include "pw_bo.h"
#include "pw_jm_hw.h"
#include "pw_jm_mem.h"
#include "pw_mmu.h"
#include "test_jm.h"

#define SZ_1M		(1ull << 20)
#define SZ_16M		(16ull << 20)

/* Poll (5 ms steps, 2 s) until @cond holds. */
#define WAIT_FOR(cond) do {							\
		for (int _i = 0; _i < 400 && !(cond); _i++)			\
			usleep(5000);						\
	} while (0)

/* ---- fixture helpers ---- */

/* jenv_init() with a page-table format chosen before probe and optionally zeroed memory. */
static bool menv_init(struct jenv *e, enum pw_pt_format fmt, bool zeroed)
{
	struct mock_config c = { 0 };
	struct pw_device_params p = { 0 };

	memset(e, 0, sizeof(*e));
	c.model = MOCK_MODEL_G31;
	c.honor_zeroed = zeroed;
	e->os = mock_create(&c);
	p.os = e->os;
	if (pw_device_alloc(&p, &e->dev)) {
		CHECK(!"device alloc failed");
		return false;
	}
	CHECK_EQ(pw_device_set_pt_format(e->dev, fmt), PW_OK);
	mock_attach_irq(e->os, e->dev);
	if (pw_device_probe(e->dev)) {
		CHECK(!"G31 probe failed");
		return false;
	}
	if (pw_file_open(e->dev, &e->file)) {
		CHECK(!"file open failed");
		return false;
	}
	return true;
}

static const char *fmt_name(enum pw_pt_format fmt)
{
	return fmt == PW_PT_FORMAT_MALI_LPAE ? "mali-lpae" : "aarch64";
}

static int create_bo(struct jenv *e, uint32_t size, uint32_t flags, uint32_t *handle,
		     uint64_t *va)
{
	struct drm_panfrost_create_bo cb;
	int ret;

	memset(&cb, 0, sizeof(cb));
	cb.size = size;
	cb.flags = flags;
	ret = JIOC(e, DRM_IOCTL_PANFROST_CREATE_BO, &cb);
	if (!ret) {
		CHECK(cb.handle != 0);
		CHECK(cb.offset != 0);
		CHECK_EQ(cb.size, size);	/* not written back */
		if (handle)
			*handle = cb.handle;
		if (va)
			*va = cb.offset;
	}
	return ret;
}

static int gem_close(struct jenv *e, uint32_t handle)
{
	struct drm_gem_close gc = { .handle = handle };

	return JIOC(e, DRM_IOCTL_GEM_CLOSE, &gc);
}

/* Borrowed pointer (the handle keeps the BO alive). */
static struct pw_bo *bo_of(struct jenv *e, uint32_t handle)
{
	struct pw_bo *bo = pw_file_bo_get(e->file, handle);

	if (bo)
		pw_bo_put(bo);
	return bo;
}

struct as_bind {
	int as;
	bool lru;	/* bound through pw_jm_as_get() */
};

#define DIRECT_AS	2u

static int bind_as(struct jenv *e, struct as_bind *b)
{
	struct pw_jm_vm *jvm = pw_jm_file_vm(e->file);
	struct pw_vm *vm = pw_jm_vm_mmu(jvm);
	int ret = pw_jm_as_get(e->dev, jvm);

	if (ret >= 0) {
		b->as = ret;
		b->lru = true;
		return PW_OK;
	}
	if (ret != PW_ENOTTY)
		return ret;
	b->lru = false;
	b->as = (int)DIRECT_AS;
	pw_os_mutex_lock(vm->op_lock);
	pw_os_mutex_lock(e->dev->mmu.slots_lock);
	ret = pw_mmu_as_bind_locked(vm, DIRECT_AS);	/* also re-enables a faulty AS */
	pw_os_mutex_unlock(e->dev->mmu.slots_lock);
	pw_os_mutex_unlock(vm->op_lock);
	return ret;
}

static void unbind_as(struct jenv *e, struct as_bind *b)
{
	struct pw_jm_vm *jvm = pw_jm_file_vm(e->file);
	struct pw_vm *vm = pw_jm_vm_mmu(jvm);

	if (b->lru) {
		pw_jm_as_put(e->dev, jvm);
		return;
	}
	pw_os_mutex_lock(vm->op_lock);
	pw_os_mutex_lock(e->dev->mmu.slots_lock);
	pw_mmu_as_unbind_locked(vm);
	pw_os_mutex_unlock(e->dev->mmu.slots_lock);
	pw_os_mutex_unlock(vm->op_lock);
}

/* Re-arm after an unhandled fault disabled the AS. */
static void rebind_as(struct jenv *e, struct as_bind *b)
{
	if (b->lru) {
		pw_jm_as_put(e->dev, pw_jm_file_vm(e->file));
		CHECK_EQ(bind_as(e, b), PW_OK);
	} else {
		CHECK_EQ(bind_as(e, b), PW_OK);
	}
}

static int gpu_tr(struct jenv *e, int as, uint64_t va, char access, uint64_t *pa)
{
	return mock_gpu_translate(e->os, (unsigned int)as, va, access, pa, NULL, NULL);
}

/* ---- shared BO hooks (jm-arch) ---- */

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

/* ---- B1: per-file VM ---- */

static void test_jm_file_vm(void)
{
	struct jenv e;
	struct pw_file *f2 = NULL;
	struct pw_jm_vm *jvm, *jvm2;
	long mems;

	if (!jenv_init(&e))
		goto out;
	jvm = pw_jm_file_vm(e.file);
	REQUIRE(jvm);
	CHECK(pw_jm_vm_mmu(jvm) != NULL);
	CHECK_EQ(pw_vm_as(pw_jm_vm_mmu(jvm)), -1);
	CHECK(pw_jm_vm_get(NULL) == NULL);
	CHECK(pw_jm_vm_tryget(NULL) == NULL);
	pw_jm_vm_put(NULL);
	CHECK(pw_jm_vm_mmu(NULL) == NULL);

	/* one address space per file */
	mems = mock_live_mems(e.os);
	CHECK_EQ(pw_file_open(e.dev, &f2), PW_OK);
	REQUIRE(f2);
	jvm2 = pw_jm_file_vm(f2);
	CHECK(jvm2 && jvm2 != jvm);
	CHECK(mock_live_mems(e.os) > mems);	/* its root table */
	CHECK(pw_jm_vm_tryget(jvm2) == jvm2);
	pw_file_close(f2);
	CHECK(mock_live_mems(e.os) > mems);	/* our reference keeps it */
	pw_jm_vm_put(jvm2);
	CHECK_EQ(mock_live_mems(e.os), mems);
out:
	jenv_fini(&e);
}

/* ---- B3: CREATE_BO errnos and flags ---- */

static void test_jm_create_bo(void)
{
	struct jenv e;
	struct drm_panfrost_create_bo cb;
	uint32_t h1 = 0, h2 = 0, hh = 0, hx = 0;
	uint64_t va1 = 0, va2 = 0, vah = 0;
	struct pw_bo *bo;

	if (!jenv_init(&e))
		goto out;

	memset(&cb, 0, sizeof(cb));
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_CREATE_BO, &cb), PW_EINVAL);	/* size 0 */
	cb.size = 4096;
	cb.pad = 1;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_CREATE_BO, &cb), PW_EINVAL);
	cb.pad = 0;
	cb.flags = 8;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_CREATE_BO, &cb), PW_EINVAL);	/* unknown flag */
	cb.flags = PANFROST_BO_WB_MMAP | PANFROST_BO_NOEXEC;	/* a 1.6 flag; we report 1.5 */
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_CREATE_BO, &cb), PW_EINVAL);
	cb.flags = PANFROST_BO_HEAP;				/* heaps must be NOEXEC */
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_CREATE_BO, &cb), PW_EINVAL);
	cb.flags = PANFROST_BO_HEAP | PANFROST_BO_NOEXEC | PANFROST_BO_WB_MMAP;
	CHECK_EQ(pw_jm_bo_create_user(e.file, &cb, PANFROST_BO_HEAP | PANFROST_BO_NOEXEC |
				      PANFROST_BO_WB_MMAP), PW_EINVAL);
	CHECK_EQ(cb.handle, 0);

	/* plain BOs: page-rounded, zeroed+WC memory, handle published, offset = GPU VA */
	CHECK_EQ(create_bo(&e, 5000, PANFROST_BO_NOEXEC, &h1, &va1), PW_OK);
	CHECK_EQ(va1, PW_JM_VA_START);			/* first fit in an empty VM */
	bo = bo_of(&e, h1);
	REQUIRE(bo);
	CHECK_EQ(bo->size, 8192);
	CHECK(bo->mem != NULL);
	CHECK(!pw_bo_cpu_cached(bo));
	CHECK_EQ(bo->flags, 0);
	CHECK(pw_jm_bo_of(bo) != NULL);
	CHECK_EQ(pw_jm_bo_va(bo), va1);
	CHECK_EQ(create_bo(&e, 4096, 0, &h2, &va2), PW_OK);
	CHECK_EQ(va2, va1 + 8192);

	/* heap: 2 MiB rounded, unbacked, never CPU-mappable */
	CHECK_EQ(create_bo(&e, 3 * SZ_1M, PANFROST_BO_HEAP | PANFROST_BO_NOEXEC, &hh, &vah),
		 PW_OK);
	bo = bo_of(&e, hh);
	REQUIRE(bo);
	CHECK_EQ(bo->size, 4 * SZ_1M);
	CHECK(bo->mem == NULL);
	CHECK_EQ(bo->flags, DRM_PANTHOR_BO_NO_MMAP);
	CHECK_EQ(vah % PW_SZ_2M, 0);

	/* WB_MMAP through the helper (what 1.6 will allow) */
	memset(&cb, 0, sizeof(cb));
	cb.size = 4096;
	cb.flags = PANFROST_BO_WB_MMAP;
	CHECK_EQ(pw_jm_bo_create_user(e.file, &cb, PANFROST_BO_NOEXEC | PANFROST_BO_HEAP |
				      PANFROST_BO_WB_MMAP), PW_OK);
	hx = cb.handle;
	bo = bo_of(&e, hx);
	REQUIRE(bo);
	CHECK(pw_bo_cpu_cached(bo));
	CHECK_EQ(bo->flags, DRM_PANTHOR_BO_WB_MMAP);

	/* kernel BOs are not panfrost BOs */
	CHECK(pw_jm_bo_of(NULL) == NULL);
	CHECK_EQ(pw_jm_bo_va(NULL), 0);

	/* GEM_CLOSE releases the VA: the next BO of that size takes the lowest hole again */
	CHECK_EQ(gem_close(&e, h1), PW_OK);
	CHECK_EQ(gem_close(&e, h1), PW_EINVAL);
	CHECK_EQ(create_bo(&e, 8192, PANFROST_BO_NOEXEC, &h1, &va1), PW_OK);
	CHECK_EQ(va1, PW_JM_VA_START);
out:
	jenv_fini(&e);	/* file close releases every BO: no leaks */
}

/* ---- B2: VA placement rules ---- */

static void test_jm_va_rules(void)
{
	struct jenv e;
	uint32_t h[8], hh = 0, hh2 = 0, hx = 0;
	uint64_t va = 0, vh = 0;
	const uint32_t heap = PANFROST_BO_HEAP | PANFROST_BO_NOEXEC;

	if (!jenv_init(&e))
		goto out;

	/* alignment */
	CHECK_EQ(create_bo(&e, 4096, PANFROST_BO_NOEXEC, &h[0], &va), PW_OK);
	CHECK_EQ(va, PW_JM_VA_START);
	CHECK_EQ(create_bo(&e, 12288, 0, &h[1], &va), PW_OK);	/* exec: aligned to its size */
	CHECK_EQ(va % 12288, 0);
	CHECK(va > PW_JM_VA_START);
	CHECK_EQ(create_bo(&e, 65536, 0, &h[2], &va), PW_OK);
	CHECK_EQ(va % 65536, 0);
	CHECK_EQ(create_bo(&e, 3 * SZ_1M, PANFROST_BO_NOEXEC, &h[3], &va), PW_OK);
	CHECK_EQ(va % PW_SZ_2M, 0);				/* NOEXEC >= 2 MiB */
	CHECK_EQ(create_bo(&e, 8192, PANFROST_BO_NOEXEC, &h[4], &va), PW_OK);
	CHECK_EQ(va % 4096, 0);
	CHECK(va < PW_JM_VA_START + PW_SZ_2M);			/* small NOEXEC fills a hole */
	CHECK_EQ(create_bo(&e, (uint32_t)SZ_16M, 0, &h[5], &va), PW_OK);
	CHECK_EQ(va % SZ_16M, 0);
	CHECK_EQ(va >> 24, (va + SZ_16M - 1) >> 24);		/* no 16 MiB crossing */
	CHECK_EQ(create_bo(&e, 5 * 4096, 0, &h[6], &va), PW_OK);
	CHECK_EQ(va % (5 * 4096), 0);
	CHECK_EQ(va / SZ_16M, (va + 5 * 4096 - 1) / SZ_16M);
	for (int i = 0; i < 7; i++)
		CHECK_EQ(gem_close(&e, h[i]), PW_OK);

	/* a hole that starts within 16 MiB of 4 GiB takes no executable BO */
	CHECK_EQ(create_bo(&e, (uint32_t)(PW_SZ_4G - PW_JM_VA_START - SZ_16M), heap, &hh, &vh),
		 PW_OK);
	CHECK_EQ(vh, PW_JM_VA_START);
	CHECK_EQ(create_bo(&e, 4096, 0, NULL, NULL), PW_ENOSPC);
	CHECK_EQ(create_bo(&e, 4096, PANFROST_BO_NOEXEC, &hx, &va), PW_OK);
	CHECK_EQ(va, PW_SZ_4G - SZ_16M);
	CHECK_EQ(gem_close(&e, hx), PW_OK);
	CHECK_EQ(gem_close(&e, hh), PW_OK);

	/* 18 MiB below 4 GiB: executable BOs fit, but never end on the 4 GiB boundary */
	CHECK_EQ(create_bo(&e, (uint32_t)(PW_SZ_4G - PW_JM_VA_START - 18 * SZ_1M), heap, &hh,
			   &vh), PW_OK);
	CHECK_EQ(create_bo(&e, 4096, 0, &hx, &va), PW_OK);
	CHECK_EQ(va, PW_SZ_4G - 18 * SZ_1M);
	CHECK_EQ(create_bo(&e, (uint32_t)SZ_16M, 0, NULL, NULL), PW_ENOSPC);
	CHECK_EQ(create_bo(&e, 8 * SZ_1M, 0, &h[0], &va), PW_OK);
	CHECK_EQ(va, PW_SZ_4G - 16 * SZ_1M);
	CHECK(va + 8 * SZ_1M <= PW_SZ_4G - 4096);
	/* the hole after it starts 8 MiB below 4 GiB: too close for executable BOs, while the
	 * 2 MiB - 4 KiB hole before it still takes small ones; NOEXEC BOs may use the last one */
	CHECK_EQ(create_bo(&e, (uint32_t)PW_SZ_2M, 0, NULL, NULL), PW_ENOSPC);
	CHECK_EQ(create_bo(&e, 4096, 0, &h[1], &va), PW_OK);
	CHECK_EQ(va, PW_SZ_4G - 18 * SZ_1M + 4096);
	CHECK_EQ(create_bo(&e, (uint32_t)PW_SZ_2M, PANFROST_BO_NOEXEC, &h[2], &va), PW_OK);
	CHECK_EQ(va, PW_SZ_4G - 8 * SZ_1M);
	for (int i = 0; i < 3; i++)
		CHECK_EQ(gem_close(&e, h[i]), PW_OK);
	CHECK_EQ(gem_close(&e, hx), PW_OK);
	CHECK_EQ(gem_close(&e, hh), PW_OK);

	/* the last 2 MiB: NOEXEC may end exactly at 4 GiB, executable BOs may not start there */
	CHECK_EQ(create_bo(&e, (uint32_t)(PW_SZ_4G - PW_JM_VA_START - PW_SZ_2M), heap, &hh, NULL),
		 PW_OK);
	CHECK_EQ(create_bo(&e, 4096, 0, NULL, NULL), PW_ENOSPC);
	CHECK_EQ(create_bo(&e, (uint32_t)PW_SZ_2M, PANFROST_BO_NOEXEC, &hx, &va), PW_OK);
	CHECK_EQ(va, PW_SZ_4G - PW_SZ_2M);
	CHECK_EQ(create_bo(&e, 4096, PANFROST_BO_NOEXEC, NULL, NULL), PW_ENOSPC);	/* full */
	CHECK_EQ(gem_close(&e, hx), PW_OK);
	CHECK_EQ(gem_close(&e, hh), PW_OK);

	/* exhaustion, and the VA comes back with GEM_CLOSE */
	CHECK_EQ(create_bo(&e, 0x80000000u, heap, &hh, &vh), PW_OK);
	CHECK_EQ(create_bo(&e, 0x80000000u, heap, &hh2, NULL), PW_ENOSPC);
	CHECK_EQ(create_bo(&e, 0xfffff000u, heap, NULL, NULL), PW_ENOSPC);	/* rounds to 4 GiB */
	CHECK_EQ(gem_close(&e, hh), PW_OK);
	CHECK_EQ(create_bo(&e, 0x80000000u, heap, &hh2, &va), PW_OK);
	CHECK_EQ(va, vh);
	/* a backed BO the OS cannot allocate fails cleanly */
	CHECK_EQ(create_bo(&e, 0x40000000u, PANFROST_BO_NOEXEC, NULL, NULL), PW_ENOMEM);
	mock_fail_mem_allocs(e.os, 1);
	CHECK_EQ(create_bo(&e, 4096, PANFROST_BO_NOEXEC, NULL, NULL), PW_ENOMEM);
out:
	jenv_fini(&e);
}

/* ---- B3/B5: the GPU view in both page-table formats, and the TLB sequence ---- */

static void gpu_view_body(enum pw_pt_format fmt)
{
	struct jenv e;
	struct as_bind b = { -1, false };
	struct mock_as_state a0, a1;
	struct pw_jm_vm *jvm;
	struct pw_vm *vm;
	struct pw_bo *bo;
	uint32_t hn = 0, hx = 0, hh = 0;
	uint64_t van = 0, vax = 0, vah = 0, pa = 0, pte = 0, cpu_pa = 0, cpu_pte = 0;
	uint32_t flushes;
	uint8_t buf[64], zero[64];
	int lvl = -1;
	const bool lpae = fmt == PW_PT_FORMAT_MALI_LPAE;

	printf("    format %s\n", fmt_name(fmt));
	if (!menv_init(&e, fmt, true))
		goto out;
	jvm = pw_jm_file_vm(e.file);
	vm = pw_jm_vm_mmu(jvm);
	REQUIRE(vm);
	CHECK_EQ(vm->pt_format, fmt);
	CHECK_EQ(vm->start_level, lpae ? 0u : 4 - PW_DIV_ROUND_UP(vm->dev->mmu.va_bits - 12, 9));

	/* unbound VM: page tables are written, no AS command is sent */
	CHECK_EQ(create_bo(&e, 8192, PANFROST_BO_NOEXEC, &hn, &van), PW_OK);
	mock_get_as(e.os, DIRECT_AS, &a0);
	CHECK_EQ(a0.locks + a0.flush_pts + a0.flush_mems + a0.unlocks, 0);

	CHECK_EQ(bind_as(&e, &b), PW_OK);
	REQUIRE(b.as >= 0);
	mock_get_as(e.os, (unsigned int)b.as, &a0);
	CHECK(a0.enabled);
	CHECK_EQ(a0.transtab, pw_vm_transtab(vm));
	CHECK_EQ(a0.transcfg, pw_vm_transcfg(vm));
	CHECK_EQ(a0.memattr, pw_vm_memattr(vm));
	if (lpae) {
		CHECK_EQ(a0.transcfg, AS_TRANSCFG_ADRMODE_LEGACY);
		CHECK_EQ(a0.transtab, pw_vm_root_pa(vm) | 3 | 4);	/* TABLE | READ_INNER */
		CHECK_EQ(a0.memattr, 0x00888d88ull);			/* what Linux programs */
	} else {
		CHECK_EQ(a0.transcfg & AS_TRANSCFG_ADRMODE_MASK, AS_TRANSCFG_ADRMODE_AARCH64_4K);
		CHECK_EQ(a0.transtab, pw_vm_root_pa(vm));
		CHECK_EQ(a0.memattr, e.dev->mmu.memattr);
	}

	/* the earlier BO is visible once bound: PA of each page, zeroed, RW, not executable */
	bo = bo_of(&e, hn);
	REQUIRE(bo);
	for (unsigned int i = 0; i < 2; i++) {
		uint64_t va = van + i * 4096 + 0x10;

		CHECK_EQ(mock_gpu_translate(e.os, (unsigned int)b.as, va, 'w', &pa, &lvl, &pte), 0);
		CHECK_EQ(pa, pw_os_mem_page_pa(bo->mem, i) + 0x10);
		CHECK_EQ(lvl, 3);
		CHECK_EQ(pw_vm_translate(vm, va, &cpu_pa, &cpu_pte, NULL), PW_OK);
		CHECK_EQ(cpu_pa, pa);
		CHECK_EQ(cpu_pte, pte);
	}
	CHECK_EQ(gpu_tr(&e, b.as, van, 'x', NULL), 0xc8 + 3);	/* NOEXEC: permission fault */
	CHECK_EQ(gpu_tr(&e, b.as, van + 8192, 'r', NULL), 0xc0 + 3);	/* past the BO */
	if (lpae) {
		CHECK_EQ(pte & 3, PW_LPAE_PTE_TYPE_LEAF);
		CHECK(pte & PW_LPAE_PTE_RD);
		CHECK(pte & PW_LPAE_PTE_WR);
		CHECK(!(pte & PW_PTE_AF));
		CHECK_EQ(pte & ~PW_PTE_OA_MASK,
			 pw_mmu_lpae_pte_attrs(PW_VM_MAP_NOEXEC, false) | PW_LPAE_PTE_TYPE_LEAF);
	} else {
		CHECK_EQ(pte & 3, PW_PTE_TYPE_PAGE);
		CHECK(pte & PW_PTE_AF);
		CHECK_EQ(pte & ~PW_PTE_OA_MASK,
			 pw_mmu_pte_attrs(PW_VM_MAP_NOEXEC, false) | PW_PTE_TYPE_PAGE);
	}
	CHECK(pte & PW_PTE_XN);
	CHECK_EQ(pte & PW_PTE_ATTRINDX_MASK, PW_PTE_ATTRINDX(PW_MAIR_IDX_WB));	/* GPU cached */
	CHECK_EQ(pte & PW_PTE_SH_MASK, lpae ? PW_PTE_SH_OS : PW_PTE_SH_IS);
	memset(zero, 0, sizeof(zero));
	CHECK_EQ(mock_gpu_read(e.os, (unsigned int)b.as, van, buf, sizeof(buf)), 0);
	CHECK_EQ(memcmp(buf, zero, sizeof(buf)), 0);		/* PW_MEM_ZEROED */

	/* map on a bound VM: LOCK, then FLUSH_PT ends the lock; no UNLOCK, no GPU_CMD flush */
	flushes = mock_flush_count(e.os);
	mock_get_as(e.os, (unsigned int)b.as, &a0);
	CHECK_EQ(create_bo(&e, 4096, 0, &hx, &vax), PW_OK);
	mock_get_as(e.os, (unsigned int)b.as, &a1);
	CHECK_EQ(a1.locks, a0.locks + 1);
	CHECK_EQ(a1.flush_pts, a0.flush_pts + 1);
	CHECK_EQ(a1.flush_mems, a0.flush_mems);
	CHECK_EQ(a1.unlocks, a0.unlocks);
	CHECK(!a1.locked);
	CHECK_EQ(mock_flush_count(e.os), flushes);
	CHECK_EQ(gpu_tr(&e, b.as, vax, 'x', NULL), 0);			/* executable */
	CHECK_EQ(gpu_tr(&e, b.as, vax, 'w', NULL), 0);
	/* heap: VA reserved, nothing mapped, no AS command */
	CHECK_EQ(create_bo(&e, (uint32_t)PW_SZ_2M, PANFROST_BO_HEAP | PANFROST_BO_NOEXEC, &hh,
			   &vah), PW_OK);
	mock_get_as(e.os, (unsigned int)b.as, &a0);
	CHECK_EQ(a0.locks, a1.locks);
	CHECK(gpu_tr(&e, b.as, vah, 'r', NULL) >= 0xc0);

	/* unmap (last reference): LOCK, FLUSH_MEM; the range faults afterwards */
	CHECK_EQ(gem_close(&e, hx), PW_OK);
	mock_get_as(e.os, (unsigned int)b.as, &a1);
	CHECK_EQ(a1.locks, a0.locks + 1);
	CHECK_EQ(a1.flush_mems, a0.flush_mems + 1);
	CHECK_EQ(a1.flush_pts, a0.flush_pts);
	CHECK_EQ(a1.unlocks, a0.unlocks);
	CHECK(!a1.locked);
	CHECK_EQ(mock_flush_count(e.os), flushes);
	CHECK(gpu_tr(&e, b.as, vax, 'r', NULL) >= 0xc0);
	CHECK_EQ(pw_vm_translate(vm, vax, NULL, NULL, NULL), PW_ENOENT);

	/* CPU writes through the (write-combined) user mapping reach the GPU directly */
	{
		struct drm_panfrost_mmap_bo mb = { .handle = hn };
		struct pw_mmap_target t;
		uint8_t *cpu;

		CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_MMAP_BO, &mb), PW_OK);
		CHECK_EQ(pw_file_mmap_lookup(e.file, mb.offset, 8192, &t), PW_OK);
		CHECK(!t.cached);
		cpu = mock_mem_user_view(e.os, pw_bo_os_mem(t.bo));
		REQUIRE(cpu);
		memset(cpu + 4096, 0x5a, 64);
		CHECK_EQ(mock_gpu_read(e.os, (unsigned int)b.as, van + 4096, buf, 64), 0);
		CHECK_EQ(buf[0], 0x5a);
		CHECK_EQ(buf[63], 0x5a);
		pw_bo_put(t.bo);
	}
	unbind_as(&e, &b);
out:
	jenv_fini(&e);
}

static void test_jm_gpu_view(void)
{
	gpu_view_body(PW_PT_FORMAT_AARCH64_4K);
	gpu_view_body(PW_PT_FORMAT_MALI_LPAE);
}

/* ---- the Mali LPAE tables at the pw_vm level: 2 MiB blocks, splits, attributes ---- */

static void test_jm_lpae_tables(void)
{
	struct jenv e;
	struct pw_vm *vm = NULL;
	struct pw_os_mem *big = NULL, *mem = NULL;
	uint64_t pa = 0, pte = 0, pa0;
	int lvl = -1;
	const unsigned int as = 3;

	if (!menv_init(&e, PW_PT_FORMAT_MALI_LPAE, false))
		goto out;
	REQUIRE(pw_vm_create(e.dev, false, &vm) == PW_OK);
	CHECK_EQ(vm->pt_format, PW_PT_FORMAT_MALI_LPAE);
	pw_os_mutex_lock(vm->op_lock);
	pw_os_mutex_lock(e.dev->mmu.slots_lock);
	CHECK_EQ(pw_mmu_as_bind_locked(vm, as), PW_OK);
	pw_os_mutex_unlock(e.dev->mmu.slots_lock);
	pw_os_mutex_unlock(vm->op_lock);

	/* 2 MiB-aligned contiguous memory -> two level-2 blocks, type 0b01 */
	REQUIRE(pw_os_mem_alloc(e.os, 4 * SZ_1M, PW_MEM_CONTIGUOUS, &big) == PW_OK);
	pa0 = pw_os_mem_page_pa(big, 0);
	REQUIRE((pa0 & (PW_SZ_2M - 1)) == 0);
	CHECK_EQ(pw_vm_map_mem(vm, 0x40000000, big, 0, 4 * SZ_1M, PW_VM_MAP_READONLY), PW_OK);
	CHECK_EQ(mock_gpu_translate(e.os, as, 0x40000000 + 0x123456, 'r', &pa, &lvl, &pte), 0);
	CHECK_EQ(pa, pa0 + 0x123456);
	CHECK_EQ(lvl, 2);
	CHECK_EQ(pte & 3, 1);
	CHECK(!(pte & PW_LPAE_PTE_WR));
	CHECK_EQ(mock_gpu_translate(e.os, as, 0x40000000, 'w', NULL, NULL, NULL), 0xc8 + 2);
	CHECK_EQ(pw_vm_translate(vm, 0x40200000 + 0x1000, &pa, &pte, &lvl), PW_OK);
	CHECK_EQ(pa, pa0 + PW_SZ_2M + 0x1000);
	CHECK_EQ(lvl, 2);

	/* unmapping one page in a block splits it into level-3 pages, type 0b01 too */
	CHECK_EQ(pw_vm_unmap(vm, 0x40001000, 4096), PW_OK);
	CHECK(mock_gpu_translate(e.os, as, 0x40001000, 'r', NULL, NULL, NULL) >= 0xc0);
	CHECK_EQ(pw_vm_translate(vm, 0x40001000, NULL, NULL, NULL), PW_ENOENT);
	CHECK_EQ(mock_gpu_translate(e.os, as, 0x40002000, 'r', &pa, &lvl, &pte), 0);
	CHECK_EQ(pa, pa0 + 0x2000);
	CHECK_EQ(lvl, 3);
	CHECK_EQ(pte & 3, 1);
	CHECK_EQ(pw_vm_translate(vm, 0x40002000, &pa, NULL, &lvl), PW_OK);
	CHECK_EQ(lvl, 3);
	CHECK_EQ(mock_gpu_translate(e.os, as, 0x40200000, 'r', NULL, &lvl, NULL), 0);
	CHECK_EQ(lvl, 2);					/* the other block is intact */

	/* uncached / read-write / exec attributes */
	REQUIRE(pw_os_mem_alloc(e.os, 4096, 0, &mem) == PW_OK);
	CHECK_EQ(pw_vm_map_mem(vm, 0x50000000, mem, 0, 4096, PW_VM_MAP_UNCACHED), PW_OK);
	CHECK_EQ(mock_gpu_translate(e.os, as, 0x50000000, 'w', NULL, NULL, &pte), 0);
	CHECK_EQ(mock_gpu_translate(e.os, as, 0x50000000, 'x', NULL, NULL, NULL), 0);
	CHECK_EQ(pte & PW_PTE_ATTRINDX_MASK, PW_PTE_ATTRINDX(PW_MAIR_IDX_NC));
	CHECK_EQ(pte & PW_PTE_SH_MASK, PW_PTE_SH_OS);
	CHECK_EQ(pw_mmu_lpae_pte_attrs(PW_VM_MAP_READONLY | PW_VM_MAP_NOEXEC, false),
		 PW_LPAE_PTE_RD | PW_PTE_XN | PW_PTE_ATTRINDX(PW_MAIR_IDX_WB) | PW_PTE_SH_OS);

	CHECK_EQ(pw_vm_unmap(vm, 0x40000000, 4 * SZ_1M), PW_OK);
	CHECK_EQ(pw_vm_unmap(vm, 0x50000000, 4096), PW_OK);
	pw_vm_destroy(vm);	/* unbinds */
	pw_os_mem_free(e.os, big);
	pw_os_mem_free(e.os, mem);
out:
	jenv_fini(&e);
}

/* ---- format selection: per VM at creation, JM only ---- */

static void test_jm_pt_format_select(void)
{
	struct jenv e;
	struct pw_vm *v1 = NULL, *v2 = NULL;
	struct pw_device_params p = { 0 };
	struct pw_device *csf = NULL;
	struct pw_os *os;
	struct mock_as_state st;

	CHECK_EQ(pw_device_set_pt_format(NULL, PW_PT_FORMAT_AARCH64_4K), PW_EINVAL);
	if (!jenv_init(&e))
		goto out;
	CHECK_EQ(pw_device_set_pt_format(e.dev, (enum pw_pt_format)7), PW_EINVAL);
	CHECK_EQ(pw_vm_create(e.dev, false, &v1), PW_OK);
	CHECK_EQ(pw_device_set_pt_format(e.dev, PW_PT_FORMAT_MALI_LPAE), PW_OK);
	CHECK_EQ(pw_vm_create(e.dev, false, &v2), PW_OK);
	REQUIRE(v1 && v2);
	CHECK_EQ(v1->pt_format, PW_PT_FORMAT_AARCH64_4K);	/* created before: unchanged */
	CHECK_EQ(v2->pt_format, PW_PT_FORMAT_MALI_LPAE);
	CHECK_EQ(pw_jm_vm_mmu(pw_jm_file_vm(e.file))->pt_format, PW_PT_FORMAT_AARCH64_4K);
	/* both formats live side by side on two AS */
	{
		struct pw_os_mem *mem = NULL;

		REQUIRE(pw_os_mem_alloc(e.os, 4096, 0, &mem) == PW_OK);
		CHECK_EQ(pw_vm_map_mem(v1, 0x2000000, mem, 0, 4096, 0), PW_OK);
		CHECK_EQ(pw_vm_map_mem(v2, 0x2000000, mem, 0, 4096, 0), PW_OK);
		pw_os_mutex_lock(e.dev->mmu.slots_lock);
		CHECK_EQ(pw_mmu_as_bind_locked(v1, 4), PW_OK);
		CHECK_EQ(pw_mmu_as_bind_locked(v2, 5), PW_OK);
		pw_os_mutex_unlock(e.dev->mmu.slots_lock);
		mock_get_as(e.os, 5, &st);
		CHECK_EQ(st.transcfg, AS_TRANSCFG_ADRMODE_LEGACY);
		CHECK_EQ(mock_gpu_translate(e.os, 4, 0x2000000, 'w', NULL, NULL, NULL), 0);
		CHECK_EQ(mock_gpu_translate(e.os, 5, 0x2000000, 'w', NULL, NULL, NULL), 0);
		pw_vm_destroy(v1);
		pw_vm_destroy(v2);
		pw_os_mem_free(e.os, mem);
	}
	CHECK_EQ(pw_device_set_pt_format(e.dev, PW_PT_FORMAT_AARCH64_4K), PW_OK);
out:
	jenv_fini(&e);

	/* CSF: a request made before probe is ignored, after probe it is refused */
	os = mock_create(NULL);
	p.os = os;
	p.boot_firmware = false;
	REQUIRE(pw_device_alloc(&p, &csf) == PW_OK);
	CHECK_EQ(pw_device_set_pt_format(csf, PW_PT_FORMAT_MALI_LPAE), PW_OK);
	REQUIRE(pw_device_probe(csf) == PW_OK);
	CHECK_EQ(pw_device_backend(csf), PW_BACKEND_CSF);
	CHECK_EQ(pw_device_set_pt_format(csf, PW_PT_FORMAT_MALI_LPAE), PW_EOPNOTSUPP);
	CHECK_EQ(pw_device_set_pt_format(csf, PW_PT_FORMAT_AARCH64_4K), PW_OK);
	if (pw_mmu_init(csf) == PW_OK) {
		CHECK_EQ(pw_device_set_pt_format(csf, PW_PT_FORMAT_AARCH64_4K), PW_OK);
		csf->mmu.pt_format = PW_PT_FORMAT_MALI_LPAE;	/* even if forced */
		CHECK_EQ(pw_vm_create(csf, false, &v1), PW_OK);
		CHECK_EQ(v1->pt_format, PW_PT_FORMAT_AARCH64_4K);
		CHECK_EQ(pw_vm_transtab(v1), pw_vm_root_pa(v1));
		CHECK_EQ(pw_vm_memattr(v1), csf->mmu.memattr);
		CHECK_EQ(pw_vm_transcfg(v1) & AS_TRANSCFG_ADRMODE_MASK,
			 AS_TRANSCFG_ADRMODE_AARCH64_4K);
		pw_vm_destroy(v1);
	} else {
		CHECK(!"CSF mmu init failed");
	}
	pw_device_destroy(csf);
	check_no_leaks(os);
	CHECK_EQ(mock_model_errors(os), 0);
	mock_destroy(os);
}

/* ---- B4: MMAP_BO, GET_BO_OFFSET, SET_LABEL_BO, MADVISE ---- */

static void test_jm_bo_ioctls(void)
{
	struct jenv e;
	struct drm_panfrost_mmap_bo mb;
	struct drm_panfrost_get_bo_offset go;
	struct drm_panfrost_set_label_bo sl;
	struct drm_panfrost_madvise ma;
	struct drm_panfrost_create_bo cb;
	struct pw_mmap_target t, t2;
	struct pw_bo *kbo = NULL;
	uint32_t h = 0, hh = 0, hw = 0, hk = 0;
	uint64_t va = 0, off;
	char longlabel[5000];

	if (!jenv_init(&e))
		goto out;
	CHECK_EQ(create_bo(&e, 8192, PANFROST_BO_NOEXEC, &h, &va), PW_OK);
	CHECK_EQ(create_bo(&e, (uint32_t)PW_SZ_2M, PANFROST_BO_HEAP | PANFROST_BO_NOEXEC, &hh,
			   NULL), PW_OK);
	CHECK_EQ(pw_bo_alloc(e.dev, 4096, PW_MEM_UNCACHED, false, true, &kbo), PW_OK);
	REQUIRE(kbo);
	CHECK_EQ(pw_handle_alloc(e.os, &e.file->bos, kbo, &hk), PW_OK);	/* not panfrost */

	/* MMAP_BO */
	memset(&mb, 0, sizeof(mb));
	mb.handle = h;
	mb.flags = 1;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_MMAP_BO, &mb), PW_EINVAL);
	mb.flags = 0;
	mb.handle = 999;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_MMAP_BO, &mb), PW_ENOENT);
	mb.handle = hh;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_MMAP_BO, &mb), PW_EINVAL);	/* heap */
	mb.handle = h;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_MMAP_BO, &mb), PW_OK);
	off = mb.offset;
	CHECK(off >= PW_SZ_4G && !(off & 4095));
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_MMAP_BO, &mb), PW_OK);
	CHECK_EQ(mb.offset, off);					/* stable */
	CHECK_EQ(pw_file_mmap_lookup(e.file, off, 8192, &t), PW_OK);
	CHECK_EQ(t.kind, PW_MMAP_BO);
	CHECK(t.bo == bo_of(&e, h));
	CHECK(!t.cached);						/* write-combined */
	CHECK(!t.read_only);
	pw_bo_put(t.bo);
	CHECK_EQ(pw_file_mmap_lookup(e.file, off + 4096, 4096, &t), PW_OK);
	CHECK_EQ(t.offset_in_obj, 4096);
	pw_bo_put(t.bo);
	CHECK_EQ(pw_file_mmap_lookup(e.file, off, 3 * 4096, &t), PW_EINVAL);	/* too big */
	/* WB_MMAP BOs are mapped cached */
	memset(&cb, 0, sizeof(cb));
	cb.size = 4096;
	cb.flags = PANFROST_BO_WB_MMAP | PANFROST_BO_NOEXEC;
	CHECK_EQ(pw_jm_bo_create_user(e.file, &cb, ~0u), PW_OK);
	hw = cb.handle;
	mb.handle = hw;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_MMAP_BO, &mb), PW_OK);
	CHECK_EQ(pw_file_mmap_lookup(e.file, mb.offset, 4096, &t), PW_OK);
	CHECK(t.cached);
	pw_bo_put(t.bo);

	/* GET_BO_OFFSET */
	memset(&go, 0, sizeof(go));
	go.handle = 999;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_GET_BO_OFFSET, &go), PW_ENOENT);
	go.handle = h;
	go.pad = 7;				/* not checked, as in Linux */
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_GET_BO_OFFSET, &go), PW_OK);
	CHECK_EQ(go.offset, va);
	go.handle = hk;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_GET_BO_OFFSET, &go), PW_EINVAL);	/* no mapping */

	/* SET_LABEL_BO */
	memset(&sl, 0, sizeof(sl));
	sl.handle = h;
	sl.pad = 1;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SET_LABEL_BO, &sl), PW_EINVAL);
	sl.pad = 0;
	sl.handle = 999;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SET_LABEL_BO, &sl), PW_ENOENT);
	sl.handle = h;
	sl.label = (uint64_t)(uintptr_t)"vertex buffer";
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SET_LABEL_BO, &sl), PW_OK);
	CHECK_EQ(strcmp(bo_of(&e, h)->label, "vertex buffer"), 0);
	memset(longlabel, 'x', sizeof(longlabel));
	sl.label = (uint64_t)(uintptr_t)longlabel;			/* no NUL in 4096 bytes */
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SET_LABEL_BO, &sl), PW_E2BIG);
	CHECK_EQ(strcmp(bo_of(&e, h)->label, "vertex buffer"), 0);	/* unchanged */
	longlabel[100] = '\0';
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SET_LABEL_BO, &sl), PW_OK);
	CHECK_EQ(strlen(bo_of(&e, h)->label), sizeof(bo_of(&e, h)->label) - 1);	/* truncated */
	sl.label = 0;							/* NULL clears */
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SET_LABEL_BO, &sl), PW_OK);
	CHECK_EQ(bo_of(&e, h)->label[0], '\0');

	/* MADVISE: nothing is ever purged */
	memset(&ma, 0, sizeof(ma));
	ma.handle = 999;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_MADVISE, &ma), PW_ENOENT);
	ma.handle = h;
	ma.madv = PANFROST_MADV_DONTNEED;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_MADVISE, &ma), PW_OK);
	CHECK_EQ(ma.retained, 1);
	ma.madv = PANFROST_MADV_WILLNEED;
	ma.retained = 0;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_MADVISE, &ma), PW_OK);
	CHECK_EQ(ma.retained, 1);
	ma.handle = hh;
	ma.madv = PANFROST_MADV_DONTNEED;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_MADVISE, &ma), PW_OK);
	CHECK_EQ(ma.retained, 1);
	ma.handle = hk;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_MADVISE, &ma), PW_EINVAL);

	/* GEM_CLOSE revokes the mmap offset; an existing mapping keeps BO and VA alive */
	CHECK_EQ(pw_file_mmap_lookup(e.file, off, 8192, &t), PW_OK);
	CHECK_EQ(gem_close(&e, h), PW_OK);
	CHECK_EQ(pw_file_mmap_lookup(e.file, off, 8192, &t2), PW_EINVAL);
	CHECK_EQ(pw_vm_translate(pw_jm_vm_mmu(pw_jm_file_vm(e.file)), va, NULL, NULL, NULL),
		 PW_OK);
	CHECK_EQ(create_bo(&e, 8192, PANFROST_BO_NOEXEC, NULL, &off), PW_OK);
	CHECK(off != va);						/* VA still taken */
	pw_bo_put(t.bo);						/* "munmap" */
	CHECK_EQ(pw_vm_translate(pw_jm_vm_mmu(pw_jm_file_vm(e.file)), va, NULL, NULL, NULL),
		 PW_ENOENT);
out:
	jenv_fini(&e);
}

/* ---- B4: the 1.6 ioctls (SYNC_BO, QUERY_BO_INFO) ---- */

static void test_jm_sync_query(void)
{
	struct jenv e;
	struct as_bind b = { -1, false };
	struct drm_panfrost_sync_bo sb;
	struct drm_panfrost_bo_sync_op ops[3];
	struct drm_panfrost_query_bo_info qi;
	struct drm_panfrost_create_bo cb;
	uint32_t hwc = 0, hh = 0, hwb = 0;
	uint64_t vwb;
	uint8_t *cpu, buf[16];
	struct pw_bo *bo;

	if (!menv_init(&e, PW_PT_FORMAT_AARCH64_4K, true))
		goto out;
	CHECK_EQ(create_bo(&e, 8192, PANFROST_BO_NOEXEC, &hwc, NULL), PW_OK);
	CHECK_EQ(create_bo(&e, (uint32_t)PW_SZ_2M, PANFROST_BO_HEAP | PANFROST_BO_NOEXEC, &hh,
			   NULL), PW_OK);
	memset(&cb, 0, sizeof(cb));
	cb.size = 8192;
	cb.flags = PANFROST_BO_WB_MMAP;
	CHECK_EQ(pw_jm_bo_create_user(e.file, &cb, ~0u), PW_OK);
	hwb = cb.handle;
	vwb = cb.offset;

	/* not available at the reported 1.5 */
	memset(&sb, 0, sizeof(sb));
	memset(&qi, 0, sizeof(qi));
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_SYNC_BO, &sb), PW_ENOTTY);
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_QUERY_BO_INFO, &qi), PW_ENOTTY);

	/* QUERY_BO_INFO */
	qi.handle = 999;
	CHECK_EQ(pw_jm_ioctl_query_bo_info(e.file, &qi), PW_ENOENT);
	qi.handle = hh;
	qi.pad = 0xdead;
	qi.extra_flags = 0xbeef;
	CHECK_EQ(pw_jm_ioctl_query_bo_info(e.file, &qi), PW_OK);
	CHECK_EQ(qi.create_flags, PANFROST_BO_HEAP | PANFROST_BO_NOEXEC);
	CHECK_EQ(qi.extra_flags, 0);
	CHECK_EQ(qi.pad, 0);
	qi.handle = hwb;
	CHECK_EQ(pw_jm_ioctl_query_bo_info(e.file, &qi), PW_OK);
	CHECK_EQ(qi.create_flags, PANFROST_BO_WB_MMAP);
	qi.handle = hwc;
	CHECK_EQ(pw_jm_ioctl_query_bo_info(e.file, &qi), PW_OK);
	CHECK_EQ(qi.create_flags, PANFROST_BO_NOEXEC);

	/* SYNC_BO errnos */
	memset(ops, 0, sizeof(ops));
	sb.pad = 1;
	CHECK_EQ(pw_jm_ioctl_sync_bo(e.file, &sb), PW_EINVAL);
	sb.pad = 0;
	sb.op_count = 0;
	CHECK_EQ(pw_jm_ioctl_sync_bo(e.file, &sb), PW_OK);		/* nothing to copy */
	sb.op_count = 1;
	sb.ops = 0;
	CHECK_EQ(pw_jm_ioctl_sync_bo(e.file, &sb), PW_EFAULT);
	sb.op_count = PW_JM_MAX_SYNC_OPS + 1;
	CHECK_EQ(pw_jm_ioctl_sync_bo(e.file, &sb), PW_ENOMEM);
	sb.ops = (uint64_t)(uintptr_t)ops;
	sb.op_count = 1;
	ops[0].handle = 999;
	CHECK_EQ(pw_jm_ioctl_sync_bo(e.file, &sb), PW_ENOENT);
	ops[0].handle = hwc;
	ops[0].type = 2;
	CHECK_EQ(pw_jm_ioctl_sync_bo(e.file, &sb), PW_EINVAL);
	ops[0].type = PANFROST_BO_SYNC_CPU_CACHE_FLUSH;
	ops[0].offset = 4096;
	ops[0].size = 8192;
	CHECK_EQ(pw_jm_ioctl_sync_bo(e.file, &sb), PW_EINVAL);		/* past the end */
	ops[0].offset = 0xfffff000u;
	ops[0].size = 0x2000;
	CHECK_EQ(pw_jm_ioctl_sync_bo(e.file, &sb), PW_EINVAL);		/* u32 wrap */
	ops[0].offset = 0;
	ops[0].size = 8192;
	CHECK_EQ(pw_jm_ioctl_sync_bo(e.file, &sb), PW_OK);		/* WC: no-op */
	ops[0].handle = hh;
	ops[0].size = 4096;
	CHECK_EQ(pw_jm_ioctl_sync_bo(e.file, &sb), PW_OK);		/* heap: no-op */

	/* cached BO: CPU writes reach the GPU only after a flush, GPU writes the CPU only after
	 * an invalidate */
	CHECK_EQ(bind_as(&e, &b), PW_OK);
	bo = bo_of(&e, hwb);
	REQUIRE(bo);
	cpu = mock_mem_user_view(e.os, bo->mem);
	REQUIRE(cpu);
	memset(cpu, 0x11, 8192);
	CHECK_EQ(mock_gpu_read(e.os, (unsigned int)b.as, vwb, buf, sizeof(buf)), 0);
	CHECK(buf[0] != 0x11);
	ops[0].handle = hwb;
	ops[0].type = PANFROST_BO_SYNC_CPU_CACHE_FLUSH;
	ops[0].offset = 0;
	ops[0].size = 4096;
	ops[1] = ops[0];
	ops[1].handle = 999;					/* fails: stops here */
	ops[2] = ops[0];
	ops[2].offset = 4096;
	sb.op_count = 3;
	CHECK_EQ(pw_jm_ioctl_sync_bo(e.file, &sb), PW_ENOENT);
	CHECK_EQ(mock_gpu_read(e.os, (unsigned int)b.as, vwb, buf, sizeof(buf)), 0);
	CHECK_EQ(buf[0], 0x11);					/* op 0 ran */
	CHECK_EQ(mock_gpu_read(e.os, (unsigned int)b.as, vwb + 4096, buf, sizeof(buf)), 0);
	CHECK(buf[0] != 0x11);					/* op 2 did not */
	memset(buf, 0x22, sizeof(buf));
	CHECK_EQ(mock_gpu_write(e.os, (unsigned int)b.as, vwb + 16, buf, sizeof(buf)), 0);
	CHECK_EQ(cpu[16], 0x11);
	ops[0].type = PANFROST_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE;
	ops[0].offset = 16;
	ops[0].size = 16;
	sb.op_count = 1;
	CHECK_EQ(pw_jm_ioctl_sync_bo(e.file, &sb), PW_OK);
	CHECK_EQ(cpu[16], 0x22);
	CHECK_EQ(cpu[31], 0x22);
	ops[0].size = 0;
	CHECK_EQ(pw_jm_ioctl_sync_bo(e.file, &sb), PW_OK);		/* empty range */
	unbind_as(&e, &b);
out:
	jenv_fini(&e);
}

/* ---- B6: WAIT_BO and the per-BO fence ---- */

struct signal_ctx {
	struct pw_fence *f;
	unsigned int delay_ms;
	int error;
};

static void *signal_thread(void *arg)
{
	struct signal_ctx *c = arg;

	usleep(c->delay_ms * 1000);
	pw_fence_signal(c->f, c->error);
	return NULL;
}

static void attach_fence(struct jenv *e, uint32_t handle, struct pw_fence *f)
{
	struct pw_bo *bo = pw_file_bo_get(e->file, handle);

	REQUIRE(bo);
	pw_jm_resv_lock(e->dev);
	pw_jm_bo_fence_set(bo, f);
	pw_jm_resv_unlock(e->dev);
	pw_bo_put(bo);
}

static void test_jm_wait_bo(void)
{
	struct jenv e;
	struct drm_panfrost_wait_bo wb;
	struct pw_fence *f1 = NULL, *f2 = NULL, *got;
	struct signal_ctx sc;
	pthread_t thr;
	uint32_t h = 0;
	uint64_t t0;
	int ret;

	if (!jenv_init(&e))
		goto out;
	CHECK_EQ(create_bo(&e, 4096, PANFROST_BO_NOEXEC, &h, NULL), PW_OK);
	memset(&wb, 0, sizeof(wb));
	wb.handle = 12345;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_WAIT_BO, &wb), PW_ENOENT);
	wb.handle = h;
	wb.pad = 1;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_WAIT_BO, &wb), PW_EINVAL);
	wb.pad = 0;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_WAIT_BO, &wb), PW_OK);	/* never used: idle */
	CHECK(pw_jm_bo_fence_get(bo_of(&e, h)) == NULL);
	CHECK(pw_jm_bo_fence_get(NULL) == NULL);

	/* pending fence */
	f1 = pw_fence_create(e.dev, 1);
	REQUIRE(f1);
	attach_fence(&e, h, f1);
	got = pw_jm_bo_fence_get(bo_of(&e, h));
	CHECK(got == f1);
	pw_fence_put(got);
	wb.timeout_ns = 0;					/* poll */
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_WAIT_BO, &wb), PW_EBUSY);
	wb.timeout_ns = -5;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_WAIT_BO, &wb), PW_EBUSY);
	wb.timeout_ns = (int64_t)pw_os_time_ns(e.os) - 1000;	/* absolute, in the past */
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_WAIT_BO, &wb), PW_EBUSY);
	t0 = pw_os_time_ns(e.os);
	wb.timeout_ns = (int64_t)(t0 + 30000000ull);		/* +30 ms, absolute */
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_WAIT_BO, &wb), PW_ETIMEDOUT);
	CHECK(pw_os_time_ns(e.os) >= t0 + 30000000ull);

	/* signalled from another thread while waiting (forever) */
	sc.f = f1;
	sc.delay_ms = 40;
	sc.error = PW_EINVAL;				/* a failed job still makes the BO idle */
	REQUIRE(pthread_create(&thr, NULL, signal_thread, &sc) == 0);
	t0 = pw_os_time_ns(e.os);
	wb.timeout_ns = INT64_MAX;
	ret = JIOC(&e, DRM_IOCTL_PANFROST_WAIT_BO, &wb);
	CHECK_EQ(ret, PW_OK);
	CHECK(pw_os_time_ns(e.os) - t0 >= 30000000ull);
	pthread_join(thr, NULL);
	wb.timeout_ns = 0;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_WAIT_BO, &wb), PW_OK);	/* signalled: poll OK */

	/* a new job replaces the fence (the old reference is dropped); finite deadline */
	f2 = pw_fence_create(e.dev, 2);
	REQUIRE(f2);
	attach_fence(&e, h, f2);
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_WAIT_BO, &wb), PW_EBUSY);
	sc.f = f2;
	sc.delay_ms = 20;
	sc.error = 0;
	REQUIRE(pthread_create(&thr, NULL, signal_thread, &sc) == 0);
	wb.timeout_ns = (int64_t)(pw_os_time_ns(e.os) + 5000000000ull);	/* +5 s */
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_WAIT_BO, &wb), PW_OK);
	pthread_join(thr, NULL);
	/* kernel BOs have no fence: set/get are no-ops */
	pw_jm_bo_fence_set(NULL, f2);
	pw_fence_put(f1);
	pw_fence_put(f2);
	/* the BO still holds f2 until it is freed: covered by the leak check at fini */
out:
	jenv_fini(&e);
}

/* ---- GEM_CLOSE while a job holds the BO and its fence is pending ---- */

static void test_jm_close_busy_bo(void)
{
	struct jenv e;
	struct as_bind b = { -1, false };
	struct drm_panfrost_wait_bo wb;
	struct mock_as_state a0, a1;
	struct pw_bo **bos = NULL;
	struct pw_fence *f;
	uint32_t ha = 0, hb = 0, hc = 0;
	uint64_t va = 0, vb = 0, vc = 0;

	if (!jenv_init(&e))
		goto out;
	CHECK_EQ(bind_as(&e, &b), PW_OK);
	CHECK_EQ(create_bo(&e, 4096, PANFROST_BO_NOEXEC, &ha, &va), PW_OK);
	CHECK_EQ(pw_jm_bos_lookup(e.file, (uint64_t)(uintptr_t)&ha, 1, &bos), PW_OK);	/* "job" */
	REQUIRE(bos);
	f = pw_fence_create(e.dev, 1);
	REQUIRE(f);
	attach_fence(&e, ha, f);

	CHECK_EQ(gem_close(&e, ha), PW_OK);
	memset(&wb, 0, sizeof(wb));
	wb.handle = ha;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_WAIT_BO, &wb), PW_ENOENT);	/* handle gone */
	CHECK_EQ(bos[0]->refcount, 1);					/* the job's */
	CHECK_EQ(gpu_tr(&e, b.as, va, 'w', NULL), 0);			/* still mapped */
	CHECK_EQ(create_bo(&e, 4096, PANFROST_BO_NOEXEC, &hb, &vb), PW_OK);
	CHECK(vb != va);						/* VA still taken */

	/* job completes and drops its references: unmapped (FLUSH_MEM), VA reusable */
	pw_fence_signal(f, 0);
	pw_fence_put(f);
	mock_get_as(e.os, (unsigned int)b.as, &a0);
	pw_jm_bos_put(e.dev, bos, 1);
	mock_get_as(e.os, (unsigned int)b.as, &a1);
	CHECK_EQ(a1.flush_mems, a0.flush_mems + 1);
	CHECK(gpu_tr(&e, b.as, va, 'r', NULL) >= 0xc0);
	CHECK_EQ(create_bo(&e, 4096, PANFROST_BO_NOEXEC, &hc, &vc), PW_OK);
	CHECK_EQ(vc, va);
	unbind_as(&e, &b);
out:
	jenv_fini(&e);
}

/* ---- B7: SUBMIT's BO list ---- */

static void test_jm_bos_lookup(void)
{
	struct jenv e;
	struct pw_bo **bos = (struct pw_bo **)1, *kbo = NULL;
	uint32_t h1 = 0, h2 = 0, hk = 0, hs[4];

	if (!jenv_init(&e))
		goto out;
	CHECK_EQ(create_bo(&e, 4096, PANFROST_BO_NOEXEC, &h1, NULL), PW_OK);
	CHECK_EQ(create_bo(&e, 4096, 0, &h2, NULL), PW_OK);
	CHECK_EQ(pw_bo_alloc(e.dev, 4096, PW_MEM_UNCACHED, false, true, &kbo), PW_OK);
	REQUIRE(kbo);
	CHECK_EQ(pw_handle_alloc(e.os, &e.file->bos, kbo, &hk), PW_OK);

	CHECK_EQ(pw_jm_bos_lookup(e.file, 0, 0, &bos), PW_OK);
	CHECK(bos == NULL);
	CHECK_EQ(pw_jm_bos_lookup(e.file, 0, 2, &bos), PW_EFAULT);
	CHECK(bos == NULL);
	CHECK_EQ(pw_jm_bos_lookup(e.file, (uint64_t)(uintptr_t)hs, PW_JM_MAX_JOB_BOS + 1, &bos),
		 PW_ENOMEM);

	/* duplicates are fine and each holds a reference */
	hs[0] = h1;
	hs[1] = h1;
	hs[2] = h2;
	CHECK_EQ(pw_jm_bos_lookup(e.file, (uint64_t)(uintptr_t)hs, 3, &bos), PW_OK);
	REQUIRE(bos);
	CHECK(bos[0] == bos[1] && bos[0] == bo_of(&e, h1));
	CHECK(bos[2] == bo_of(&e, h2));
	CHECK_EQ(bos[0]->refcount, 3);
	pw_jm_bos_put(e.dev, bos, 3);
	CHECK_EQ(bo_of(&e, h1)->refcount, 1);

	/* any unknown handle -> ENOENT, a BO without a mapping here -> EINVAL; nothing held */
	hs[2] = 777;
	CHECK_EQ(pw_jm_bos_lookup(e.file, (uint64_t)(uintptr_t)hs, 3, &bos), PW_ENOENT);
	CHECK(bos == NULL);
	hs[2] = hk;
	CHECK_EQ(pw_jm_bos_lookup(e.file, (uint64_t)(uintptr_t)hs, 3, &bos), PW_EINVAL);
	CHECK(bos == NULL);
	CHECK_EQ(bo_of(&e, h1)->refcount, 1);
	CHECK_EQ(bo_of(&e, hk)->refcount, 1);
	pw_jm_bos_put(e.dev, NULL, 3);
out:
	jenv_fini(&e);
}

/* ---- B8: HEAP growth on GPU faults, both formats ---- */

static bool fault_handled(struct jenv *e, int as, uint64_t va)
{
	return gpu_tr(e, as, va, 'w', NULL) == 0 && !(mock_rawstat(e->os, 1) & (1u << as));
}

static bool fault_unhandled(struct jenv *e, int as)
{
	struct mock_as_state st;

	mock_get_as(e->os, (unsigned int)as, &st);
	return mock_log_contains(e->os, "Unhandled page fault") && !st.enabled &&
	       !(mock_rawstat(e->os, 1) & (1u << as));
}

static void heap_fault_body(enum pw_pt_format fmt)
{
	struct jenv e;
	struct as_bind b = { -1, false };
	struct mock_as_state st;
	uint32_t hh = 0, hn = 0;
	uint64_t vh = 0, vn = 0, pa1 = 0, pa2 = 0;
	uint8_t buf[32], zero[32];
	long mems;

	printf("    format %s\n", fmt_name(fmt));
	if (!menv_init(&e, fmt, true))
		goto out;
	CHECK_EQ(create_bo(&e, (uint32_t)(6 * SZ_1M + 1), PANFROST_BO_HEAP | PANFROST_BO_NOEXEC,
			   &hh, &vh), PW_OK);
	CHECK_EQ(bo_of(&e, hh)->size, 8 * SZ_1M);
	CHECK_EQ(create_bo(&e, 4096, PANFROST_BO_NOEXEC, &hn, &vn), PW_OK);
	CHECK_EQ(bind_as(&e, &b), PW_OK);
	CHECK(gpu_tr(&e, b.as, vh + 3 * SZ_1M, 'w', NULL) >= 0xc0);
	mock_log_clear(e.os);

	/* translation fault (write, level 3) inside chunk 1 -> [vh + 2M, vh + 4M) populated */
	mock_inject_mmu_fault(e.os, (unsigned int)b.as, 0x3c3, vh + 3 * SZ_1M + 0x123);
	WAIT_FOR(fault_handled(&e, b.as, vh + 3 * SZ_1M));
	CHECK(fault_handled(&e, b.as, vh + 3 * SZ_1M));
	CHECK_EQ(gpu_tr(&e, b.as, vh + 2 * SZ_1M, 'r', &pa1), 0);
	CHECK_EQ(gpu_tr(&e, b.as, vh + 4 * SZ_1M - 4096, 'w', NULL), 0);
	CHECK(gpu_tr(&e, b.as, vh + 2 * SZ_1M - 4096, 'r', NULL) >= 0xc0);
	CHECK(gpu_tr(&e, b.as, vh + 4 * SZ_1M, 'r', NULL) >= 0xc0);
	CHECK_EQ(gpu_tr(&e, b.as, vh + 3 * SZ_1M, 'x', NULL), 0xc8 + 3);	/* NOEXEC */
	mock_get_as(e.os, (unsigned int)b.as, &st);
	CHECK(st.enabled);
	CHECK_EQ(e.dev->mmu.faulty_mask & (1u << b.as), 0);
	CHECK(!mock_log_contains(e.os, "Unhandled"));
	memset(zero, 0, sizeof(zero));
	CHECK_EQ(mock_gpu_read(e.os, (unsigned int)b.as, vh + 2 * SZ_1M, buf, sizeof(buf)), 0);
	CHECK_EQ(memcmp(buf, zero, sizeof(buf)), 0);		/* zeroed pages */
	memset(buf, 0x77, sizeof(buf));
	CHECK_EQ(mock_gpu_write(e.os, (unsigned int)b.as, vh + 2 * SZ_1M, buf, sizeof(buf)), 0);

	/* a second fault in the same chunk changes nothing */
	mems = mock_live_mems(e.os);
	mock_inject_mmu_fault(e.os, (unsigned int)b.as, 0x2c3, vh + 2 * SZ_1M + 0x5000);
	WAIT_FOR(!(mock_rawstat(e.os, 1) & (1u << b.as)));
	usleep(10000);
	CHECK_EQ(mock_live_mems(e.os), mems);
	CHECK_EQ(gpu_tr(&e, b.as, vh + 2 * SZ_1M, 'r', &pa2), 0);
	CHECK_EQ(pa2, pa1);
	CHECK_EQ(mock_gpu_read(e.os, (unsigned int)b.as, vh + 2 * SZ_1M, buf, sizeof(buf)), 0);
	CHECK_EQ(buf[0], 0x77);
	CHECK(!mock_log_contains(e.os, "Unhandled"));

	/* allocation failure -> unhandled (AS disabled, the job would fail); retry works */
	mock_fail_mem_allocs(e.os, 1);
	mock_inject_mmu_fault(e.os, (unsigned int)b.as, 0x3c3, vh + 7 * SZ_1M);
	WAIT_FOR(fault_unhandled(&e, b.as));
	CHECK(fault_unhandled(&e, b.as));
	CHECK(e.dev->mmu.faulty_mask & (1u << b.as));
	rebind_as(&e, &b);
	mock_log_clear(e.os);
	mock_inject_mmu_fault(e.os, (unsigned int)b.as, 0x3c3, vh + 7 * SZ_1M);
	WAIT_FOR(fault_handled(&e, b.as, vh + 7 * SZ_1M));
	CHECK(fault_handled(&e, b.as, vh + 7 * SZ_1M));
	CHECK_EQ(gpu_tr(&e, b.as, vh + 6 * SZ_1M, 'r', NULL), 0);
	CHECK(!mock_log_contains(e.os, "Unhandled"));

	/* not a translation fault (permission) in the heap -> unhandled */
	mock_inject_mmu_fault(e.os, (unsigned int)b.as, 0x3cb, vh);
	WAIT_FOR(fault_unhandled(&e, b.as));
	CHECK(fault_unhandled(&e, b.as));
	rebind_as(&e, &b);
	CHECK(gpu_tr(&e, b.as, vh, 'r', NULL) >= 0xc0);		/* not populated */

	/* a fault in a BO that is not a heap -> unhandled */
	mock_log_clear(e.os);
	mock_inject_mmu_fault(e.os, (unsigned int)b.as, 0x3c3, vn);
	WAIT_FOR(fault_unhandled(&e, b.as));
	CHECK(fault_unhandled(&e, b.as));
	CHECK(mock_log_contains(e.os, "not a heap"));
	rebind_as(&e, &b);

	/* outside any BO -> unhandled */
	mock_log_clear(e.os);
	mock_inject_mmu_fault(e.os, (unsigned int)b.as, 0x2c2, 3ull << 30);
	WAIT_FOR(fault_unhandled(&e, b.as));
	CHECK(fault_unhandled(&e, b.as));
	rebind_as(&e, &b);

	/* GEM_CLOSE frees the two populated chunks (page tables stay until VM destroy) */
	mems = mock_live_mems(e.os);
	CHECK_EQ(gem_close(&e, hh), PW_OK);
	CHECK_EQ(mock_live_mems(e.os), mems - 2);
	CHECK(gpu_tr(&e, b.as, vh + 2 * SZ_1M, 'r', NULL) >= 0xc0);
	unbind_as(&e, &b);
out:
	jenv_fini(&e);
}

static void test_jm_heap_fault(void)
{
	heap_fault_body(PW_PT_FORMAT_AARCH64_4K);
	heap_fault_body(PW_PT_FORMAT_MALI_LPAE);
}

/*
 * (review) HEAP growth end to end through jm-hw's mock executor: WRITE_VALUE jobs (Mesa genxml
 * v7.xml Job Header + payload) target two unpopulated chunks of a heap BO, the executor's
 * page faults go through the real MMU IRQ path (pw_mmu_irq_handler -> pw_jm_mmu_fault), the
 * job is retried and completes. Then a job writing outside any BO fails, both formats.
 */
struct hj_log {
	pthread_mutex_t m;
	unsigned int n;
	struct pw_jm_irq_event last;
};

static void hj_hook(struct pw_device *dev, const struct pw_jm_irq_event *ev, void *ctx)
{
	struct hj_log *l = ctx;

	(void)dev;
	pthread_mutex_lock(&l->m);
	l->last = *ev;
	l->n++;
	pthread_mutex_unlock(&l->m);
}

static bool hj_wait(struct hj_log *l, unsigned int n, struct pw_jm_irq_event *out)
{
	bool ok = false;

	for (int i = 0; i < 2000 && !ok; i++) {
		pthread_mutex_lock(&l->m);
		ok = l->n >= n;
		if (ok)
			*out = l->last;
		pthread_mutex_unlock(&l->m);
		if (!ok)
			usleep(1000);
	}
	return ok;
}

static void hj_wv(uint8_t *p, uint32_t index, uint64_t next, uint64_t addr, uint32_t wtype,
		  uint64_t imm)
{
	uint32_t w = 1u | (2u << 1) | (index << 16);	/* Is 64b, Type = WRITE_VALUE, Index */

	memset(p, 0, 64);
	memcpy(p + 16, &w, 4);
	memcpy(p + 24, &next, 8);
	memcpy(p + 32, &addr, 8);
	memcpy(p + 40, &wtype, 4);
	memcpy(p + 48, &imm, 8);
}

static void heap_job_body(enum pw_pt_format fmt)
{
	struct jenv e;
	struct as_bind b = { -1, false };
	struct hj_log l;
	struct pw_jm_irq_event ev;
	struct mock_js_stats s0, s1;
	struct mock_as_state a0, a1;
	uint32_t hj = 0, hh = 0, v32 = 0;
	uint64_t vj = 0, vh = 0, v64 = 0;
	uint8_t jobs[256];
	long mems;

	printf("    format %s\n", fmt_name(fmt));
	memset(&l, 0, sizeof(l));
	pthread_mutex_init(&l.m, NULL);
	if (!menv_init(&e, fmt, true))
		goto out;
	pw_jm_hw_set_irq_hook(e.dev, hj_hook, &l);
	CHECK_EQ(create_bo(&e, 4096, PANFROST_BO_NOEXEC, &hj, &vj), PW_OK);
	CHECK_EQ(create_bo(&e, (uint32_t)(4 * SZ_1M), PANFROST_BO_HEAP | PANFROST_BO_NOEXEC,
			   &hh, &vh), PW_OK);
	CHECK_EQ(bind_as(&e, &b), PW_OK);
	if (b.as < 0)
		goto out_hook;
	/* the descriptors, written through the GPU view (the BO is write-combined) */
	hj_wv(jobs, 1, vj + 128, vh + 2 * SZ_1M + 0x40, 6 /* IMMEDIATE_32 */, 0x600d);
	hj_wv(jobs + 128, 2, 0, vh + 0x80, 7 /* IMMEDIATE_64 */, 0x1122334455667788ull);
	CHECK_EQ(mock_gpu_write(e.os, (unsigned int)b.as, vj, jobs, sizeof(jobs)), 0);
	mock_jm_js_stats(e.os, 1, &s0);
	mock_get_as(e.os, (unsigned int)b.as, &a0);
	mock_log_clear(e.os);

	pw_jm_slot_submit(e.dev, 1, vj, (uint32_t)b.as, pw_jm_latest_flush_id(e.dev), false);
	CHECK(hj_wait(&l, 1, &ev));
	CHECK_EQ(ev.done, PW_BIT(1));
	CHECK_EQ(ev.failed, 0);
	mock_jm_js_stats(e.os, 1, &s1);
	CHECK_EQ(s1.done, s0.done + 1);
	CHECK_EQ(s1.page_faults, s0.page_faults + 2);		/* one per chunk */
	CHECK_EQ(s1.fault_retries, s0.fault_retries + 2);
	mock_get_as(e.os, (unsigned int)b.as, &a1);
	CHECK_EQ(a1.flush_pts, a0.flush_pts + 2);		/* one map per chunk */
	CHECK(a1.enabled);
	CHECK(!mock_log_contains(e.os, "Unhandled"));
	CHECK_EQ(mock_gpu_read(e.os, (unsigned int)b.as, vh + 2 * SZ_1M + 0x40, &v32, 4), 0);
	CHECK_EQ(v32, 0x600d);
	CHECK_EQ(mock_gpu_read(e.os, (unsigned int)b.as, vh + 0x80, &v64, 8), 0);
	CHECK_EQ(v64, 0x1122334455667788ull);
	CHECK(gpu_tr(&e, b.as, vh + 4 * SZ_1M - 4096, 'r', NULL) == 0);
	CHECK(gpu_tr(&e, b.as, vh + 4 * SZ_1M, 'r', NULL) >= 0xc0);	/* past the BO */

	/* a job writing outside any BO: unhandled fault, AS disabled, the chain fails */
	hj_wv(jobs, 1, 0, 3ull << 30, 6, 1);
	CHECK_EQ(mock_gpu_write(e.os, (unsigned int)b.as, vj, jobs, 64), 0);
	pw_jm_slot_submit(e.dev, 1, vj, (uint32_t)b.as, pw_jm_latest_flush_id(e.dev), false);
	CHECK(hj_wait(&l, 2, &ev));
	CHECK_EQ(ev.done, 0);
	CHECK_EQ(ev.failed, PW_BIT(1));
	CHECK(mock_log_contains(e.os, "Unhandled page fault"));
	unbind_as(&e, &b);

	/* GEM_CLOSE frees both chunks */
	mems = mock_live_mems(e.os);
	CHECK_EQ(gem_close(&e, hh), PW_OK);
	CHECK_EQ(mock_live_mems(e.os), mems - 2);
out_hook:
	pw_jm_hw_set_irq_hook(e.dev, NULL, NULL);
out:
	jenv_fini(&e);
	pthread_mutex_destroy(&l.m);
}

static void test_jm_heap_job(void)
{
	heap_job_body(PW_PT_FORMAT_AARCH64_4K);
	heap_job_body(PW_PT_FORMAT_MALI_LPAE);
}

/* the fault hook directly: no device state, wrong exception types, unbound AS */
static void test_jm_fault_hook(void)
{
	struct jenv e;

	if (!jenv_init(&e))
		goto out;
	CHECK_EQ(pw_jm_mmu_fault(e.dev, 0, 0xc8, PW_JM_VA_START), PW_EINVAL);	/* permission */
	CHECK_EQ(pw_jm_mmu_fault(e.dev, 0, 0xd8, PW_JM_VA_START), PW_EINVAL);	/* access flag */
	CHECK_EQ(pw_jm_mmu_fault(e.dev, 0, 0xc1, PW_JM_VA_START), PW_ENOENT);	/* no VM bound */
	CHECK_EQ(pw_jm_mmu_fault(e.dev, 40, 0xc1, PW_JM_VA_START), PW_ENOENT);
out:
	jenv_fini(&e);
}

const struct test_case jm_mem_tests[] = {
	{ "jm_bo_ext_hooks", test_jm_bo_ext_hooks },
	{ "jm_file_vm", test_jm_file_vm },
	{ "jm_create_bo", test_jm_create_bo },
	{ "jm_va_rules", test_jm_va_rules },
	{ "jm_gpu_view", test_jm_gpu_view },
	{ "jm_lpae_tables", test_jm_lpae_tables },
	{ "jm_pt_format_select", test_jm_pt_format_select },
	{ "jm_bo_ioctls", test_jm_bo_ioctls },
	{ "jm_sync_query", test_jm_sync_query },
	{ "jm_wait_bo", test_jm_wait_bo },
	{ "jm_close_busy_bo", test_jm_close_busy_bo },
	{ "jm_bos_lookup", test_jm_bos_lookup },
	{ "jm_heap_fault", test_jm_heap_fault },
	{ "jm_heap_job", test_jm_heap_job },
	{ "jm_fault_hook", test_jm_fault_hook },
	{ NULL, NULL },
};
