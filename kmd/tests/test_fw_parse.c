// SPDX-License-Identifier: MIT
/*
 * test_fw_parse.c - firmware image parser against the real arch10.8 mali_csffw.bin, crafted
 * malformed images, truncations and random mutations (run under ASan/UBSan), plus mutated
 * images pushed through the full loader (allocation + mapping + error-path cleanup).
 */
#include <stdlib.h>
#include <string.h>

#include "mock_gpu.h"
#include "pw_fw.h"
#include "test.h"

#define MAX_SEEN 64

struct seen {
	unsigned int n;
	struct pw_fw_iface_entry e[MAX_SEEN];
	const uint8_t *image;
	size_t size;
	unsigned int bad;	/* invariant violations reported to the callback */
};

static int record_iface(void *ctx, const struct pw_fw_iface_entry *e, const uint8_t *image)
{
	struct seen *s = ctx;

	/* Invariants the loader relies on. */
	if (e->va_end < e->va_start || (e->va_start & 0xfff) || (e->va_end & 0xfff) ||
	    e->data_end < e->data_start || e->data_end > s->size ||
	    e->data_end - e->data_start > e->va_end - e->va_start ||
	    (e->flags & ~CSF_FW_BINARY_IFACE_ENTRY_SUPPORTED_FLAGS) ||
	    (e->flags & CSF_FW_BINARY_IFACE_ENTRY_PROT) || e->va_end == e->va_start ||
	    e->va_end - e->va_start > PW_FW_MAX_SECTION_SIZE ||
	    e->name < image || e->name + e->name_len > image + s->size)
		s->bad++;
	/* touch all bytes the loader would copy, so ASan sees any overrun */
	{
		volatile uint8_t sum = 0;

		for (uint32_t i = e->data_start; i < e->data_end; i += 64)
			sum += image[i];
		for (uint32_t i = 0; i < e->name_len; i++)
			sum += e->name[i];
		(void)sum;
	}
	if (s->n < MAX_SEEN)
		s->e[s->n++] = *e;
	return 0;
}

static const struct pw_fw_parse_ops record_ops = { .iface = record_iface };

static uint8_t *g_img;
static size_t g_size;

static bool load_fw(void)
{
	if (!g_fw_path) {
		test_skip("real firmware not available");
		return false;
	}
	if (!g_img)
		g_img = read_file(g_fw_path, &g_size);
	return g_img != NULL;
}

static int parse(struct pw_os *os, const uint8_t *img, size_t size, struct seen *s,
		 struct pw_fw_image_info *info)
{
	memset(s, 0, sizeof(*s));
	s->image = img;
	s->size = size;
	return pw_fw_parse(os, img, size, &record_ops, s, info);
}

static const struct pw_fw_iface_entry *find_va(const struct seen *s, uint32_t va)
{
	for (unsigned int i = 0; i < s->n; i++)
		if (s->e[i].va_start == va)
			return &s->e[i];
	return NULL;
}

static void test_real_firmware(void)
{
	struct pw_os *os = mock_create(NULL);
	struct pw_fw_image_info info;
	struct seen s;
	/* kernel-panthor.md 3.1, and a python dump of the same file */
	static const struct { uint32_t va, end, flags; } want[] = {
		{ 0x00000000, 0x00010000, 0x00000009 },
		{ 0x00400000, 0x00410000, 0x00000009 },
		{ 0x00410000, 0x00420000, 0x00000009 },
		{ 0x00800000, 0x00820000, 0x0000000d },	/* code: RD|EX|CACHED */
		{ 0x01000000, 0x01040000, 0x80000009 },
		{ 0x02000000, 0x02040000, 0x8000000b },
		{ 0x04000000, 0x04010000, 0xc000001b },	/* shared: SHARED|ZERO|CACHED_COH|RW */
	};

	if (!load_fw()) {
		mock_destroy(os);
		return;
	}
	CHECK_EQ(g_size, 282624);
	CHECK_EQ(parse(os, g_img, g_size, &s, &info), PW_OK);
	CHECK_EQ(info.major, 0);
	CHECK_EQ(info.minor, 3);
	CHECK_EQ(info.version_hash, 0x1050000);
	CHECK_EQ(info.hdr_size, 792);
	CHECK_EQ(info.n_iface, 8);		/* 8 IFACE entries ... */
	CHECK_EQ(info.n_iface_prot, 1);		/* ... the 0x3000000 one is protected-mode */
	CHECK_EQ(info.n_iface_loaded, 7);
	CHECK_EQ(info.n_config, 2);		/* "Log verbosity", "WA_CFG0" */
	CHECK_EQ(info.n_trace, 8);
	CHECK(info.has_shared);
	CHECK(info.git_sha[0] != 0);
	CHECK_EQ(s.bad, 0);
	CHECK_EQ(s.n, 7);
	for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		const struct pw_fw_iface_entry *e = find_va(&s, want[i].va);

		CHECK(e != NULL);
		if (!e)
			continue;
		CHECK_EQ(e->va_end, want[i].end);
		CHECK_EQ(e->flags, want[i].flags);
	}
	CHECK(find_va(&s, 0x3000000) == NULL);
	CHECK_EQ(find_va(&s, 0x800000)->data_start, 0x26c4);
	CHECK_EQ(find_va(&s, 0x800000)->data_end, 0x16ac8);
	fprintf(stderr, "    fw git sha: %s\n", info.git_sha);
	mock_destroy(os);
}

static void test_truncated(void)
{
	struct pw_os *os = mock_create(NULL);
	struct pw_fw_image_info info;
	struct seen s;
	unsigned int ok = 0;

	if (!load_fw()) {
		mock_destroy(os);
		return;
	}
	/* Every prefix up to past the header must fail, and so must any shorter-than-data
	 * length (the shared section's data ends at the end of the file). */
	for (size_t len = 0; len < 2048; len++) {
		uint8_t *copy = malloc(len ? len : 1);

		memcpy(copy, g_img, len);
		if (parse(os, copy, len, &s, &info) == PW_OK)
			ok++;
		CHECK_EQ(s.bad, 0);
		free(copy);
	}
	for (size_t len = 2048; len < g_size; len += 4093) {
		uint8_t *copy = malloc(len);

		memcpy(copy, g_img, len);
		if (parse(os, copy, len, &s, &info) == PW_OK)
			ok++;
		free(copy);
	}
	{
		uint8_t *copy = malloc(g_size - 1);

		memcpy(copy, g_img, g_size - 1);
		CHECK(parse(os, copy, g_size - 1, &s, &info) != PW_OK);
		free(copy);
	}
	CHECK_EQ(ok, 0);
	CHECK(pw_fw_parse(os, NULL, 0, NULL, NULL, &info) != PW_OK);
	mock_destroy(os);
}

/* Helpers to patch the real image: find the n-th entry of a type. */
static size_t entry_offset(const uint8_t *img, uint32_t type, unsigned int nth)
{
	uint32_t hsize;
	size_t off = 20;

	memcpy(&hsize, img + 16, 4);
	while (off < hsize) {
		uint32_t ehdr;

		memcpy(&ehdr, img + off, 4);
		if ((ehdr & 0xff) == type && !nth--)
			return off;
		off += (ehdr >> 8) & 0xff;
	}
	return 0;
}

static size_t iface_with_va(const uint8_t *img, uint32_t va)
{
	for (unsigned int i = 0; i < 16; i++) {
		size_t off = entry_offset(img, 0, i);
		uint32_t v;

		if (!off)
			break;
		memcpy(&v, img + off + 8, 4);
		if (v == va)
			return off;
	}
	return 0;
}

static void put32(uint8_t *p, uint32_t v)
{
	memcpy(p, &v, 4);
}

static uint32_t get32(const uint8_t *p)
{
	uint32_t v;

	memcpy(&v, p, 4);
	return v;
}

static void test_crafted(void)
{
	struct pw_os *os = mock_create(NULL);
	struct pw_fw_image_info info;
	struct seen s;
	uint8_t *c;
	size_t code, shared;

	if (!load_fw()) {
		mock_destroy(os);
		return;
	}
	c = malloc(g_size);
	code = iface_with_va(g_img, 0x800000);
	shared = iface_with_va(g_img, 0x4000000);
	REQUIRE(code && shared);

#define MUTATE(desc, stmt, expect_ok) do { \
		memcpy(c, g_img, g_size); \
		stmt; \
		int _r = parse(os, c, g_size, &s, &info); \
		if ((_r == PW_OK) != (expect_ok)) { \
			fprintf(stderr, "    case '%s': got %d\n", desc, _r); \
			CHECK(!"unexpected parse result"); \
		} \
		CHECK_EQ(s.bad, 0); \
	} while (0)

	MUTATE("baseline", (void)0, true);
	MUTATE("bad magic", c[0] ^= 1, false);
	MUTATE("major 1", c[5] = 1, false);
	MUTATE("hdr.size > file", put32(c + 16, (uint32_t)g_size + 1), false);
	MUTATE("hdr.size = 0 (no entries -> no shared)", put32(c + 16, 0), false);
	MUTATE("hdr.size = 3", put32(c + 16, 3), false);
	MUTATE("entry size 0", c[code + 1] = 0, false);
	MUTATE("entry size 2", c[code + 1] = 2, false);
	MUTATE("entry size not multiple of 4", c[code + 1] = 0x21, false);
	MUTATE("entry size runs past header", c[code + 1] = 0xfc, false);
	MUTATE("iface entry too short for hdr", c[code + 1] = 8, false);
	MUTATE("data.end < data.start", put32(c + code + 20, get32(c + code + 16) - 4), false);
	MUTATE("data.end past file", put32(c + code + 20, (uint32_t)g_size + 4), false);
	MUTATE("data.start huge", put32(c + code + 16, 0xfffffff0u), false);
	MUTATE("va.end < va.start", put32(c + code + 12, 0x7ff000), false);
	MUTATE("va.start unaligned", put32(c + code + 8, 0x800010), false);
	MUTATE("va.end unaligned", put32(c + code + 12, 0x820004), false);
	MUTATE("unsupported flag bit 6", put32(c + code + 4, get32(c + code + 4) | 0x40), false);
	MUTATE("shared without SHARED", put32(c + shared + 4, get32(c + shared + 4) & ~(1u << 30)),
	       false);
	MUTATE("data > section", put32(c + code + 12, 0x801000), false);
	MUTATE("shared moved away", (put32(c + shared + 8, 0x5000000), put32(c + shared + 12, 0x5010000)),
	       false);
	MUTATE("shared too small", put32(c + shared + 12, 0x4001000), false);
	MUTATE("section too large", put32(c + code + 12, 0xfffff000u), false);
	MUTATE("empty section is skipped", put32(c + code + 12, 0x800000) /* va.end == start ... */
	       , false /* ... but data > 0 then exceeds the 0-sized section */);
	MUTATE("empty section, no data", (put32(c + code + 12, 0x800000), put32(c + code + 20, get32(c + code + 16))),
	       true);
	MUTATE("protected shared section is skipped -> no shared",
	       put32(c + shared + 4, get32(c + shared + 4) | (1u << 5)), false);
	/* Unknown entry types: optional ones are skipped, mandatory ones rejected. */
	{
		size_t cfg = entry_offset(g_img, 1, 0);

		MUTATE("unknown optional type", (c[cfg] = 0x55, c[cfg + 3] |= 0x80), true);
		MUTATE("unknown mandatory type", (c[cfg] = 0x55, c[cfg + 3] &= 0x7f), false);
	}
	/* Corrupt build info: not fatal. */
	{
		size_t bi = entry_offset(g_img, 6, 0);

		REQUIRE(bi);
		MUTATE("build info meta_start huge", put32(c + bi + 4, 0xffffff00u), true);
		CHECK_EQ(info.git_sha[0], 0);
		MUTATE("build info meta_size huge", put32(c + bi + 8, 0xffffff00u), true);
		MUTATE("build info not NUL terminated",
		       c[get32(g_img + bi + 4) + get32(g_img + bi + 8) - 1] = 'x', true);
		CHECK_EQ(info.git_sha[0], 0);
	}
	/* A duplicate shared section (turn the 0x2000000 entry into a second shared one). */
	{
		size_t z = iface_with_va(g_img, 0x2000000);

		MUTATE("duplicate shared", (put32(c + z + 4, get32(c + shared + 4)),
					    put32(c + z + 8, 0x4000000), put32(c + z + 12, 0x4010000),
					    put32(c + z + 20, get32(c + z + 16))), false);
	}
#undef MUTATE
	free(c);

	/* Too many entries: header + 300 minimal optional entries. */
	{
		size_t sz = 20 + 300 * 4;
		uint8_t *img = calloc(1, sz);

		put32(img, CSF_FW_BINARY_HEADER_MAGIC);
		put32(img + 16, (uint32_t)sz);
		for (unsigned int i = 0; i < 300; i++)
			put32(img + 20 + i * 4, 0x80000400u | 0x7);	/* size 4, type 7, optional */
		CHECK(parse(os, img, sz, &s, &info) != PW_OK);
		free(img);
	}
	mock_destroy(os);
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

static uint32_t rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (uint32_t)(rng_state >> 16);
}

static void mutate(uint8_t *c, size_t size, uint32_t hdr_size)
{
	static const uint32_t interesting[] = {
		0, 1, 4, 0x1000, 0xfff, 0x7fffffff, 0x80000000u, 0xffffffffu, 0xfffffffcu,
		0x4000000, 0x4010000, 0x800000, 282624, 282628, 792, 0xc000001b, 0x40000000,
	};
	unsigned int n = 1 + rnd() % 8;

	for (unsigned int i = 0; i < n; i++) {
		size_t off = rnd() % (hdr_size + 16 < size ? hdr_size + 16 : size);

		switch (rnd() % 4) {
		case 0:
			c[off] ^= (uint8_t)(1u << (rnd() % 8));
			break;
		case 1:
			c[off] = (uint8_t)rnd();
			break;
		default:
			off &= ~(size_t)3;
			if (off + 4 <= size)
				put32(c + off, interesting[rnd() % (sizeof(interesting) /
								     sizeof(interesting[0]))]);
		}
	}
}

static void test_fuzz_parser(void)
{
	struct pw_os *os = mock_create(NULL);
	struct pw_fw_image_info info;
	struct seen s;
	unsigned int ok = 0, iters = 20000;
	uint8_t *c;

	if (!load_fw()) {
		mock_destroy(os);
		return;
	}
	c = malloc(g_size);
	for (unsigned int it = 0; it < iters; it++) {
		size_t len = g_size;

		memcpy(c, g_img, g_size);
		mutate(c, g_size, 792);
		/* sometimes also truncate */
		if (rnd() % 8 == 0)
			len = rnd() % g_size;
		if (parse(os, c, len, &s, &info) == PW_OK)
			ok++;
		CHECK_EQ(s.bad, 0);
		if (s.bad)
			break;
	}
	fprintf(stderr, "    %u/%u mutated images accepted\n", ok, iters);
	CHECK(ok > 0);		/* the fuzzer isn't trivially rejecting everything */
	CHECK(ok < iters);
	free(c);
	mock_destroy(os);
}

/* Mutated images through the real loader: allocation, mapping, MCU boot, error cleanup. */
static void test_fuzz_loader(void)
{
	unsigned int ok = 0, iters = 150;
	uint8_t *c;

	if (!load_fw())
		return;
	c = malloc(g_size);
	for (unsigned int it = 0; it < iters; it++) {
		struct mock_config cfg = { 0 };
		struct pw_device_params p = { 0 };
		struct pw_device *dev = NULL;
		struct pw_os *os;
		int ret;

		memcpy(c, g_img, g_size);
		if (it)
			mutate(c, g_size, 792);
		cfg.fw_data = c;
		cfg.fw_size = g_size;
		cfg.mcu_lenient = true;
		os = mock_create(&cfg);
		p.os = os;
		p.boot_firmware = true;
		ret = pw_device_create(&p, &dev);
		if (it == 0)
			CHECK_EQ(ret, PW_OK);
		if (ret == PW_OK) {
			ok++;
			pw_device_destroy(dev);
		}
		check_no_leaks(os);
		mock_destroy(os);
	}
	fprintf(stderr, "    %u/%u mutated images booted\n", ok, iters);
	free(c);
}

static void free_img(void)
{
	free(g_img);
	g_img = NULL;
}

static void test_cleanup(void)
{
	free_img();
}

const struct test_case fw_parse_tests[] = {
	{ "fw.parse_real_firmware", test_real_firmware },
	{ "fw.parse_truncated", test_truncated },
	{ "fw.parse_crafted_invalid", test_crafted },
	{ "fw.parse_fuzz", test_fuzz_parser },
	{ "fw.loader_fuzz", test_fuzz_loader },
	{ "fw.cleanup", test_cleanup },
	{ NULL, NULL },
};
