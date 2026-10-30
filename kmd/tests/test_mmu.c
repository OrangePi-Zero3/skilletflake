// SPDX-License-Identifier: MIT
/*
 * test_mmu.c - page tables (4K/2M, attributes, split, rollback, flush discipline) and AS
 * programming, verified with the mock's independent page-table walker reading "DRAM".
 */
#include <stdlib.h>
#include <string.h>

#include "mock_gpu.h"
#include "pw_mmu.h"
#include "test.h"

struct env {
	struct pw_os *os;
	struct pw_device *dev;
};

/* GPU + MMU initialised, no firmware (uses core internals). */
static bool env_init(struct env *e, const struct mock_config *cfg)
{
	struct pw_device_params p = { 0 };

	e->os = mock_create(cfg);
	p.os = e->os;
	p.boot_firmware = false;
	if (pw_device_alloc(&p, &e->dev) || pw_device_probe(e->dev)) {
		CHECK(!"device probe failed");
		return false;
	}
	if (pw_mmu_init(e->dev)) {
		CHECK(!"mmu init failed");
		return false;
	}
	mock_attach_irq(e->os, e->dev);
	return true;
}

static void env_fini(struct env *e)
{
	mock_detach_irq(e->os);
	pw_device_destroy(e->dev);
	check_no_leaks(e->os);
	CHECK_EQ(mock_model_errors(e->os), 0);
	CHECK_EQ(mock_irq_unclaimed(e->os), 0);
	mock_destroy(e->os);
}

/* Check one VA through both walkers. */
static void check_va(struct env *e, struct pw_vm *vm, uint64_t va, uint64_t want_pa,
		     int want_level)
{
	uint64_t pa = 0, pte = 0, gpa = 0, gpte = 0;
	int lvl = -1, glvl = -1, r;

	r = pw_vm_translate(vm, va, &pa, &pte, &lvl);
	CHECK_EQ(r, PW_OK);
	CHECK_EQ(pa, want_pa);
	CHECK_EQ(lvl, want_level);
	if (vm->as >= 0) {
		r = mock_gpu_translate(e->os, (unsigned int)vm->as, va, 'r', &gpa, &glvl, &gpte);
		CHECK_EQ(r, 0);
		CHECK_EQ(gpa, want_pa);
		CHECK_EQ(glvl, want_level);
		CHECK_EQ(gpte, pte);	/* GPU sees exactly what the CPU wrote (flushed) */
	}
}

static void test_vm_basic(void)
{
	struct env e;
	struct pw_vm *vm;
	struct pw_os_mem *mem;
	struct mock_as_state as;

	if (!env_init(&e, NULL))
		return;
	REQUIRE(pw_vm_create(e.dev, false, &vm) == PW_OK);
	CHECK_EQ(vm->start_level, 0);		/* 48-bit VA: 4 levels */
	CHECK_EQ(vm->va_size, 1ull << 48);
	CHECK_EQ(pw_vm_root_pa(vm) & 0xfff, 0);
	CHECK(pw_vm_root_pa(vm) >= MOCK_PA_BASE);	/* above 4 GiB on purpose */
	CHECK_EQ(pw_vm_transcfg(vm), 0x420001c6ull);	/* WB|RA|AARCH64_4K|INA_BITS(7) */

	REQUIRE(pw_os_mem_alloc(e.os, 16 * 4096, PW_MEM_KMAP, &mem) == PW_OK);
	/* the mock never hands out contiguous pages without PW_MEM_CONTIGUOUS */
	CHECK(pw_os_mem_page_pa(mem, 1) != pw_os_mem_page_pa(mem, 0) + 4096);
	CHECK_EQ(pw_vm_map_mem(vm, 0x1000000, mem, 0, 16 * 4096, 0), PW_OK);
	/* not active yet: no AS commands */
	CHECK_EQ(vm->as, -1);
	REQUIRE(pw_vm_activate(vm) == PW_OK);
	CHECK_EQ(vm->as, 1);
	mock_get_as(e.os, 1, &as);
	CHECK(as.enabled);
	CHECK_EQ(as.transtab, pw_vm_root_pa(vm));
	CHECK_EQ(as.transcfg, 0x420001c6ull);
	CHECK_EQ(as.memattr, 0x4c4c4c4c8c4c8f4cull);

	for (unsigned int i = 0; i < 16; i++)
		check_va(&e, vm, 0x1000000 + i * 4096 + 0x123, pw_os_mem_page_pa(mem, i) + 0x123, 3);

	/* default attributes: RW, exec, cached */
	{
		uint64_t pte = 0;

		CHECK_EQ(pw_vm_translate(vm, 0x1000000, NULL, &pte, NULL), PW_OK);
		CHECK_EQ(pte & 3, 3);
		CHECK(pte & PW_PTE_AF);
		CHECK(!(pte & PW_PTE_AP_RDONLY));
		CHECK(!(pte & PW_PTE_XN));
		CHECK_EQ(pte & PW_PTE_ATTRINDX_MASK, PW_PTE_ATTRINDX(PW_MAIR_IDX_WB));
		CHECK_EQ(pte & PW_PTE_SH_MASK, PW_PTE_SH_IS);
		CHECK_EQ(mock_gpu_translate(e.os, 1, 0x1000000, 'w', NULL, NULL, NULL), 0);
		CHECK_EQ(mock_gpu_translate(e.os, 1, 0x1000000, 'x', NULL, NULL, NULL), 0);
	}
	/* unmapped neighbours fault at the right level */
	CHECK_EQ(mock_gpu_translate(e.os, 1, 0x1000000 + 16 * 4096, 'r', NULL, NULL, NULL), 0xc3);
	CHECK_EQ(mock_gpu_translate(e.os, 1, 0x10000000, 'r', NULL, NULL, NULL), 0xc2);
	CHECK_EQ(mock_gpu_translate(e.os, 1, 0x80000000, 'r', NULL, NULL, NULL), 0xc1);
	CHECK_EQ(mock_gpu_translate(e.os, 1, 0x800000000000ull, 'r', NULL, NULL, NULL), 0xc0);
	CHECK_EQ(pw_vm_translate(vm, 0x1000000 + 16 * 4096, NULL, NULL, NULL), PW_ENOENT);

	/* unmap */
	CHECK_EQ(pw_vm_unmap(vm, 0x1000000 + 4096, 2 * 4096), PW_OK);
	CHECK_EQ(mock_gpu_translate(e.os, 1, 0x1001000, 'r', NULL, NULL, NULL), 0xc3);
	CHECK_EQ(mock_gpu_translate(e.os, 1, 0x1002000, 'r', NULL, NULL, NULL), 0xc3);
	check_va(&e, vm, 0x1003000, pw_os_mem_page_pa(mem, 3), 3);
	check_va(&e, vm, 0x1000000, pw_os_mem_page_pa(mem, 0), 3);
	/* unmapping holes is fine */
	CHECK_EQ(pw_vm_unmap(vm, 0x1000000, 64 * 4096), PW_OK);
	CHECK_EQ(mock_gpu_translate(e.os, 1, 0x1000000, 'r', NULL, NULL, NULL), 0xc3);

	pw_vm_destroy(vm);
	mock_get_as(e.os, 1, &as);
	CHECK(!as.enabled);			/* UPDATE with ADRMODE_UNMAPPED */
	pw_os_mem_free(e.os, mem);
	env_fini(&e);
}

static void test_attrs(void)
{
	static const struct {
		uint32_t flags;
		int w, x;	/* expected mock fault for write / exec, 0 = allowed */
		uint64_t idx, sh;
	} cases[] = {
		{ 0, 0, 0, PW_MAIR_IDX_WB, PW_PTE_SH_IS },
		{ PW_VM_MAP_READONLY, 0xcb, 0, PW_MAIR_IDX_WB, PW_PTE_SH_IS },
		{ PW_VM_MAP_NOEXEC, 0, 0xcb, PW_MAIR_IDX_WB, PW_PTE_SH_IS },
		{ PW_VM_MAP_UNCACHED, 0, 0, PW_MAIR_IDX_NC, PW_PTE_SH_OS },
		{ PW_VM_MAP_READONLY | PW_VM_MAP_NOEXEC | PW_VM_MAP_UNCACHED, 0xcb, 0xcb,
		  PW_MAIR_IDX_NC, PW_PTE_SH_OS },
	};
	struct env e;
	struct pw_vm *vm;
	struct pw_os_mem *mem;

	if (!env_init(&e, NULL))
		return;
	REQUIRE(pw_vm_create(e.dev, false, &vm) == PW_OK);
	REQUIRE(pw_vm_activate(vm) == PW_OK);
	REQUIRE(pw_os_mem_alloc(e.os, 4096 * 8, PW_MEM_KMAP, &mem) == PW_OK);
	for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint64_t va = 0x200000000ull + i * 0x10000, pte;

		CHECK_EQ(pw_vm_map_mem(vm, va, mem, i * 4096, 4096, cases[i].flags), PW_OK);
		CHECK_EQ(mock_gpu_translate(e.os, 1, va, 'r', NULL, NULL, &pte), 0);
		CHECK_EQ(mock_gpu_translate(e.os, 1, va, 'w', NULL, NULL, NULL), cases[i].w);
		CHECK_EQ(mock_gpu_translate(e.os, 1, va, 'x', NULL, NULL, NULL), cases[i].x);
		CHECK_EQ(pte & PW_PTE_ATTRINDX_MASK, PW_PTE_ATTRINDX(cases[i].idx));
		CHECK_EQ(pte & PW_PTE_SH_MASK, cases[i].sh);
		CHECK(pte & PW_PTE_AF);
		CHECK(pte & PW_PTE_AP_UNPRIV);
	}
	pw_vm_destroy(vm);
	pw_os_mem_free(e.os, mem);
	env_fini(&e);
}

static void test_blocks(void)
{
	struct env e;
	struct pw_vm *vm;
	struct pw_os_mem *big, *odd;
	uint64_t pa0;

	if (!env_init(&e, NULL))
		return;
	REQUIRE(pw_vm_create(e.dev, false, &vm) == PW_OK);
	REQUIRE(pw_vm_activate(vm) == PW_OK);
	REQUIRE(pw_os_mem_alloc(e.os, 4u << 20, PW_MEM_CONTIGUOUS, &big) == PW_OK);
	pa0 = pw_os_mem_page_pa(big, 0);
	REQUIRE((pa0 & (PW_SZ_2M - 1)) == 0);

	/* aligned VA + aligned contiguous PA -> two 2M blocks */
	CHECK_EQ(pw_vm_map_mem(vm, 0x40000000, big, 0, 4u << 20, PW_VM_MAP_NOEXEC), PW_OK);
	check_va(&e, vm, 0x40000000, pa0, 2);
	check_va(&e, vm, 0x40000000 + 0x1fffff, pa0 + 0x1fffff, 2);
	check_va(&e, vm, 0x40200000 + 0x12345, pa0 + 0x200000 + 0x12345, 2);
	CHECK_EQ(vm->pt_pages, 3);		/* root, L1, L2 only: no level-3 tables */
	{
		uint64_t pte;

		CHECK_EQ(pw_vm_translate(vm, 0x40000000, NULL, &pte, NULL), PW_OK);
		CHECK_EQ(pte & 3, 1);		/* block descriptor */
		CHECK(pte & PW_PTE_XN);
	}

	/* partial unmap splits the first block */
	CHECK_EQ(pw_vm_unmap(vm, 0x40003000, 0x1000), PW_OK);
	CHECK_EQ(mock_gpu_translate(e.os, 1, 0x40003000, 'r', NULL, NULL, NULL), 0xc3);
	check_va(&e, vm, 0x40002000, pa0 + 0x2000, 3);
	check_va(&e, vm, 0x40004000, pa0 + 0x4000, 3);
	check_va(&e, vm, 0x401ff000, pa0 + 0x1ff000, 3);
	{
		uint64_t pte;

		CHECK_EQ(pw_vm_translate(vm, 0x40004000, NULL, &pte, NULL), PW_OK);
		CHECK(pte & PW_PTE_XN);		/* attributes survive the split */
		CHECK_EQ(pte & 3, 3);
	}
	check_va(&e, vm, 0x40200000, pa0 + 0x200000, 2);	/* second block untouched */
	/* full unmap of the second block */
	CHECK_EQ(pw_vm_unmap(vm, 0x40200000, PW_SZ_2M), PW_OK);
	CHECK_EQ(mock_gpu_translate(e.os, 1, 0x40200000, 'r', NULL, NULL, NULL), 0xc2);
	CHECK_EQ(pw_vm_unmap(vm, 0x40000000, 4u << 20), PW_OK);

	/* misaligned VA -> 4K pages */
	CHECK_EQ(pw_vm_map_mem(vm, 0x80001000, big, 0, PW_SZ_2M, 0), PW_OK);
	check_va(&e, vm, 0x80001000, pa0, 3);
	check_va(&e, vm, 0x80200000, pa0 + 0x1ff000, 3);
	/* aligned VA, misaligned PA (offset 4K) -> 4K pages */
	REQUIRE(pw_os_mem_alloc(e.os, (2u << 20) + 4096, PW_MEM_CONTIGUOUS, &odd) == PW_OK);
	CHECK_EQ(pw_vm_map_mem(vm, 0xc0000000, odd, 4096, PW_SZ_2M, 0), PW_OK);
	check_va(&e, vm, 0xc0000000, pw_os_mem_page_pa(odd, 1), 3);
	/* map_pa of an aligned 2M range just below a 1G boundary: one block */
	CHECK_EQ(pw_vm_map_pa(vm, 0x7fc00000, pa0, PW_SZ_2M, 0), PW_OK);
	check_va(&e, vm, 0x7fc00000, pa0, 2);

	pw_vm_destroy(vm);
	pw_os_mem_free(e.os, big);
	pw_os_mem_free(e.os, odd);
	env_fini(&e);
}

static void test_overlap_and_ranges(void)
{
	struct env e;
	struct pw_vm *vm, *mcu;
	struct pw_os_mem *mem;

	if (!env_init(&e, NULL))
		return;
	REQUIRE(pw_vm_create(e.dev, false, &vm) == PW_OK);
	REQUIRE(pw_vm_activate(vm) == PW_OK);
	REQUIRE(pw_os_mem_alloc(e.os, 8 * 4096, PW_MEM_KMAP, &mem) == PW_OK);

	CHECK_EQ(pw_vm_map_mem(vm, 0x10003000, mem, 0, 4096, 0), PW_OK);
	/* map 4 pages where the 4th is taken: fails and leaves nothing behind */
	CHECK_EQ(pw_vm_map_mem(vm, 0x10000000, mem, 4096, 4 * 4096, 0), PW_EBUSY);
	for (unsigned int i = 0; i < 3; i++)
		CHECK_EQ(mock_gpu_translate(e.os, 1, 0x10000000 + i * 4096, 'r', NULL, NULL, NULL),
			 0xc3);
	check_va(&e, vm, 0x10003000, pw_os_mem_page_pa(mem, 0), 3);	/* original untouched */

	/* a block over existing pages, and pages over an existing block */
	CHECK_EQ(pw_vm_map_pa(vm, 0x10000000, MOCK_PA_BASE, PW_SZ_2M, 0), PW_EBUSY);
	CHECK_EQ(pw_vm_map_pa(vm, 0x20000000, MOCK_PA_BASE, PW_SZ_2M, 0), PW_OK);
	CHECK_EQ(pw_vm_map_pa(vm, 0x20001000, MOCK_PA_BASE, 4096, 0), PW_EBUSY);

	CHECK_EQ(pw_vm_map_mem(vm, 0x10000800, mem, 0, 4096, 0), PW_EINVAL);
	CHECK_EQ(pw_vm_map_mem(vm, 0x10000000, mem, 0, 0, 0), PW_EINVAL);
	CHECK_EQ(pw_vm_map_mem(vm, 0x10000000, mem, 0, 9 * 4096, 0), PW_EINVAL);
	CHECK_EQ(pw_vm_map_mem(vm, 0x10000000, mem, 0x800, 4096, 0), PW_EINVAL);
	CHECK_EQ(pw_vm_map_mem(vm, (1ull << 48) - 4096, mem, 0, 2 * 4096, 0), PW_EINVAL);
	CHECK_EQ(pw_vm_map_mem(vm, (1ull << 48) - 4096, mem, 0, 4096, 0), PW_OK);
	check_va(&e, vm, (1ull << 48) - 4096, pw_os_mem_page_pa(mem, 0), 3);
	CHECK_EQ(pw_vm_map_pa(vm, 0x30000000, 1ull << 41, 4096, 0), PW_ERANGE);	/* > 40 PA bits */
	CHECK_EQ(pw_vm_map_pa(vm, 0x30000000, 0x123, 4096, 0), PW_EINVAL);
	CHECK_EQ(pw_vm_unmap(vm, 0x123, 4096), PW_EINVAL);

	/* MCU VM: 4 GiB */
	REQUIRE(pw_vm_create(e.dev, true, &mcu) == PW_OK);
	CHECK_EQ(mcu->va_size, PW_SZ_4G);
	CHECK_EQ(pw_vm_map_mem(mcu, PW_SZ_4G, mem, 0, 4096, 0), PW_EINVAL);
	CHECK_EQ(pw_vm_map_mem(mcu, PW_SZ_4G - 4096, mem, 0, 4096, 0), PW_OK);
	REQUIRE(pw_vm_activate(mcu) == PW_OK);
	CHECK_EQ(mcu->as, 0);
	pw_vm_destroy(mcu);

	pw_vm_destroy(vm);
	pw_os_mem_free(e.os, mem);
	env_fini(&e);
}

static void test_large_noncontig(void)
{
	struct env e;
	struct pw_vm *vm;
	struct pw_os_mem *mem;
	const unsigned int n = 1100;
	const uint64_t va = 0x3ff8000ull;	/* 8 pages before a 2M boundary... */

	if (!env_init(&e, NULL))
		return;
	REQUIRE(pw_vm_create(e.dev, false, &vm) == PW_OK);
	REQUIRE(pw_os_mem_alloc(e.os, (size_t)n * 4096, PW_MEM_KMAP, &mem) == PW_OK);
	CHECK_EQ(pw_vm_map_mem(vm, va, mem, 0, (uint64_t)n * 4096, PW_VM_MAP_UNCACHED), PW_OK);
	REQUIRE(pw_vm_activate(vm) == PW_OK);
	for (unsigned int i = 0; i < n; i++)
		check_va(&e, vm, va + (uint64_t)i * 4096, pw_os_mem_page_pa(mem, i), 3);
	CHECK_EQ(vm->pt_pages, 3 + 4);		/* ...so 1100 pages span 4 level-3 tables */
	pw_vm_destroy(vm);
	pw_os_mem_free(e.os, mem);
	env_fini(&e);
}

/* Updates on an active AS go through LOCK -> PTE writes -> GPU cache flush -> UNLOCK. */
static void test_active_update(void)
{
	struct env e;
	struct pw_vm *vm;
	struct pw_os_mem *mem;
	struct mock_as_state a0, a1;
	uint32_t f0;

	if (!env_init(&e, NULL))
		return;
	REQUIRE(pw_vm_create(e.dev, false, &vm) == PW_OK);
	REQUIRE(pw_vm_activate(vm) == PW_OK);
	REQUIRE(pw_os_mem_alloc(e.os, 4 * 4096, PW_MEM_KMAP, &mem) == PW_OK);
	mock_get_as(e.os, 1, &a0);
	f0 = mock_flush_count(e.os);
	CHECK_EQ(pw_vm_map_mem(vm, 0x123450000ull, mem, 0, 4 * 4096, 0), PW_OK);
	mock_get_as(e.os, 1, &a1);
	CHECK_EQ(a1.locks, a0.locks + 1);
	CHECK_EQ(a1.unlocks, a0.unlocks + 1);
	CHECK(!a1.locked);
	{
		uint64_t s = 0x123450000ull, sz = 4 * 4096;
		uint64_t v = pw_mmu_pack_region_range(&s, &sz);

		CHECK_EQ(a1.lockaddr, v);
	}
	CHECK_EQ(mock_flush_count(e.os), f0 + 1);
	/* L2 clean+inv, LSC clean+inv, other inv (mmu.c:1839) */
	CHECK_EQ(mock_last_flush_cmd(e.os), 0x23304);
	check_va(&e, vm, 0x123451000ull, pw_os_mem_page_pa(mem, 1), 3);

	CHECK_EQ(pw_vm_unmap(vm, 0x123450000ull, 4 * 4096), PW_OK);
	mock_get_as(e.os, 1, &a0);
	CHECK_EQ(a0.locks, a1.locks + 1);
	CHECK_EQ(a0.unlocks, a1.unlocks + 1);

	/* deactivate: flush + UNMAPPED; re-activate gets AS1 back */
	pw_vm_deactivate(vm);
	mock_get_as(e.os, 1, &a1);
	CHECK(!a1.enabled);
	CHECK_EQ(vm->as, -1);
	CHECK_EQ(pw_vm_activate(vm), PW_OK);
	CHECK_EQ(vm->as, 1);
	pw_vm_destroy(vm);
	pw_os_mem_free(e.os, mem);
	env_fini(&e);
}

/* Sanity check of the test infrastructure: an unflushed PTE write is invisible to the GPU. */
static void test_noncoherent_model(void)
{
	struct env e;
	struct pw_vm *vm;
	struct pw_os_mem *mem;
	struct pw_pt *l3;
	uint64_t pte;

	if (!env_init(&e, NULL))
		return;
	REQUIRE(pw_vm_create(e.dev, false, &vm) == PW_OK);
	REQUIRE(pw_vm_activate(vm) == PW_OK);
	REQUIRE(pw_os_mem_alloc(e.os, 2 * 4096, PW_MEM_KMAP, &mem) == PW_OK);
	CHECK_EQ(pw_vm_map_mem(vm, 0x5000000, mem, 0, 4096, 0), PW_OK);
	CHECK_EQ(pw_vm_translate(vm, 0x5000000, NULL, &pte, NULL), PW_OK);
	/* poke the neighbouring PTE directly, without pw_os_mem_flush() */
	l3 = vm->root->child[0]->child[0]->child[0x5000000 >> 21];
	REQUIRE(l3 != NULL);
	l3->ptes[1] = (pw_os_mem_page_pa(mem, 1) & PW_PTE_OA_MASK) | (pte & ~PW_PTE_OA_MASK);
	CHECK_EQ(pw_vm_translate(vm, 0x5001000, NULL, NULL, NULL), PW_OK);	/* CPU sees it */
	CHECK_EQ(mock_gpu_translate(e.os, 1, 0x5001000, 'r', NULL, NULL, NULL), 0xc3);	/* GPU not */
	pw_os_mem_flush(e.os, l3->mem, 8, 8);
	CHECK_EQ(mock_gpu_translate(e.os, 1, 0x5001000, 'r', NULL, NULL, NULL), 0);
	l3->ptes[1] = 0;
	pw_os_mem_flush(e.os, l3->mem, 8, 8);
	pw_vm_destroy(vm);
	pw_os_mem_free(e.os, mem);
	env_fini(&e);
}

static void test_as_exhaustion(void)
{
	struct env e;
	struct pw_vm *vms[8];

	if (!env_init(&e, NULL))
		return;
	for (unsigned int i = 0; i < 8; i++) {
		REQUIRE(pw_vm_create(e.dev, false, &vms[i]) == PW_OK);
		if (i < 7) {
			CHECK_EQ(pw_vm_activate(vms[i]), PW_OK);
			CHECK_EQ(vms[i]->as, (int)i + 1);
		} else {
			CHECK_EQ(pw_vm_activate(vms[i]), PW_EBUSY);	/* AS1..7 taken, no LRU yet */
		}
	}
	pw_vm_destroy(vms[3]);
	CHECK_EQ(pw_vm_activate(vms[7]), PW_OK);
	CHECK_EQ(vms[7]->as, 4);
	for (unsigned int i = 0; i < 8; i++)
		if (i != 3)
			pw_vm_destroy(vms[i]);
	env_fini(&e);
}

static void test_pa_out_of_range(void)
{
	struct mock_config cfg = { .pa_base = 1ull << 40 };	/* all DRAM beyond 40 PA bits */
	struct env e;
	struct pw_vm *vm = NULL;

	if (!env_init(&e, &cfg))
		return;
	CHECK(pw_vm_create(e.dev, false, &vm) != PW_OK);
	CHECK(mock_log_contains(e.os, "unusable by the GPU"));
	env_fini(&e);
}

/*
 * Review regression: empty level-3 tables are kept after unmap (freeing them must wait for the
 * TLB flush), so a later 2M-eligible map over the same 2 MiB slot finds a table descriptor there.
 * That must not be mistaken for an overlap (it used to return PW_EBUSY).
 */
static void test_block_over_empty_table(void)
{
	struct env e;
	struct pw_vm *vm;
	struct pw_os_mem *big;
	uint64_t pa0;

	if (!env_init(&e, NULL))
		return;
	REQUIRE(pw_vm_create(e.dev, false, &vm) == PW_OK);
	REQUIRE(pw_vm_activate(vm) == PW_OK);
	REQUIRE(pw_os_mem_alloc(e.os, 2u << 20, PW_MEM_CONTIGUOUS, &big) == PW_OK);
	pa0 = pw_os_mem_page_pa(big, 0);
	REQUIRE((pa0 & (PW_SZ_2M - 1)) == 0);

	CHECK_EQ(pw_vm_map_pa(vm, 0x40005000, pa0, 0x1000, 0), PW_OK);
	CHECK_EQ(pw_vm_unmap(vm, 0x40005000, 0x1000), PW_OK);
	CHECK_EQ(pw_vm_map_mem(vm, 0x40000000, big, 0, PW_SZ_2M, 0), PW_OK);
	/* falls back to 4K pages in the existing (empty) level-3 table */
	check_va(&e, vm, 0x40000000, pa0, 3);
	check_va(&e, vm, 0x401ff000 + 0x10, pa0 + 0x1ff010, 3);
	/* a real overlap inside that table is still refused, with rollback */
	CHECK_EQ(pw_vm_unmap(vm, 0x40000000, PW_SZ_2M), PW_OK);
	CHECK_EQ(pw_vm_map_pa(vm, 0x40100000, pa0, 0x1000, 0), PW_OK);
	CHECK_EQ(pw_vm_map_mem(vm, 0x40000000, big, 0, PW_SZ_2M, 0), PW_EBUSY);
	CHECK_EQ(mock_gpu_translate(e.os, 1, 0x40000000, 'r', NULL, NULL, NULL), 0xc3);
	check_va(&e, vm, 0x40100000, pa0, 3);

	pw_vm_destroy(vm);
	pw_os_mem_free(e.os, big);
	env_fini(&e);
}

const struct test_case mmu_tests[] = {
	{ "mmu.vm_basic_4k", test_vm_basic },
	{ "mmu.attributes", test_attrs },
	{ "mmu.blocks_2m_and_split", test_blocks },
	{ "mmu.overlap_rollback_ranges", test_overlap_and_ranges },
	{ "mmu.large_noncontiguous", test_large_noncontig },
	{ "mmu.active_as_lock_flush_unlock", test_active_update },
	{ "mmu.noncoherent_model_sanity", test_noncoherent_model },
	{ "mmu.as_exhaustion", test_as_exhaustion },
	{ "mmu.pa_out_of_range", test_pa_out_of_range },
	{ "mmu.block_over_empty_table", test_block_over_empty_table },
	{ NULL, NULL },
};
