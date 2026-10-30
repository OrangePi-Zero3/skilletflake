// SPDX-License-Identifier: MIT
/* test_util.c - freestanding helpers, region packing, MEMATTR, timer conversion. */
#include <stdio.h>
#include <string.h>

#include "pw_mmu.h"
#include "test.h"

static void test_snprintf(void)
{
	char a[128], b[128];

#define SAME(...) do { \
		int ra = pw_snprintf(a, sizeof(a), __VA_ARGS__); \
		int rb = snprintf(b, sizeof(b), __VA_ARGS__); \
		if (strcmp(a, b) || ra != rb) { \
			fprintf(stderr, "    got '%s' (%d), want '%s' (%d)\n", a, ra, b, rb); \
			CHECK(!"snprintf mismatch"); \
		} \
	} while (0)
	SAME("plain");
	SAME("%d %i %u", -42, 17, 4000000000u);
	SAME("%x %X %08x %x", 0xabcu, 0xabcu, 0x12u, 0u);
	SAME("%016llx %llu %lld", 0x123456789abcull, 18446744073709551615ull, -9000000000ll);
	SAME("%zu %lu %hhu %hd", (size_t)12345, 99ul, (unsigned char)250, (short)-5);
	SAME("[%s] [%5s] [%-5s] [%.2s] %c %%", "abc", "ab", "ab", "abcdef", 'Z');
	SAME("%5d|%-5d|%05d", 42, 42, -42);
#undef SAME
	/* Unsupported flags ('#') must not crash. */
	{
		const char *volatile bad = "%#x %q %";

		pw_snprintf(a, sizeof(a), bad, 1u);
	}

	/* truncation: returns full length, always terminated */
	CHECK_EQ(pw_snprintf(a, 5, "hello world"), 11);
	CHECK(!strcmp(a, "hell"));
	CHECK_EQ(pw_snprintf(a, 1, "x%dy", 5), 3);
	CHECK_EQ(a[0], 0);
	CHECK_EQ(pw_snprintf(NULL, 0, "abc"), 3);
	CHECK_EQ(pw_strlcpy(a, "abcdef", 4), 6);
	CHECK(!strcmp(a, "abc"));
}

static void test_bits(void)
{
	CHECK_EQ(pw_fls64(0), 0);
	CHECK_EQ(pw_fls64(1), 1);
	CHECK_EQ(pw_fls64(0x8000000000000000ull), 64);
	CHECK_EQ(pw_ffs32(0), 0);
	CHECK_EQ(pw_ffs32(0x10), 5);
	CHECK_EQ(pw_hweight64(0x50005), 4);
	CHECK_EQ(PW_GENMASK(10, 0), 0x7ff);
	CHECK_EQ(PW_GENMASK(26, 20), 0x07f00000);
	CHECK_EQ(PW_GENMASK64(47, 12), 0x0000fffffffff000ull);
}

/* Brute force: smallest naturally aligned power of two >= 32K containing [start, start+size). */
static void ref_region(uint64_t start, uint64_t size, uint64_t *rs, uint64_t *rsz)
{
	for (unsigned int w = 15; w < 64; w++) {
		uint64_t bs = 1ull << w, base = start & ~(bs - 1);

		if (base + bs >= start + size || w == 63) {
			*rs = base;
			*rsz = bs;
			return;
		}
	}
}

static void test_pack_region(void)
{
	static const uint64_t cases[][2] = {
		{ 0x1000, 0x1000 }, { 0, 0x8000 }, { 0x8000, 0x8000 }, { 0x7000, 0x2000 },
		{ 0x4000000, 0x10000 }, { 0x123456000, 0x5000 }, { 0x1ff000, 0x2000 },
		{ 0xffff00000000ull, 0x100000000ull }, { 0x40000000, 0x400000 },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint64_t s = cases[i][0], sz = cases[i][1], rs, rsz, v;

		ref_region(s, sz, &rs, &rsz);
		v = pw_mmu_pack_region_range(&s, &sz);
		CHECK_EQ(s, rs);
		CHECK_EQ(sz, rsz);
		/* encoded as log2(size) - 1 in the low bits */
		CHECK_EQ(v & 0x3f, pw_fls64(rsz) - 2);
		CHECK_EQ(v & ~0x3full, rs);
	}
	/* mmu.c:570 example: 4K at 4K -> 32K region at 0, width 14 */
	{
		uint64_t s = 0x1000, sz = 0x1000;

		CHECK_EQ(pw_mmu_pack_region_range(&s, &sz), 14);
	}
}

static void test_memattr(void)
{
	/* idx0 NC -> 0x4c, idx1 WB RWA -> 0x8f, idx2 Device -> 0x4c, idx3 iNC/oWB -> 0x8c
	 * (panthor's check only looks at inner bit 2), idx4-7 (0x00) -> 0x4c. */
	CHECK_EQ(pw_mmu_mair_to_memattr(PW_MAIR_VALUE, false), 0x4c4c4c4c8c4c8f4cull);
	/* coherent: WB entries get SH_CPU_INNER (1 << 4) */
	CHECK_EQ(pw_mmu_mair_to_memattr(PW_MAIR_VALUE, true), 0x4c4c4c4c9c4c9f4cull);
}

static void test_pte_attrs(void)
{
	uint64_t a;

	a = pw_mmu_pte_attrs(0, false);
	CHECK_EQ(a, PW_PTE_AF | PW_PTE_NG | PW_PTE_AP_UNPRIV | PW_PTE_ATTRINDX(1) | PW_PTE_SH_IS);
	a = pw_mmu_pte_attrs(PW_VM_MAP_READONLY | PW_VM_MAP_NOEXEC | PW_VM_MAP_UNCACHED, false);
	CHECK_EQ(a, PW_PTE_AF | PW_PTE_NG | PW_PTE_AP_UNPRIV | PW_PTE_AP_RDONLY | PW_PTE_XN |
		    PW_PTE_ATTRINDX(0) | PW_PTE_SH_OS);
	CHECK_EQ(pw_mmu_pte_attrs(PW_VM_MAP_DEVICE, false) & PW_PTE_ATTRINDX_MASK,
		 PW_PTE_ATTRINDX(2));
	/* same values as the uAPI flags */
	CHECK_EQ(PW_VM_MAP_READONLY, DRM_PANTHOR_VM_BIND_OP_MAP_READONLY);
	CHECK_EQ(PW_VM_MAP_NOEXEC, DRM_PANTHOR_VM_BIND_OP_MAP_NOEXEC);
	CHECK_EQ(PW_VM_MAP_UNCACHED, DRM_PANTHOR_VM_BIND_OP_MAP_UNCACHED);
}

static void test_conv_timeout(void)
{
	/* 24 MHz: ceil(us * 24e6 / (1e6 << 10)) */
	CHECK_EQ(pw_fw_conv_timeout(24000000, 10000), 235);
	CHECK_EQ(pw_fw_conv_timeout(24000000, 800), 19);
	CHECK_EQ(pw_fw_conv_timeout(0, 800), 0xffffffffu);
	CHECK_EQ(pw_fw_conv_timeout(24000000, 0xffffffffu), 0x6000000);
	CHECK_EQ(pw_fw_conv_timeout(0xffffffffu, 0xffffffffu), 0x7fffffffu);
}

static void test_reqs_helpers(void)
{
	/* toggle: bits in mask become ~ack, others keep req */
	CHECK_EQ(pw_fw_toggle_val(0x0000000f, 0x00000100, 0x00000300), 0x0000020f);
	CHECK_EQ(pw_fw_update_val(0xffff0000, 0x00001234, 0x0000ff00), 0xffff1200);
	CHECK_EQ(GPU_FLUSH_CACHES(CACHE_CLEAN | CACHE_INV, CACHE_CLEAN | CACHE_INV, CACHE_INV),
		 0x23304);
	CHECK_EQ(GPU_SOFT_RESET, 0x101);
}

const struct test_case util_tests[] = {
	{ "util.snprintf", test_snprintf },
	{ "util.bits", test_bits },
	{ "util.pack_region_range", test_pack_region },
	{ "util.mair_to_memattr", test_memattr },
	{ "util.pte_attrs", test_pte_attrs },
	{ "util.conv_timeout", test_conv_timeout },
	{ "util.req_helpers", test_reqs_helpers },
	{ NULL, NULL },
};
