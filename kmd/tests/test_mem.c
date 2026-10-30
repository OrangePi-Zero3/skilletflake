// SPDX-License-Identifier: MIT
/*
 * test_mem.c - the per-file layer, BOs and user VMs, driven like userspace would: pw_file_open +
 * pw_file_ioctl with real Linux uAPI structs against the mock G610 (firmware booted). GPU-side
 * results are checked with the mock's independent page-table walker (mock_gpu_translate).
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mock_gpu.h"
#include "pw_bo.h"
#include "pw_sync.h"
#include "test.h"

struct menv {
	struct pw_os *os;
	struct pw_device *dev;
	struct pw_file *file;
};

static bool menv_init(struct menv *e)
{
	struct mock_config cfg = { 0 };
	struct pw_device_params p = { 0 };

	memset(e, 0, sizeof(*e));
	if (!g_fw_path) {
		test_skip("real firmware not available");
		return false;
	}
	cfg.fw_path = g_fw_path;
	e->os = mock_create(&cfg);
	p.os = e->os;
	p.boot_firmware = true;
	if (pw_device_alloc(&p, &e->dev)) {
		CHECK(!"device alloc failed");
		return false;
	}
	mock_attach_irq(e->os, e->dev);
	if (pw_device_probe(e->dev) || pw_file_open(e->dev, &e->file)) {
		CHECK(!"bring-up / file open failed");
		return false;
	}
	return true;
}

static void menv_close_file(struct menv *e)
{
	pw_file_close(e->file);
	e->file = NULL;
}

static void menv_fini(struct menv *e)
{
	if (e->file)
		menv_close_file(e);
	mock_detach_irq(e->os);
	pw_device_destroy(e->dev);
	check_no_leaks(e->os);
	CHECK_EQ(mock_model_errors(e->os), 0);
	CHECK_EQ(mock_irq_unclaimed(e->os), 0);
	mock_destroy(e->os);
}

/* ioctl(fd, req, arg) equivalent: size from the Linux ioctl number, like the OS layer. */
#define IOC(e, req, arg) pw_file_ioctl((e)->file, _IOC_NR(req), (arg), _IOC_SIZE(req))

static uint32_t bo_new(struct menv *e, uint64_t size, uint32_t flags)
{
	struct drm_panthor_bo_create c = { .size = size, .flags = flags };

	CHECK_EQ(IOC(e, DRM_IOCTL_PANTHOR_BO_CREATE, &c), PW_OK);
	return c.handle;
}

static uint32_t vm_new(struct menv *e, uint64_t user_va_range)
{
	struct drm_panthor_vm_create c = { .user_va_range = user_va_range };

	CHECK_EQ(IOC(e, DRM_IOCTL_PANTHOR_VM_CREATE, &c), PW_OK);
	return c.id;
}

static int bind1(struct menv *e, uint32_t vm, uint32_t flags, uint32_t bo, uint64_t bo_offset,
		 uint64_t va, uint64_t size)
{
	struct drm_panthor_vm_bind_op op = {
		.flags = flags, .bo_handle = bo, .bo_offset = bo_offset, .va = va, .size = size,
	};
	struct drm_panthor_vm_bind b = {
		.vm_id = vm, .ops = DRM_PANTHOR_OBJ_ARRAY(1, &op),
	};

	return IOC(e, DRM_IOCTL_PANTHOR_VM_BIND, &b);
}

#define MAP	DRM_PANTHOR_VM_BIND_OP_TYPE_MAP
#define UNMAP	DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP

/* Activate VM @id and return its AS. */
static int vm_as(struct menv *e, uint32_t id, struct pw_uvm **out)
{
	struct pw_uvm *uvm = pw_file_vm_get(e->file, id);
	int as;

	if (!uvm)
		return -1;
	if (pw_uvm_activate(uvm)) {
		pw_uvm_put(uvm);
		return -1;
	}
	as = pw_vm_as(pw_uvm_vm(uvm));
	if (out)
		*out = uvm;
	else
		pw_uvm_put(uvm);
	return as;
}

static uint64_t bo_pa(struct menv *e, uint32_t handle, uint64_t offset)
{
	struct pw_bo *bo = pw_file_bo_get(e->file, handle);
	uint64_t pa;

	if (!bo)
		return 0;
	pa = pw_os_mem_page_pa(bo->mem, (size_t)(offset >> 12)) + (offset & 0xfff);
	pw_bo_put(bo);
	return pa;
}

static uint32_t bo_refs(struct pw_bo *bo)
{
	return __atomic_load_n(&bo->refcount, __ATOMIC_ACQUIRE);
}

/* ------------------------------------------------------------------------------------------ */

static void test_mem_version_caps(void)
{
	struct menv e;
	struct drm_version v;
	struct drm_get_cap cap;
	char name[16], date[8], desc[64];
	uint32_t major, minor, patch;

	if (!menv_init(&e))
		return;
	pw_core_drm_version(&major, &minor, &patch);
	CHECK_EQ(major, 1);
	CHECK_EQ(minor, 6);
	CHECK_EQ(patch, 0);

	/* pass 1: lengths only */
	memset(&v, 0, sizeof(v));
	CHECK_EQ(IOC(&e, DRM_IOCTL_VERSION, &v), PW_OK);
	CHECK_EQ(v.version_major, 1);
	CHECK_EQ(v.version_minor, 6);
	CHECK_EQ(v.name_len, 7);
	CHECK_EQ(v.date_len, 1);
	CHECK(v.desc_len > 0 && v.desc_len < sizeof(desc));
	/* pass 2: strings (not NUL-terminated by the kernel, like DRM) */
	memset(name, 'x', sizeof(name));
	memset(date, 'x', sizeof(date));
	memset(desc, 0, sizeof(desc));
	v.name = name;
	v.name_len = sizeof(name);
	v.date = date;
	v.date_len = sizeof(date);
	v.desc = desc;
	v.desc_len = sizeof(desc);
	CHECK_EQ(IOC(&e, DRM_IOCTL_VERSION, &v), PW_OK);
	CHECK(!memcmp(name, "panthorx", 8));
	CHECK_EQ(v.name_len, 7);
	CHECK(!memcmp(date, "0x", 2));
	CHECK(!strncmp(desc, "Panthor DRM driver", 18));
	/* short buffer: truncated copy, full length reported */
	memset(name, 'x', sizeof(name));
	v.name_len = 3;
	CHECK_EQ(IOC(&e, DRM_IOCTL_VERSION, &v), PW_OK);
	CHECK(!memcmp(name, "panx", 4));
	CHECK_EQ(v.name_len, 7);
	/* bad user pointer */
	v.name = (char *)NULL;
	v.name_len = 16;
	v.date = (char *)NULL;
	v.desc = (char *)NULL;
	CHECK_EQ(IOC(&e, DRM_IOCTL_VERSION, &v), PW_OK);	/* NULL buffer = length query */

	/* GET_CAP */
	memset(&cap, 0, sizeof(cap));
	cap.capability = DRM_CAP_SYNCOBJ;
	CHECK_EQ(IOC(&e, DRM_IOCTL_GET_CAP, &cap), PW_OK);
	CHECK_EQ(cap.value, 1);
	cap.capability = DRM_CAP_SYNCOBJ_TIMELINE;
	CHECK_EQ(IOC(&e, DRM_IOCTL_GET_CAP, &cap), PW_OK);
	CHECK_EQ(cap.value, 1);
	cap.capability = DRM_CAP_PRIME;
	cap.value = 77;
	CHECK_EQ(IOC(&e, DRM_IOCTL_GET_CAP, &cap), PW_OK);
	CHECK_EQ(cap.value, 0);
	cap.capability = DRM_CAP_DUMB_BUFFER;
	CHECK_EQ(IOC(&e, DRM_IOCTL_GET_CAP, &cap), PW_EOPNOTSUPP);

	/* unsupported / unknown / version-gated ioctls */
	{
		struct drm_prime_handle ph = { 0 };
		struct drm_syncobj_handle sh = { 0 };
		struct drm_panthor_bo_sync bs = { 0 };
		struct drm_panthor_bo_query_info qi = { 0 };
		uint8_t junk[64] = { 0 };

		CHECK_EQ(IOC(&e, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph), PW_EOPNOTSUPP);
		CHECK_EQ(IOC(&e, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph), PW_EOPNOTSUPP);
		CHECK_EQ(IOC(&e, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &sh), PW_EOPNOTSUPP);
		CHECK_EQ(IOC(&e, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &sh), PW_EOPNOTSUPP);
		CHECK_EQ(pw_file_ioctl(e.file, 0xCF, junk, 16), PW_EOPNOTSUPP);	/* EVENTFD */
		CHECK_EQ(pw_file_ioctl(e.file, 0x3f, junk, 8), PW_ENOTTY);
		CHECK_EQ(pw_file_ioctl(e.file, 0x40 + 0x20, junk, 8), PW_ENOTTY);
		/* 1.7 ioctls are not reachable at 1.6 */
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_SYNC, &bs), PW_ENOTTY);
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_QUERY_INFO, &qi), PW_ENOTTY);
	}
	menv_fini(&e);
}

static void test_mem_dev_query(void)
{
	struct menv e;
	struct drm_panthor_dev_query q;
	struct drm_panthor_gpu_info gi, ref;
	struct drm_panthor_csif_info ci;
	struct drm_panthor_timestamp_info ts;
	struct drm_panthor_group_priorities_info pi;
	struct drm_panthor_mmu_info mi;
	uint8_t big[256];

	if (!menv_init(&e))
		return;
	pw_device_get_gpu_info(e.dev, &ref);

	/* size negotiation: pointer == 0 returns the kernel size */
	static const struct { uint32_t type, size; } sizes[] = {
		{ DRM_PANTHOR_DEV_QUERY_GPU_INFO, sizeof(struct drm_panthor_gpu_info) },
		{ DRM_PANTHOR_DEV_QUERY_CSIF_INFO, sizeof(struct drm_panthor_csif_info) },
		{ DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO, sizeof(struct drm_panthor_timestamp_info) },
		{ DRM_PANTHOR_DEV_QUERY_GROUP_PRIORITIES_INFO, 4 },
		{ DRM_PANTHOR_DEV_QUERY_MMU_INFO, 8 },
	};
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		memset(&q, 0, sizeof(q));
		q.type = sizes[i].type;
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), PW_OK);
		CHECK_EQ(q.size, sizes[i].size);
	}
	CHECK_EQ(sizeof(struct drm_panthor_gpu_info), 112);
	CHECK_EQ(sizeof(struct drm_panthor_timestamp_info), 56);
	q.type = 99;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), PW_EINVAL);
	q.pointer = (uintptr_t)big;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), PW_EINVAL);

	/* GPU_INFO, exact size */
	memset(&gi, 0xee, sizeof(gi));
	q = (struct drm_panthor_dev_query){ DRM_PANTHOR_DEV_QUERY_GPU_INFO, sizeof(gi), (uintptr_t)&gi };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), PW_OK);
	CHECK(!memcmp(&gi, &ref, sizeof(gi)));
	CHECK_EQ(gi.gpu_id, MOCK_G610_GPU_ID);
	/* older (1.0) size: truncated, nothing written past it */
	memset(big, 0xee, sizeof(big));
	q.pointer = (uintptr_t)big;
	q.size = offsetof(struct drm_panthor_gpu_info, tiler_present) + 8;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), PW_OK);
	CHECK(!memcmp(big, &ref, q.size));
	CHECK_EQ(big[q.size], 0xee);
	/* below the minimum: EINVAL */
	q.size -= 4;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), PW_EINVAL);
	/* newer (bigger) size: the tail is zeroed */
	memset(big, 0xee, sizeof(big));
	q.size = sizeof(gi) + 16;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), PW_OK);
	CHECK(!memcmp(big, &ref, sizeof(gi)));
	for (size_t i = sizeof(gi); i < sizeof(gi) + 16; i++)
		CHECK_EQ(big[i], 0);
	CHECK_EQ(big[sizeof(gi) + 16], 0xee);

	/* CSIF_INFO */
	q = (struct drm_panthor_dev_query){ DRM_PANTHOR_DEV_QUERY_CSIF_INFO, sizeof(ci), (uintptr_t)&ci };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), PW_OK);
	CHECK_EQ(ci.csg_slot_count, 8);
	CHECK_EQ(ci.cs_reg_count, 96);

	/* TIMESTAMP_INFO, flags 0 = GPU | GPU_OFFSET | FREQ (drv.c:793) */
	memset(&ts, 0, sizeof(ts));
	ts.cpu_timestamp_sec = 1234;	/* output-only field: must be overwritten */
	q = (struct drm_panthor_dev_query){ DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO, sizeof(ts), (uintptr_t)&ts };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), PW_OK);
	CHECK_EQ(ts.timestamp_frequency, 24000000);
	CHECK_EQ(ts.cpu_timestamp_sec, 0);
	CHECK_EQ(ts.duration_nsec, 0);
	/* CPU clock + duration + cycle count */
	memset(&ts, 0, sizeof(ts));
	ts.flags = DRM_PANTHOR_TIMESTAMP_CPU_MONOTONIC | DRM_PANTHOR_TIMESTAMP_GPU |
		   DRM_PANTHOR_TIMESTAMP_DURATION | DRM_PANTHOR_TIMESTAMP_GPU_CYCLE_COUNT;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), PW_OK);
	CHECK_EQ(ts.timestamp_frequency, 0);	/* FREQ not asked */
	CHECK(ts.cpu_timestamp_sec || ts.cpu_timestamp_nsec);
	CHECK(ts.cpu_timestamp_nsec < 1000000000u);
	/* invalid CPU clock type / unknown flag */
	ts.flags = 3u << 1;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), PW_EINVAL);
	ts.flags = 1u << 9;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), PW_EINVAL);
	/* 1.1-era 24-byte struct */
	memset(big, 0xee, sizeof(big));
	memset(big, 0, 24);
	q.pointer = (uintptr_t)big;
	q.size = 24;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), PW_OK);
	CHECK_EQ(((struct drm_panthor_timestamp_info *)big)->timestamp_frequency, 24000000);
	CHECK_EQ(big[24], 0xee);
	/* larger than ours: unknown tail must be zero (copy_struct_from_user -> E2BIG) */
	memset(big, 0, sizeof(big));
	q.size = sizeof(ts) + 8;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), PW_OK);
	big[sizeof(ts) + 3] = 1;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), PW_E2BIG);

	/* GROUP_PRIORITIES_INFO: LOW | MEDIUM */
	memset(&pi, 0xff, sizeof(pi));
	q = (struct drm_panthor_dev_query){ DRM_PANTHOR_DEV_QUERY_GROUP_PRIORITIES_INFO, sizeof(pi), (uintptr_t)&pi };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), PW_OK);
	CHECK_EQ(pi.allowed_mask, 0x3);
	CHECK_EQ(pi.pad[0] | pi.pad[1] | pi.pad[2], 0);

	/* MMU_INFO */
	q = (struct drm_panthor_dev_query){ DRM_PANTHOR_DEV_QUERY_MMU_INFO, sizeof(mi), (uintptr_t)&mi };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_DEV_QUERY, &q), PW_OK);
	CHECK_EQ(mi.page_size_bitmap, 0x201000);
	menv_fini(&e);
}

/* drm_ioctl()-style size handling done generically by the dispatcher. */
static void test_mem_ioctl_size_ext(void)
{
	struct menv e;
	uint8_t buf[96];
	struct drm_panthor_bo_create *bc = (void *)buf;
	struct drm_version *v = (void *)buf;
	struct drm_get_cap *cap = (void *)buf;

	if (!menv_init(&e))
		return;

	/* Older/smaller struct: zero-extended in, truncated out. GET_CAP with only 'capability'. */
	memset(buf, 0xee, sizeof(buf));
	cap->capability = DRM_CAP_SYNCOBJ;
	CHECK_EQ(pw_file_ioctl(e.file, _IOC_NR(DRM_IOCTL_GET_CAP), buf, 8), PW_OK);
	CHECK_EQ(buf[8], 0xee);		/* value not copied back */

	/* VERSION with only the three ints: the pointers/lengths read as 0, nothing else written */
	memset(buf, 0xee, sizeof(buf));
	CHECK_EQ(pw_file_ioctl(e.file, _IOC_NR(DRM_IOCTL_VERSION), buf, 12), PW_OK);
	CHECK_EQ(v->version_major, 1);
	CHECK_EQ(v->version_minor, 6);
	CHECK_EQ(buf[12], 0xee);

	/* BO_CREATE truncated before 'handle': still creates (size/flags read), handle not
	 * returned, so it can only be released by closing the file. */
	memset(buf, 0, sizeof(buf));
	buf[20] = 0xee;
	bc->size = 4096;
	CHECK_EQ(pw_file_ioctl(e.file, _IOC_NR(DRM_IOCTL_PANTHOR_BO_CREATE), buf, 16), PW_OK);
	CHECK_EQ(buf[20], 0xee);
	/* Newer/bigger struct: passed through, unknown tail untouched. */
	memset(buf, 0, sizeof(buf));
	bc->size = 4096;
	memset(buf + sizeof(*bc), 0x5a, 16);
	CHECK_EQ(pw_file_ioctl(e.file, _IOC_NR(DRM_IOCTL_PANTHOR_BO_CREATE), buf, sizeof(*bc) + 16),
		 PW_OK);
	CHECK_EQ(bc->handle, 2);
	CHECK_EQ(buf[sizeof(*bc)], 0x5a);
	/* size 0 needs no buffer; NULL with a size is a fault */
	CHECK_EQ(pw_file_ioctl(e.file, _IOC_NR(DRM_IOCTL_GET_CAP), NULL, 0), PW_EOPNOTSUPP);
	CHECK_EQ(pw_file_ioctl(e.file, _IOC_NR(DRM_IOCTL_GET_CAP), NULL, 16), PW_EFAULT);

	/* file close releases the handle-less BO */
	menv_fini(&e);
}

static void test_mem_obj_array(void)
{
	struct menv e;
	struct item { uint64_t a; uint32_t b, c; } *out = NULL;	/* "kernel" view: 16 bytes */
	uint8_t user[4][24];
	struct drm_panthor_obj_array arr;

	if (!menv_init(&e))
		return;
	memset(user, 0, sizeof(user));
	for (int i = 0; i < 4; i++) {
		uint64_t a = 0x1000 + i;
		uint32_t b = 0x20 + i, c = 0x30 + i;

		memcpy(&user[i][0], &a, 8);
		memcpy(&user[i][8], &b, 4);
		memcpy(&user[i][12], &c, 4);
	}
	/* count 0 */
	arr = (struct drm_panthor_obj_array){ .stride = 16, .count = 0, .array = 0 };
	CHECK_EQ(pw_copy_obj_array(e.file, &arr, 16, 12, (void **)&out), PW_OK);
	CHECK(out == NULL);
	/* same stride (fast path) needs a contiguous array: use stride 24 vs kernel 24 */
	arr = (struct drm_panthor_obj_array){ .stride = 24, .count = 4, .array = (uintptr_t)user };
	{
		uint8_t *k = NULL;

		CHECK_EQ(pw_copy_obj_array(e.file, &arr, 24, 12, (void **)&k), PW_OK);
		CHECK(k && !memcmp(k, user, sizeof(user)));
		pw_os_free(e.os, k);
	}
	/* larger user stride, zero tail: accepted, element by element */
	CHECK_EQ(pw_copy_obj_array(e.file, &arr, 16, 12, (void **)&out), PW_OK);
	REQUIRE(out);
	for (int i = 0; i < 4; i++) {
		CHECK_EQ(out[i].a, 0x1000u + i);
		CHECK_EQ(out[i].b, 0x20u + i);
		CHECK_EQ(out[i].c, 0x30u + i);
	}
	pw_os_free(e.os, out);
	out = NULL;
	/* non-zero byte in the unknown tail */
	user[2][20] = 1;
	CHECK_EQ(pw_copy_obj_array(e.file, &arr, 16, 12, (void **)&out), PW_E2BIG);
	CHECK(out == NULL);
	user[2][20] = 0;
	/* smaller user stride (older uAPI): zero-extended */
	arr.stride = 12;
	arr.array = (uintptr_t)&user[0][0];
	CHECK_EQ(pw_copy_obj_array(e.file, &arr, 16, 12, (void **)&out), PW_OK);
	REQUIRE(out);
	CHECK_EQ(out[0].a, 0x1000);
	CHECK_EQ(out[0].b, 0x20);
	CHECK_EQ(out[0].c, 0);
	pw_os_free(e.os, out);
	out = NULL;
	/* below the minimum stride */
	arr.stride = 8;
	CHECK_EQ(pw_copy_obj_array(e.file, &arr, 16, 12, (void **)&out), PW_EINVAL);
	/* bad pointer */
	arr.stride = 16;
	arr.array = 0;
	CHECK_EQ(pw_copy_obj_array(e.file, &arr, 16, 12, (void **)&out), PW_EFAULT);
	/* absurd count */
	arr.count = 0xffffffffu;
	arr.array = (uintptr_t)user;
	CHECK_EQ(pw_copy_obj_array(e.file, &arr, 16, 12, (void **)&out), PW_ENOMEM);
	menv_fini(&e);
}

static void test_mem_bo_basic(void)
{
	struct menv e;
	struct drm_panthor_bo_create c;
	struct drm_panthor_bo_mmap_offset mo;
	struct drm_gem_close gc;
	struct pw_mmap_target t;
	uint32_t h1, h2, h3, h4;
	uint64_t off1, off2;

	if (!menv_init(&e))
		return;

	/* page-aligned size, handles from 1 */
	c = (struct drm_panthor_bo_create){ .size = 5000 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_CREATE, &c), PW_OK);
	CHECK_EQ(c.size, 8192);
	CHECK_EQ(c.handle, 1);
	h1 = c.handle;
	h2 = bo_new(&e, 3 * 4096, 0);
	CHECK_EQ(h2, 2);
	h3 = bo_new(&e, 4096, DRM_PANTHOR_BO_NO_MMAP);

	/* invalid creates */
	c = (struct drm_panthor_bo_create){ .size = 0 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_CREATE, &c), PW_EINVAL);
	c = (struct drm_panthor_bo_create){ .size = 4096, .pad = 1 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_CREATE, &c), PW_EINVAL);
	c = (struct drm_panthor_bo_create){ .size = 4096, .flags = 1u << 5 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_CREATE, &c), PW_EINVAL);
	c = (struct drm_panthor_bo_create){ .size = 4096, .flags = DRM_PANTHOR_BO_WB_MMAP };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_CREATE, &c), PW_EINVAL);	/* 1.7 flag at 1.6 */
	c = (struct drm_panthor_bo_create){ .size = 4096,
		.flags = DRM_PANTHOR_BO_WB_MMAP | DRM_PANTHOR_BO_NO_MMAP };
	CHECK_EQ(pw_bo_create_user(e.file, &c, 3), PW_EINVAL);
	c = (struct drm_panthor_bo_create){ .size = 4096, .exclusive_vm_id = 7 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_CREATE, &c), PW_EINVAL);
	c = (struct drm_panthor_bo_create){ .size = 1ull << 41 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_CREATE, &c), PW_ENOMEM);
	c = (struct drm_panthor_bo_create){ .size = ~0ull };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_CREATE, &c), PW_ENOMEM);

	/* mmap offsets: in [4G, 1<<43), page aligned, stable, disjoint */
	mo = (struct drm_panthor_bo_mmap_offset){ .handle = h1 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &mo), PW_OK);
	off1 = mo.offset;
	CHECK(off1 >= (1ull << 32) && off1 < DRM_PANTHOR_USER_MMIO_OFFSET_32BIT);
	CHECK_EQ(off1 & 0xfff, 0);
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &mo), PW_OK);
	CHECK_EQ(mo.offset, off1);
	mo = (struct drm_panthor_bo_mmap_offset){ .handle = h2 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &mo), PW_OK);
	off2 = mo.offset;
	CHECK(off2 >= off1 + 8192 || off2 + 3 * 4096 <= off1);
	mo = (struct drm_panthor_bo_mmap_offset){ .handle = h3 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &mo), PW_EPERM);
	mo = (struct drm_panthor_bo_mmap_offset){ .handle = 99 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &mo), PW_ENOENT);
	mo = (struct drm_panthor_bo_mmap_offset){ .handle = h1, .pad = 1 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &mo), PW_EINVAL);

	/* mmap lookup */
	CHECK_EQ(pw_file_mmap_lookup(e.file, off2, 3 * 4096, &t), PW_OK);
	CHECK_EQ(t.kind, PW_MMAP_BO);
	CHECK_EQ(t.offset_in_obj, 0);
	CHECK(!t.read_only);
	CHECK(!t.cached);		/* write-combined by default */
	CHECK(pw_bo_os_mem(t.bo) != NULL);
	CHECK_EQ(pw_os_mem_size(pw_bo_os_mem(t.bo)), 3 * 4096);
	pw_bo_put(t.bo);
	CHECK_EQ(pw_file_mmap_lookup(e.file, off2 + 4096, 8192, &t), PW_OK);
	CHECK_EQ(t.offset_in_obj, 4096);
	pw_bo_put(t.bo);
	CHECK_EQ(pw_file_mmap_lookup(e.file, off2 + 4096, 3 * 4096, &t), PW_EINVAL);
	CHECK_EQ(pw_file_mmap_lookup(e.file, off2 + 1, 4096, &t), PW_EINVAL);
	CHECK_EQ(pw_file_mmap_lookup(e.file, off2, 0, &t), PW_EINVAL);
	CHECK_EQ(pw_file_mmap_lookup(e.file, 1ull << 40, 4096, &t), PW_EINVAL);
	CHECK_EQ(pw_file_mmap_lookup(e.file, 4096, 4096, &t), PW_EINVAL);

	/* A user mapping keeps the BO alive after GEM_CLOSE; the offset is revoked. */
	CHECK_EQ(pw_file_mmap_lookup(e.file, off1, 8192, &t), PW_OK);
	CHECK_EQ(bo_refs(t.bo), 2);
	gc = (struct drm_gem_close){ .handle = h1 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_GEM_CLOSE, &gc), PW_OK);
	CHECK_EQ(IOC(&e, DRM_IOCTL_GEM_CLOSE, &gc), PW_EINVAL);
	CHECK_EQ(bo_refs(t.bo), 1);
	{
		struct pw_mmap_target t2;

		CHECK_EQ(pw_file_mmap_lookup(e.file, off1, 4096, &t2), PW_EINVAL);
	}
	{
		long mems = mock_live_mems(e.os);

		pw_bo_put(t.bo);	/* the OS layer's munmap */
		CHECK_EQ(mock_live_mems(e.os), mems - 1);
	}
	/* the freed offset range is reused (first fit), the handle too (lowest free) */
	h4 = bo_new(&e, 4096, 0);
	CHECK_EQ(h4, 1);
	mo = (struct drm_panthor_bo_mmap_offset){ .handle = h4 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &mo), PW_OK);
	CHECK_EQ(mo.offset, off1);
	/* unknown handle */
	gc = (struct drm_gem_close){ .handle = 1234 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_GEM_CLOSE, &gc), PW_EINVAL);

	/* leave BOs with offsets alive: closing the file releases them (leak check in fini) */
	menv_fini(&e);
}

static void test_mem_user_mmio(void)
{
	struct menv e;
	struct pw_mmap_target t;
	struct drm_panthor_set_user_mmio_offset so;

	if (!menv_init(&e))
		return;
	CHECK_EQ(e.file->user_mmio_offset, DRM_PANTHOR_USER_MMIO_OFFSET_64BIT);
	for (int i = 0; i < 2; i++) {
		uint64_t off = i ? DRM_PANTHOR_USER_MMIO_OFFSET_64BIT : DRM_PANTHOR_USER_MMIO_OFFSET_32BIT;

		memset(&t, 0xff, sizeof(t));
		CHECK_EQ(pw_file_mmap_lookup(e.file, off, 4096, &t), PW_OK);
		CHECK_EQ(t.kind, PW_MMAP_MMIO);
		CHECK_EQ(t.mmio_offset, 0x10000);	/* CSF_GPU_LATEST_FLUSH_ID */
		CHECK(t.read_only);
		CHECK(!t.cached);
		CHECK(t.bo == NULL);
		CHECK_EQ(pw_file_mmap_lookup(e.file, off, 8192, &t), PW_EINVAL);
		CHECK_EQ(pw_file_mmap_lookup(e.file, off + 4096, 4096, &t), PW_EINVAL);
	}
	CHECK_EQ(pw_file_mmap_lookup(e.file, (1ull << 44), 4096, &t), PW_EINVAL);

	so.offset = DRM_PANTHOR_USER_MMIO_OFFSET_32BIT;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_SET_USER_MMIO_OFFSET, &so), PW_OK);
	CHECK_EQ(e.file->user_mmio_offset, DRM_PANTHOR_USER_MMIO_OFFSET_32BIT);
	/* both stay accepted */
	CHECK_EQ(pw_file_mmap_lookup(e.file, DRM_PANTHOR_USER_MMIO_OFFSET_64BIT, 4096, &t), PW_OK);
	so.offset = 1ull << 50;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_SET_USER_MMIO_OFFSET, &so), PW_EINVAL);
	menv_fini(&e);
}

static void test_mem_bo_label(void)
{
	struct menv e;
	struct drm_panthor_bo_set_label sl;
	struct pw_bo *bo;
	char *big;
	uint32_t h;

	if (!menv_init(&e))
		return;
	h = bo_new(&e, 4096, 0);
	bo = pw_file_bo_get(e.file, h);
	REQUIRE(bo);

	sl = (struct drm_panthor_bo_set_label){ .handle = h, .label = (uintptr_t)"cmdbuf" };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_SET_LABEL, &sl), PW_OK);
	CHECK(!strcmp(bo->label, "cmdbuf"));
	sl.label = 0;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_SET_LABEL, &sl), PW_OK);
	CHECK_EQ(bo->label[0], 0);

	/* 4095 chars + NUL: accepted, stored truncated */
	big = malloc(8192);
	REQUIRE(big);
	memset(big, 'a', 8192);
	big[4095] = 0;
	sl.label = (uintptr_t)big;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_SET_LABEL, &sl), PW_OK);
	CHECK_EQ(strlen(bo->label), sizeof(bo->label) - 1);
	/* no NUL within 4096 bytes: E2BIG (strndup_user's EINVAL, drv.c:1489) */
	big[4095] = 'a';
	big[5000] = 0;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_SET_LABEL, &sl), PW_E2BIG);
	free(big);

	sl = (struct drm_panthor_bo_set_label){ .handle = h, .pad = 1 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_SET_LABEL, &sl), PW_EINVAL);
	sl = (struct drm_panthor_bo_set_label){ .handle = 77 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_SET_LABEL, &sl), PW_ENOENT);
	pw_bo_put(bo);
	menv_fini(&e);
}

/* 1.7 features, implemented but not advertised: exercised through the handlers directly. */
static void test_mem_bo_wb_sync_query(void)
{
	struct menv e;
	struct drm_panthor_bo_create c;
	struct drm_panthor_bo_mmap_offset mo;
	struct drm_panthor_bo_sync_op ops[2];
	struct drm_panthor_bo_sync bs;
	struct drm_panthor_bo_query_info qi;
	struct pw_mmap_target t;

	if (!menv_init(&e))
		return;
	c = (struct drm_panthor_bo_create){ .size = 16384, .flags = DRM_PANTHOR_BO_WB_MMAP };
	CHECK_EQ(pw_bo_create_user(e.file, &c,
				   DRM_PANTHOR_BO_NO_MMAP | DRM_PANTHOR_BO_WB_MMAP), PW_OK);
	mo = (struct drm_panthor_bo_mmap_offset){ .handle = c.handle };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &mo), PW_OK);
	CHECK_EQ(pw_file_mmap_lookup(e.file, mo.offset, 16384, &t), PW_OK);
	CHECK(t.cached);
	pw_bo_put(t.bo);

	ops[0] = (struct drm_panthor_bo_sync_op){ .handle = c.handle,
		.type = DRM_PANTHOR_BO_SYNC_CPU_CACHE_FLUSH, .offset = 4096, .size = 8192 };
	ops[1] = (struct drm_panthor_bo_sync_op){ .handle = c.handle,
		.type = DRM_PANTHOR_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE, .offset = 0, .size = 16384 };
	bs.ops = (struct drm_panthor_obj_array)DRM_PANTHOR_OBJ_ARRAY(2, ops);
	CHECK_EQ(pw_ioctl_bo_sync(e.file, &bs), PW_OK);
	ops[1].size = 16385;
	CHECK_EQ(pw_ioctl_bo_sync(e.file, &bs), PW_EINVAL);
	ops[1].size = 0;
	ops[1].offset = ~0ull;
	CHECK_EQ(pw_ioctl_bo_sync(e.file, &bs), PW_EINVAL);
	ops[1].offset = 0;
	ops[1].type = 2;
	CHECK_EQ(pw_ioctl_bo_sync(e.file, &bs), PW_EINVAL);
	ops[1].type = 0;
	ops[1].handle = 55;
	CHECK_EQ(pw_ioctl_bo_sync(e.file, &bs), PW_ENOENT);
	bs.ops.count = 0;
	CHECK_EQ(pw_ioctl_bo_sync(e.file, &bs), PW_OK);

	qi = (struct drm_panthor_bo_query_info){ .handle = c.handle, .pad = 9 };
	CHECK_EQ(pw_ioctl_bo_query_info(e.file, &qi), PW_OK);
	CHECK_EQ(qi.create_flags, DRM_PANTHOR_BO_WB_MMAP);
	CHECK_EQ(qi.extra_flags, 0);
	CHECK_EQ(qi.pad, 0);
	qi.handle = 55;
	CHECK_EQ(pw_ioctl_bo_query_info(e.file, &qi), PW_ENOENT);
	menv_fini(&e);
}

static void test_mem_vm_create(void)
{
	struct menv e;
	struct drm_panthor_vm_create c;
	struct drm_panthor_vm_destroy d;
	struct drm_panthor_vm_get_state gs;
	const uint64_t full = 1ull << 48;
	uint32_t ids[32];

	if (!menv_init(&e))
		return;
	/* 0: TASK_SIZE (128 TiB on Windows ARM64) < 256 TiB GPU VA -> user = 1 << 47 */
	c = (struct drm_panthor_vm_create){ 0 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_CREATE, &c), PW_OK);
	CHECK_EQ(c.id, 1);
	CHECK_EQ(c.user_va_range, 1ull << 47);
	/* explicit: what Mesa passes for 48-bit VA */
	c = (struct drm_panthor_vm_create){ .user_va_range = 1ull << 47 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_CREATE, &c), PW_OK);
	CHECK_EQ(c.id, 2);
	CHECK_EQ(c.user_va_range, 1ull << 47);
	/* kernel range is rounded down to a power of two (mmu.c:1567) */
	c = (struct drm_panthor_vm_create){ .user_va_range = (1ull << 47) + 4096 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_CREATE, &c), PW_OK);
	CHECK_EQ(c.user_va_range, 3ull << 46);
	c = (struct drm_panthor_vm_create){ .user_va_range = 1ull << 32 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_CREATE, &c), PW_OK);
	CHECK_EQ(c.user_va_range, 1ull << 47);
	/* largest allowed: leaves the 256 MiB minimum kernel range */
	c = (struct drm_panthor_vm_create){ .user_va_range = full - (256ull << 20) };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_CREATE, &c), PW_OK);
	CHECK_EQ(c.user_va_range, full - (256ull << 20));
	c = (struct drm_panthor_vm_create){ .user_va_range = full - (256ull << 20) + 4096 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_CREATE, &c), PW_EINVAL);
	c = (struct drm_panthor_vm_create){ .flags = 1 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_CREATE, &c), PW_EINVAL);

	gs = (struct drm_panthor_vm_get_state){ .vm_id = 1, .state = 77 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_GET_STATE, &gs), PW_OK);
	CHECK_EQ(gs.state, DRM_PANTHOR_VM_STATE_USABLE);
	gs.vm_id = 40;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_GET_STATE, &gs), PW_EINVAL);

	d = (struct drm_panthor_vm_destroy){ .id = 2, .pad = 1 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_DESTROY, &d), PW_EINVAL);
	d.pad = 0;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_DESTROY, &d), PW_OK);
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_DESTROY, &d), PW_EINVAL);
	d.id = 0;
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_DESTROY, &d), PW_EINVAL);

	/* 32 VMs per file (mmu.c:1576); 4 exist now */
	for (int i = 0; i < 28; i++) {
		c = (struct drm_panthor_vm_create){ 0 };
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_CREATE, &c), PW_OK);
		ids[i] = c.id;
	}
	CHECK_EQ(ids[0], 2);		/* lowest free id reused */
	c = (struct drm_panthor_vm_create){ 0 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_CREATE, &c), PW_EBUSY);
	d = (struct drm_panthor_vm_destroy){ .id = ids[5] };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_DESTROY, &d), PW_OK);
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_CREATE, &c), PW_OK);
	CHECK_EQ(c.id, ids[5]);
	menv_fini(&e);
}

static void test_mem_vm_bind_map(void)
{
	struct menv e;
	struct pw_bo *bo;
	uint64_t pa, pte;
	uint32_t vm, h, h2;
	int as, lvl;
	const uint64_t va = 0x10000000;

	if (!menv_init(&e))
		return;
	vm = vm_new(&e, 0);
	h = bo_new(&e, 16 * 4096, 0);
	h2 = bo_new(&e, 4 * 4096, 0);
	as = vm_as(&e, vm, NULL);
	REQUIRE(as >= 1);

	/* plain map: every page translates to the BO's page through the GPU's walker */
	CHECK_EQ(bind1(&e, vm, MAP | DRM_PANTHOR_VM_BIND_OP_MAP_NOEXEC, h, 0, va, 16 * 4096), PW_OK);
	for (int i = 0; i < 16; i++) {
		CHECK_EQ(mock_gpu_translate(e.os, as, va + i * 4096 + 0x10, 'w', &pa, &lvl, &pte), 0);
		CHECK_EQ(pa, bo_pa(&e, h, i * 4096 + 0x10));
		CHECK_EQ(lvl, 3);
	}
	/* NOEXEC, cached by default (WB MAIR index) */
	CHECK_EQ(mock_gpu_translate(e.os, as, va, 'x', NULL, NULL, NULL), 0xcb);
	CHECK_EQ(pte & PW_PTE_ATTRINDX_MASK, PW_PTE_ATTRINDX(PW_MAIR_IDX_WB));
	CHECK(mock_gpu_translate(e.os, as, va + 16 * 4096, 'r', NULL, NULL, NULL) >= 0xc0);

	/* READONLY + UNCACHED, executable, with a bo_offset */
	CHECK_EQ(bind1(&e, vm, MAP | DRM_PANTHOR_VM_BIND_OP_MAP_READONLY |
		       DRM_PANTHOR_VM_BIND_OP_MAP_UNCACHED, h, 8192, 0x20000000, 8192), PW_OK);
	CHECK_EQ(mock_gpu_translate(e.os, as, 0x20000000, 'r', &pa, NULL, &pte), 0);
	CHECK_EQ(pa, bo_pa(&e, h, 8192));
	CHECK_EQ(mock_gpu_translate(e.os, as, 0x20001000, 'x', &pa, NULL, NULL), 0);
	CHECK_EQ(pa, bo_pa(&e, h, 12288));
	CHECK_EQ(mock_gpu_translate(e.os, as, 0x20000000, 'w', NULL, NULL, NULL), 0xcb);
	CHECK_EQ(pte & PW_PTE_ATTRINDX_MASK, PW_PTE_ATTRINDX(PW_MAIR_IDX_NC));

	/* MAP over an existing mapping replaces the overlapped part (drm_gpuvm sm_map) */
	CHECK_EQ(bind1(&e, vm, MAP, h2, 0, va + 4 * 4096, 4 * 4096), PW_OK);
	CHECK_EQ(mock_gpu_translate(e.os, as, va + 3 * 4096, 'r', &pa, NULL, NULL), 0);
	CHECK_EQ(pa, bo_pa(&e, h, 3 * 4096));
	for (int i = 4; i < 8; i++) {
		CHECK_EQ(mock_gpu_translate(e.os, as, va + i * 4096, 'x', &pa, NULL, NULL), 0);
		CHECK_EQ(pa, bo_pa(&e, h2, (i - 4) * 4096));
	}
	CHECK_EQ(mock_gpu_translate(e.os, as, va + 8 * 4096, 'r', &pa, NULL, NULL), 0);
	CHECK_EQ(pa, bo_pa(&e, h, 8 * 4096));

	/* references: h is mapped by 3 VMAs (two halves of the split + the RO one), h2 by one */
	bo = pw_file_bo_get(e.file, h);
	REQUIRE(bo);
	CHECK_EQ(bo_refs(bo), 1 + 1 + 3);

	/* partial unmap, across the h/h2 boundary */
	CHECK_EQ(bind1(&e, vm, UNMAP, 0, 0, va + 2 * 4096, 4 * 4096), PW_OK);
	for (int i = 2; i < 6; i++)
		CHECK(mock_gpu_translate(e.os, as, va + i * 4096, 'r', NULL, NULL, NULL) >= 0xc0);
	CHECK_EQ(mock_gpu_translate(e.os, as, va + 1 * 4096, 'r', &pa, NULL, NULL), 0);
	CHECK_EQ(pa, bo_pa(&e, h, 4096));
	CHECK_EQ(mock_gpu_translate(e.os, as, va + 6 * 4096, 'r', &pa, NULL, NULL), 0);
	CHECK_EQ(pa, bo_pa(&e, h2, 2 * 4096));
	/* unmapping a hole is fine */
	CHECK_EQ(bind1(&e, vm, UNMAP, 0, 0, 0x30000000, 1ull << 30), PW_OK);

	/* GEM_CLOSE while mapped: the mappings keep the BO alive */
	{
		struct drm_gem_close gc = { .handle = h };

		CHECK_EQ(IOC(&e, DRM_IOCTL_GEM_CLOSE, &gc), PW_OK);
		CHECK_EQ(bo_refs(bo), 1 + 3);
		CHECK_EQ(mock_gpu_translate(e.os, as, va, 'r', NULL, NULL, NULL), 0);
	}
	/* unmap everything of the user range */
	CHECK_EQ(bind1(&e, vm, UNMAP, 0, 0, 0, 1ull << 47), PW_OK);
	CHECK_EQ(bo_refs(bo), 1);
	CHECK(mock_gpu_translate(e.os, as, va, 'r', NULL, NULL, NULL) >= 0xc0);
	CHECK(mock_gpu_translate(e.os, as, 0x20000000, 'r', NULL, NULL, NULL) >= 0xc0);
	pw_bo_put(bo);

	/* VM_DESTROY drops the mapping references */
	bo = pw_file_bo_get(e.file, h2);
	REQUIRE(bo);
	CHECK_EQ(bind1(&e, vm, MAP, h2, 0, va, 4 * 4096), PW_OK);
	CHECK_EQ(bo_refs(bo), 3);
	{
		struct drm_panthor_vm_destroy d = { .id = vm };

		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_DESTROY, &d), PW_OK);
	}
	CHECK_EQ(bo_refs(bo), 2);
	pw_bo_put(bo);
	menv_fini(&e);
}

static void test_mem_vm_bind_errors(void)
{
	struct menv e;
	uint32_t vm, vm2, h, hx;
	struct drm_panthor_vm_bind_op ops[3];
	struct drm_panthor_vm_bind b;
	struct drm_panthor_sync_op so = { 0 };
	struct drm_panthor_bo_create c;
	int as;

	if (!menv_init(&e))
		return;
	vm = vm_new(&e, 0);
	vm2 = vm_new(&e, 0);
	h = bo_new(&e, 8 * 4096, 0);
	as = vm_as(&e, vm, NULL);
	REQUIRE(as >= 1);

	CHECK_EQ(bind1(&e, 99, MAP, h, 0, 0x100000, 4096), PW_EINVAL);			/* VM */
	CHECK_EQ(bind1(&e, vm, MAP, h, 0, 0x100800, 4096), PW_EINVAL);			/* align */
	CHECK_EQ(bind1(&e, vm, MAP, h, 0x800, 0x100000, 4096), PW_EINVAL);
	CHECK_EQ(bind1(&e, vm, MAP, h, 0, 0x100000, 0x1800), PW_EINVAL);
	CHECK_EQ(bind1(&e, vm, MAP, h, 0, 0x100000, 9 * 4096), PW_EINVAL);		/* > BO */
	CHECK_EQ(bind1(&e, vm, MAP, h, 4096, 0x100000, 8 * 4096), PW_EINVAL);
	CHECK_EQ(bind1(&e, vm, MAP, h, 9 * 4096, 0x100000, 0), PW_OK);			/* size 0: no-op */
	CHECK_EQ(bind1(&e, vm, MAP, 77, 0, 0x100000, 4096), PW_EINVAL);			/* BO */
	CHECK_EQ(bind1(&e, vm, MAP, h, 0, (1ull << 47) - 4096, 8192), PW_EINVAL);	/* kernel VA */
	CHECK_EQ(bind1(&e, vm, MAP, h, 0, 1ull << 47, 4096), PW_EINVAL);
	CHECK_EQ(bind1(&e, vm, MAP | DRM_PANTHOR_VM_BIND_OP_MAP_SPARSE, h, 0, 0x100000, 4096),
		 PW_EINVAL);								/* 1.9 */
	CHECK_EQ(bind1(&e, vm, MAP | (1u << 7), h, 0, 0x100000, 4096), PW_EINVAL);
	CHECK_EQ(bind1(&e, vm, UNMAP | DRM_PANTHOR_VM_BIND_OP_MAP_READONLY, 0, 0, 0x100000, 4096),
		 PW_EINVAL);
	CHECK_EQ(bind1(&e, vm, UNMAP, h, 0, 0x100000, 4096), PW_EINVAL);
	CHECK_EQ(bind1(&e, vm, UNMAP, 0, 4096, 0x100000, 4096), PW_EINVAL);
	CHECK_EQ(bind1(&e, vm, 3u << 28, 0, 0, 0x100000, 4096), PW_EINVAL);		/* type */
	/* SYNC_ONLY in synchronous mode has size 0: skipped like panthor (mmu.c:3170) */
	CHECK_EQ(bind1(&e, vm, DRM_PANTHOR_VM_BIND_OP_TYPE_SYNC_ONLY, 0, 0, 0, 0), PW_OK);
	CHECK(mock_gpu_translate(e.os, as, 0x100000, 'r', NULL, NULL, NULL) >= 0xc0);

	/* syncs are not allowed without ASYNC; failing op index reported in ops.count */
	memset(ops, 0, sizeof(ops));
	ops[0] = (struct drm_panthor_vm_bind_op){ .flags = MAP, .bo_handle = h, .va = 0x100000, .size = 4096 };
	ops[1] = (struct drm_panthor_vm_bind_op){ .flags = MAP, .bo_handle = h, .va = 0x200000, .size = 4096,
		.syncs = DRM_PANTHOR_OBJ_ARRAY(1, &so) };
	ops[2] = (struct drm_panthor_vm_bind_op){ .flags = MAP, .bo_handle = h, .va = 0x300000, .size = 4096 };
	b = (struct drm_panthor_vm_bind){ .vm_id = vm, .ops = DRM_PANTHOR_OBJ_ARRAY(3, ops) };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_BIND, &b), PW_EINVAL);
	CHECK_EQ(b.ops.count, 1);
	CHECK_EQ(mock_gpu_translate(e.os, as, 0x100000, 'r', NULL, NULL, NULL), 0);
	CHECK(mock_gpu_translate(e.os, as, 0x300000, 'r', NULL, NULL, NULL) >= 0xc0);
	/* op array with a too-small stride */
	b = (struct drm_panthor_vm_bind){ .vm_id = vm, .ops = { .stride = 16, .count = 1,
		.array = (uintptr_t)ops } };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_BIND, &b), PW_EINVAL);
	/* a newer, bigger op struct with a zero tail is fine */
	{
		uint8_t big[2][sizeof(ops[0]) + 8];

		memset(big, 0, sizeof(big));
		memcpy(big[0], &ops[2], sizeof(ops[0]));
		ops[0].va = 0x400000;
		memcpy(big[1], &ops[0], sizeof(ops[0]));
		b = (struct drm_panthor_vm_bind){ .vm_id = vm, .ops = { .stride = sizeof(big[0]),
			.count = 2, .array = (uintptr_t)big } };
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_BIND, &b), PW_OK);
		CHECK_EQ(mock_gpu_translate(e.os, as, 0x300000, 'r', NULL, NULL, NULL), 0);
		CHECK_EQ(mock_gpu_translate(e.os, as, 0x400000, 'r', NULL, NULL, NULL), 0);
	}
	/* empty bind */
	b = (struct drm_panthor_vm_bind){ .vm_id = vm };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_BIND, &b), PW_OK);

	/* exclusive VM BOs only map in their VM */
	c = (struct drm_panthor_bo_create){ .size = 4096, .exclusive_vm_id = vm2 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_CREATE, &c), PW_OK);
	hx = c.handle;
	CHECK_EQ(bind1(&e, vm, MAP, hx, 0, 0x500000, 4096), PW_EINVAL);
	CHECK_EQ(bind1(&e, vm2, MAP, hx, 0, 0x500000, 4096), PW_OK);
	/* the exclusive VM going away leaves the BO usable nowhere else */
	{
		struct drm_panthor_vm_destroy d = { .id = vm2 };

		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_DESTROY, &d), PW_OK);
		vm2 = vm_new(&e, 0);	/* same id, different VM */
		CHECK_EQ(bind1(&e, vm2, MAP, hx, 0, 0x500000, 4096), PW_EINVAL);
	}
	menv_fini(&e);
}

/* 2 MiB blocks and partial unmaps that split them, through VM_BIND. User BOs are not physically
 * contiguous in the mock, so a contiguous kernel BO is published in the handle table. */
static void test_mem_vm_bind_2m_split(void)
{
	struct menv e;
	struct pw_bo *kbo;
	uint64_t pa, base_pa;
	uint32_t vm, h;
	int as, lvl;
	const uint64_t va = 0x40000000;

	if (!menv_init(&e))
		return;
	vm = vm_new(&e, 0);
	REQUIRE(pw_bo_create_kernel(e.dev, 4 * 1024 * 1024, PW_MEM_CONTIGUOUS, &kbo) == PW_OK);
	base_pa = pw_os_mem_page_pa(kbo->mem, 0);
	REQUIRE((base_pa & (PW_SZ_2M - 1)) == 0);
	REQUIRE(pw_handle_alloc(e.os, &e.file->bos, kbo, &h) == PW_OK);	/* table owns the ref */
	pw_bo_get(kbo);
	as = vm_as(&e, vm, NULL);
	REQUIRE(as >= 1);

	CHECK_EQ(bind1(&e, vm, MAP, h, 0, va, 4 << 20), PW_OK);
	CHECK_EQ(mock_gpu_translate(e.os, as, va + 0x1234, 'r', &pa, &lvl, NULL), 0);
	CHECK_EQ(lvl, 2);
	CHECK_EQ(pa, base_pa + 0x1234);
	CHECK_EQ(mock_gpu_translate(e.os, as, va + 0x3ff000, 'r', &pa, &lvl, NULL), 0);
	CHECK_EQ(lvl, 2);
	CHECK_EQ(bo_refs(kbo), 3);

	/* one page out of the second block: that block becomes a level-3 table */
	CHECK_EQ(bind1(&e, vm, UNMAP, 0, 0, va + 0x201000, 0x1000), PW_OK);
	CHECK(mock_gpu_translate(e.os, as, va + 0x201000, 'r', NULL, NULL, NULL) >= 0xc0);
	CHECK_EQ(mock_gpu_translate(e.os, as, va + 0x200000, 'r', &pa, &lvl, NULL), 0);
	CHECK_EQ(lvl, 3);
	CHECK_EQ(pa, base_pa + 0x200000);
	CHECK_EQ(mock_gpu_translate(e.os, as, va + 0x202000, 'r', &pa, &lvl, NULL), 0);
	CHECK_EQ(lvl, 3);
	CHECK_EQ(pa, base_pa + 0x202000);
	CHECK_EQ(mock_gpu_translate(e.os, as, va + 0x1000, 'r', &pa, &lvl, NULL), 0);
	CHECK_EQ(lvl, 2);
	CHECK_EQ(bo_refs(kbo), 4);	/* split VMA: two pieces */

	/* a range straddling both blocks */
	CHECK_EQ(bind1(&e, vm, UNMAP, 0, 0, va + 0x100000, 0x200000), PW_OK);
	CHECK_EQ(mock_gpu_translate(e.os, as, va + 0xff000, 'r', &pa, &lvl, NULL), 0);
	CHECK_EQ(lvl, 3);
	CHECK_EQ(pa, base_pa + 0xff000);
	CHECK(mock_gpu_translate(e.os, as, va + 0x100000, 'r', NULL, NULL, NULL) >= 0xc0);
	CHECK(mock_gpu_translate(e.os, as, va + 0x2ff000, 'r', NULL, NULL, NULL) >= 0xc0);
	CHECK_EQ(mock_gpu_translate(e.os, as, va + 0x300000, 'r', &pa, NULL, NULL), 0);
	CHECK_EQ(pa, base_pa + 0x300000);
	CHECK_EQ(bo_refs(kbo), 4);

	/* remap into the hole with a bo_offset that matches the old layout */
	CHECK_EQ(bind1(&e, vm, MAP, h, 0x100000, va + 0x100000, 0x200000), PW_OK);
	CHECK_EQ(mock_gpu_translate(e.os, as, va + 0x201000, 'r', &pa, NULL, NULL), 0);
	CHECK_EQ(pa, base_pa + 0x201000);
	CHECK_EQ(bo_refs(kbo), 5);

	CHECK_EQ(bind1(&e, vm, UNMAP, 0, 0, va, 4 << 20), PW_OK);
	CHECK_EQ(bo_refs(kbo), 2);
	CHECK(mock_gpu_translate(e.os, as, va, 'r', NULL, NULL, NULL) >= 0xc0);
	pw_bo_put(kbo);
	menv_fini(&e);
}

static void test_mem_kernel_mappings(void)
{
	struct menv e;
	struct pw_uvm *uvm;
	struct pw_bo *k1, *k2, *k3;
	uint64_t va1, va2, va3, kstart, kend, pa;
	uint32_t vm;
	int as, lvl;

	if (!menv_init(&e))
		return;
	vm = vm_new(&e, 0);
	as = vm_as(&e, vm, &uvm);
	REQUIRE(as >= 1);
	pw_uvm_kernel_va_range(uvm, &kstart, &kend);
	CHECK_EQ(kstart, 1ull << 47);
	CHECK_EQ(kend, 1ull << 48);

	REQUIRE(pw_bo_create_kernel(e.dev, 8192, PW_MEM_KMAP, &k1) == PW_OK);
	REQUIRE(pw_bo_create_kernel(e.dev, 2 << 20, PW_MEM_CONTIGUOUS, &k2) == PW_OK);
	REQUIRE(pw_bo_create_kernel(e.dev, 4096, 0, &k3) == PW_OK);
	CHECK(pw_os_mem_kva(k1->mem) != NULL);
	CHECK(k1->flags & DRM_PANTHOR_BO_NO_MMAP);

	CHECK_EQ(pw_uvm_map_kernel_bo(uvm, k1, PW_VM_MAP_NOEXEC, &va1), PW_OK);
	CHECK_EQ(pw_uvm_map_kernel_bo(uvm, k2, PW_VM_MAP_NOEXEC, &va2), PW_OK);
	CHECK_EQ(pw_uvm_map_kernel_bo(uvm, k3, PW_VM_MAP_READONLY, &va3), PW_OK);
	CHECK_EQ(va1, kstart);
	CHECK_EQ(va2 & (PW_SZ_2M - 1), 0);
	CHECK(va2 >= va1 + 8192);
	CHECK_EQ(va3, va1 + 8192);	/* first fit */
	CHECK_EQ(mock_gpu_translate(e.os, as, va1 + 4096, 'w', &pa, NULL, NULL), 0);
	CHECK_EQ(pa, pw_os_mem_page_pa(k1->mem, 1));
	CHECK_EQ(mock_gpu_translate(e.os, as, va2 + 0x12345, 'r', &pa, &lvl, NULL), 0);
	CHECK_EQ(lvl, 2);
	CHECK_EQ(pa, pw_os_mem_page_pa(k2->mem, 0) + 0x12345);
	CHECK_EQ(mock_gpu_translate(e.os, as, va3, 'w', NULL, NULL, NULL), 0xcb);
	CHECK_EQ(bo_refs(k1), 2);
	{
		uint64_t dummy = 1;

		CHECK_EQ(pw_uvm_map_kernel_bo(uvm, k1, 1u << 12, &dummy), PW_EINVAL);
		CHECK_EQ(dummy, 0);
	}

	/* user binds can't touch the kernel range */
	CHECK_EQ(bind1(&e, vm, UNMAP, 0, 0, kstart, 4096), PW_EINVAL);

	pw_uvm_unmap_kernel_bo(uvm, va1);
	CHECK(mock_gpu_translate(e.os, as, va1, 'r', NULL, NULL, NULL) >= 0xc0);
	CHECK_EQ(bo_refs(k1), 1);
	pw_uvm_unmap_kernel_bo(uvm, va1);	/* already gone: no-op */
	pw_uvm_unmap_kernel_bo(uvm, va2 + 4096);	/* not a mapping start: no-op */
	CHECK_EQ(mock_gpu_translate(e.os, as, va2, 'r', NULL, NULL, NULL), 0);
	/* the freed slot is reused */
	CHECK_EQ(pw_uvm_map_kernel_bo(uvm, k1, 0, &va1), PW_OK);
	CHECK_EQ(va1, kstart);

	/* VM_DESTROY unmaps the kernel range too (mmu.c:1633); our uvm ref keeps it allocated */
	{
		struct drm_panthor_vm_destroy d = { .id = vm };

		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_DESTROY, &d), PW_OK);
	}
	CHECK_EQ(bo_refs(k1), 1);
	CHECK_EQ(bo_refs(k2), 1);
	CHECK(mock_gpu_translate(e.os, as, va2, 'r', NULL, NULL, NULL) >= 0xc0);
	{
		uint64_t dummy;

		CHECK_EQ(pw_uvm_map_kernel_bo(uvm, k1, 0, &dummy), PW_EINVAL);
	}
	pw_uvm_unmap_kernel_bo(uvm, va3);
	pw_uvm_put(uvm);
	pw_bo_put(k1);
	pw_bo_put(k2);
	pw_bo_put(k3);
	menv_fini(&e);
}

static bool vm_unusable(void *arg)
{
	struct menv *e = arg;
	struct drm_panthor_vm_get_state gs = { .vm_id = 1 };

	return IOC(e, DRM_IOCTL_PANTHOR_VM_GET_STATE, &gs) == PW_OK &&
	       gs.state == DRM_PANTHOR_VM_STATE_UNUSABLE;
}

static void test_mem_vm_unusable(void)
{
	struct menv e;
	struct pw_uvm *uvm;
	uint32_t vm, h;
	int as;

	if (!menv_init(&e))
		return;
	vm = vm_new(&e, 0);
	CHECK_EQ(vm, 1);
	h = bo_new(&e, 4096, 0);
	as = vm_as(&e, vm, &uvm);
	REQUIRE(as >= 1);
	CHECK_EQ(bind1(&e, vm, MAP, h, 0, 0x100000, 4096), PW_OK);
	CHECK(!pw_uvm_is_unusable(uvm));

	/* unrecoverable GPU page fault on the VM's AS: sticky UNUSABLE */
	mock_inject_mmu_fault(e.os, (unsigned int)as, 0x000003c1u, 0x200000);
	CHECK(wait_until(vm_unusable, &e, 2000));
	CHECK(pw_uvm_is_unusable(uvm));
	CHECK_EQ(bind1(&e, vm, MAP, h, 0, 0x300000, 4096), PW_EINVAL);
	CHECK_EQ(bind1(&e, vm, UNMAP, 0, 0, 0x100000, 4096), PW_OK);	/* unmap still allowed */
	CHECK_EQ(pw_uvm_activate(uvm), PW_EINVAL);
	CHECK(vm_unusable(&e));
	pw_uvm_put(uvm);

	/* a fresh VM is fine */
	vm = vm_new(&e, 0);
	CHECK_EQ(bind1(&e, vm, MAP, h, 0, 0x300000, 4096), PW_OK);
	menv_fini(&e);
}

/* Async binds: without fences (no sync ops) they run like sync ones; with sync ops they need
 * pw_sync.c (implemented in parallel; tolerated/skipped while it is a stub). */
static void test_mem_vm_bind_async(void)
{
	struct menv e;
	struct drm_panthor_vm_bind_op op;
	struct drm_panthor_vm_bind b;
	struct drm_panthor_sync_op so[2];
	struct drm_syncobj_create sc;
	uint32_t vm, h;
	int as, ret;

	if (!menv_init(&e))
		return;
	vm = vm_new(&e, 0);
	h = bo_new(&e, 8192, 0);
	as = vm_as(&e, vm, NULL);
	REQUIRE(as >= 1);

	op = (struct drm_panthor_vm_bind_op){ .flags = MAP, .bo_handle = h, .va = 0x100000, .size = 8192 };
	b = (struct drm_panthor_vm_bind){ .vm_id = vm, .flags = DRM_PANTHOR_VM_BIND_ASYNC,
		.ops = DRM_PANTHOR_OBJ_ARRAY(1, &op) };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_BIND, &b), PW_OK);
	CHECK_EQ(mock_gpu_translate(e.os, as, 0x101000, 'r', NULL, NULL, NULL), 0);
	/* validation errors fail the whole call before anything runs */
	memset(so, 0, sizeof(so));
	so[0].flags = 7;	/* bad handle type */
	op = (struct drm_panthor_vm_bind_op){ .flags = UNMAP, .va = 0x100000, .size = 8192,
		.syncs = DRM_PANTHOR_OBJ_ARRAY(1, so) };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_BIND, &b), PW_EINVAL);
	so[0].flags = DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ;
	so[0].timeline_value = 3;	/* binary syncobj with a point */
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_BIND, &b), PW_EINVAL);
	so[0] = (struct drm_panthor_sync_op){ .flags = DRM_PANTHOR_SYNC_OP_WAIT, .handle = 999 };
	/* no such syncobj: whatever pw_syncobj_find_fence() says (contract: EINVAL; Linux:
	 * ENOENT, drv.c:633) */
	ret = IOC(&e, DRM_IOCTL_PANTHOR_VM_BIND, &b);
	CHECK(ret == PW_EINVAL || ret == PW_ENOENT);
	op = (struct drm_panthor_vm_bind_op){ .flags = DRM_PANTHOR_VM_BIND_OP_TYPE_SYNC_ONLY };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_BIND, &b), PW_EINVAL);	/* SYNC_ONLY needs syncs */
	CHECK_EQ(mock_gpu_translate(e.os, as, 0x101000, 'r', NULL, NULL, NULL), 0);

	/* with syncobjs */
	sc = (struct drm_syncobj_create){ .flags = DRM_SYNCOBJ_CREATE_SIGNALED };
	ret = IOC(&e, DRM_IOCTL_SYNCOBJ_CREATE, &sc);
	if (ret == PW_ENOTTY) {
		test_skip("pw_sync.c is still a stub: async binds with sync ops not tested");
		menv_fini(&e);
		return;
	}
	REQUIRE(ret == PW_OK);
	{
		struct drm_syncobj_create out = { 0 };
		struct drm_syncobj_wait w;
		uint32_t handles[1];

		CHECK_EQ(IOC(&e, DRM_IOCTL_SYNCOBJ_CREATE, &out), PW_OK);
		so[0] = (struct drm_panthor_sync_op){ .flags = DRM_PANTHOR_SYNC_OP_WAIT |
			DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ, .handle = sc.handle };
		so[1] = (struct drm_panthor_sync_op){ .flags = DRM_PANTHOR_SYNC_OP_SIGNAL |
			DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ, .handle = out.handle };
		op = (struct drm_panthor_vm_bind_op){ .flags = UNMAP, .va = 0x100000, .size = 4096,
			.syncs = DRM_PANTHOR_OBJ_ARRAY(2, so) };
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_BIND, &b), PW_OK);
		CHECK(mock_gpu_translate(e.os, as, 0x100000, 'r', NULL, NULL, NULL) >= 0xc0);
		handles[0] = out.handle;
		w = (struct drm_syncobj_wait){ .handles = (uintptr_t)handles, .count_handles = 1,
			.timeout_nsec = 0 };
		CHECK_EQ(IOC(&e, DRM_IOCTL_SYNCOBJ_WAIT, &w), PW_OK);
	}
	menv_fini(&e);
}

/* Close the file with everything still alive: BOs with offsets, user mappings, VMs with
 * user and kernel mappings, an active AS. Everything must be released. */
static void test_mem_close_with_live_objects(void)
{
	struct menv e;
	struct pw_uvm *uvm;
	struct pw_bo *k;
	struct drm_panthor_bo_mmap_offset mo;
	uint32_t vm1, vm2, h1, h2;
	uint64_t kva;
	int as;

	if (!menv_init(&e))
		return;
	vm1 = vm_new(&e, 0);
	vm2 = vm_new(&e, 1ull << 40);
	h1 = bo_new(&e, 64 * 1024, 0);
	h2 = bo_new(&e, 4096, DRM_PANTHOR_BO_NO_MMAP);
	mo = (struct drm_panthor_bo_mmap_offset){ .handle = h1 };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &mo), PW_OK);
	CHECK_EQ(bind1(&e, vm1, MAP, h1, 0, 0x100000, 64 * 1024), PW_OK);
	CHECK_EQ(bind1(&e, vm2, MAP, h1, 4096, 0x100000, 4096), PW_OK);
	CHECK_EQ(bind1(&e, vm2, MAP, h2, 0, 0x200000, 4096), PW_OK);
	as = vm_as(&e, vm1, &uvm);
	REQUIRE(as >= 1);
	REQUIRE(pw_bo_create_kernel(e.dev, 8192, 0, &k) == PW_OK);
	CHECK_EQ(pw_uvm_map_kernel_bo(uvm, k, 0, &kva), PW_OK);
	pw_bo_put(k);		/* the mapping holds the last ref */
	pw_uvm_put(uvm);
	CHECK(mock_live_mems(e.os) > 0);
	menv_close_file(&e);
	{
		struct mock_as_state st;

		mock_get_as(e.os, (unsigned int)as, &st);
		CHECK(!st.enabled);	/* AS released when the VM went away */
	}
	menv_fini(&e);
}

/* Several threads binding/unbinding in one VM and creating/closing BOs (ASan/TSan food). */
struct stress_arg {
	struct menv *e;
	uint32_t vm;
	int idx;
};

static void *stress_thread(void *p)
{
	struct stress_arg *a = p;

	for (int i = 0; i < 50; i++) {
		uint32_t h = bo_new(a->e, 8192, 0);
		uint64_t va = 0x1000000ull * (uint64_t)(a->idx + 1) + (uint64_t)(i % 4) * 0x10000;
		struct drm_gem_close gc = { .handle = h };
		struct drm_panthor_bo_mmap_offset mo = { .handle = h };
		struct pw_mmap_target t;

		CHECK_EQ(bind1(a->e, a->vm, MAP, h, 0, va, 8192), PW_OK);
		CHECK_EQ(IOC(a->e, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &mo), PW_OK);
		if (pw_file_mmap_lookup(a->e->file, mo.offset, 8192, &t) == PW_OK)
			pw_bo_put(t.bo);
		CHECK_EQ(IOC(a->e, DRM_IOCTL_GEM_CLOSE, &gc), PW_OK);
		if (i & 1)
			CHECK_EQ(bind1(a->e, a->vm, UNMAP, 0, 0, va, 8192), PW_OK);
	}
	return NULL;
}

static void test_mem_threads(void)
{
	struct menv e;
	pthread_t th[4];
	struct stress_arg a[4];
	uint32_t vm;

	if (!menv_init(&e))
		return;
	vm = vm_new(&e, 0);
	REQUIRE(vm_as(&e, vm, NULL) >= 1);
	for (int i = 0; i < 4; i++) {
		a[i] = (struct stress_arg){ &e, vm, i };
		pthread_create(&th[i], NULL, stress_thread, &a[i]);
	}
	for (int i = 0; i < 4; i++)
		pthread_join(th[i], NULL);
	menv_fini(&e);
}

/* ---- added by the core-mem review ---- */

static uint32_t syncobj_new(struct menv *e, bool signalled)
{
	struct drm_syncobj_create c = { .flags = signalled ? DRM_SYNCOBJ_CREATE_SIGNALED : 0 };

	CHECK_EQ(IOC(e, DRM_IOCTL_SYNCOBJ_CREATE, &c), PW_OK);
	return c.handle;
}

/* Timeline point @point of syncobj @h signalled (absolute timeout 0 = poll)? */
static bool tl_signalled(struct menv *e, uint32_t h, uint64_t point)
{
	struct drm_syncobj_timeline_wait w = {
		.handles = (uintptr_t)&h, .points = (uintptr_t)&point, .count_handles = 1,
		.timeout_nsec = 0,
	};

	return IOC(e, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &w) == PW_OK;
}

struct delayed_signal {
	struct pw_fence *f;
	unsigned int ms;
	uint64_t signalled_at_ms;
};

static void *delayed_signal_thread(void *p)
{
	struct delayed_signal *d = p;

	usleep(d->ms * 1000);
	d->signalled_at_ms = now_ms();
	pw_fence_signal(d->f, 0);
	return NULL;
}

/* An async bind whose wait fence is not signalled yet blocks (our deviation from panthor's
 * queued job) and runs the op only once the fence signals; its out-fence is published after. */
static void test_mem_vm_bind_async_blocking(void)
{
	struct menv e;
	struct drm_panthor_vm_bind_op op;
	struct drm_panthor_vm_bind b;
	struct drm_panthor_sync_op so[2];
	struct delayed_signal d;
	struct pw_fence *f;
	pthread_t th;
	uint32_t vm, h, in, out;
	int as;

	if (!menv_init(&e))
		return;
	vm = vm_new(&e, 0);
	h = bo_new(&e, 8192, 0);
	as = vm_as(&e, vm, NULL);
	REQUIRE(as >= 1);
	in = syncobj_new(&e, false);
	out = syncobj_new(&e, false);
	f = pw_fence_create(e.dev, 1);
	REQUIRE(f);
	CHECK_EQ(pw_syncobj_attach_fence(e.file, in, 0, f), PW_OK);

	so[0] = (struct drm_panthor_sync_op){ .flags = DRM_PANTHOR_SYNC_OP_WAIT |
		DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ, .handle = in };
	so[1] = (struct drm_panthor_sync_op){ .flags = DRM_PANTHOR_SYNC_OP_SIGNAL |
		DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ, .handle = out };
	op = (struct drm_panthor_vm_bind_op){ .flags = MAP, .bo_handle = h, .va = 0x100000,
		.size = 8192, .syncs = DRM_PANTHOR_OBJ_ARRAY(2, so) };
	b = (struct drm_panthor_vm_bind){ .vm_id = vm, .flags = DRM_PANTHOR_VM_BIND_ASYNC,
		.ops = DRM_PANTHOR_OBJ_ARRAY(1, &op) };

	d = (struct delayed_signal){ .f = f, .ms = 60 };
	REQUIRE(pthread_create(&th, NULL, delayed_signal_thread, &d) == 0);
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_BIND, &b), PW_OK);
	pthread_join(th, NULL);
	/* the bind returned only after the wait fence was signalled */
	CHECK(d.signalled_at_ms != 0 && now_ms() >= d.signalled_at_ms);
	CHECK_EQ(mock_gpu_translate(e.os, as, 0x101000, 'r', NULL, NULL, NULL), 0);
	CHECK(tl_signalled(&e, out, 0));
	pw_fence_put(f);
	menv_fini(&e);
}

/* panthor resolves every signal syncobj before queueing anything (drv.c:396): a missing one
 * fails the whole call with EINVAL and no op runs. Ops of one call may depend on each other
 * through a (handle, point) one of them signals; a wait on a point only a LATER op signals
 * needs an existing fence (drv.c:629 -> EINVAL here). */
static void test_mem_vm_bind_async_syncs(void)
{
	struct menv e;
	struct drm_panthor_vm_bind_op op[3];
	struct drm_panthor_vm_bind b;
	struct drm_panthor_sync_op s0[2], s1[2], s2[1];
	uint32_t vm, h, tl, bin;
	int as;

	if (!menv_init(&e))
		return;
	vm = vm_new(&e, 0);
	h = bo_new(&e, 3 * 4096, 0);
	as = vm_as(&e, vm, NULL);
	REQUIRE(as >= 1);
	tl = syncobj_new(&e, false);
	bin = syncobj_new(&e, false);

	/* missing signal syncobj: nothing mapped, nothing signalled */
	s0[0] = (struct drm_panthor_sync_op){ .flags = DRM_PANTHOR_SYNC_OP_SIGNAL |
		DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ, .handle = bin };
	s0[1] = (struct drm_panthor_sync_op){ .flags = DRM_PANTHOR_SYNC_OP_SIGNAL |
		DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ, .handle = 999 };
	op[0] = (struct drm_panthor_vm_bind_op){ .flags = MAP, .bo_handle = h, .va = 0x100000,
		.size = 4096, .syncs = DRM_PANTHOR_OBJ_ARRAY(2, s0) };
	b = (struct drm_panthor_vm_bind){ .vm_id = vm, .flags = DRM_PANTHOR_VM_BIND_ASYNC,
		.ops = DRM_PANTHOR_OBJ_ARRAY(1, op) };
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_BIND, &b), PW_EINVAL);
	CHECK(mock_gpu_translate(e.os, as, 0x100000, 'r', NULL, NULL, NULL) >= 0xc0);
	CHECK(!tl_signalled(&e, bin, 0));

	/* op0 signals tl@1, op1 waits tl@1 (same call) and signals tl@2 + bin, op2 waits tl@2 */
	s0[0] = (struct drm_panthor_sync_op){ .flags = DRM_PANTHOR_SYNC_OP_SIGNAL |
		DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ, .handle = tl, .timeline_value = 1 };
	s1[0] = (struct drm_panthor_sync_op){ .flags = DRM_PANTHOR_SYNC_OP_WAIT |
		DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ, .handle = tl, .timeline_value = 1 };
	s1[1] = (struct drm_panthor_sync_op){ .flags = DRM_PANTHOR_SYNC_OP_SIGNAL |
		DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ, .handle = tl, .timeline_value = 2 };
	s2[0] = (struct drm_panthor_sync_op){ .flags = DRM_PANTHOR_SYNC_OP_WAIT |
		DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ, .handle = tl, .timeline_value = 2 };
	op[0] = (struct drm_panthor_vm_bind_op){ .flags = MAP, .bo_handle = h, .va = 0x100000,
		.size = 4096, .syncs = DRM_PANTHOR_OBJ_ARRAY(1, s0) };
	op[1] = (struct drm_panthor_vm_bind_op){ .flags = MAP, .bo_handle = h, .bo_offset = 4096,
		.va = 0x200000, .size = 4096, .syncs = DRM_PANTHOR_OBJ_ARRAY(2, s1) };
	op[2] = (struct drm_panthor_vm_bind_op){ .flags = MAP, .bo_handle = h, .bo_offset = 8192,
		.va = 0x300000, .size = 4096, .syncs = DRM_PANTHOR_OBJ_ARRAY(1, s2) };
	b.ops = (struct drm_panthor_obj_array)DRM_PANTHOR_OBJ_ARRAY(3, op);

	/* order matters: op0 waiting on tl@2 that only op1 signals has no fence -> EINVAL */
	{
		struct drm_panthor_sync_op w2[2] = { s2[0], s0[0] };

		op[0].syncs = (struct drm_panthor_obj_array)DRM_PANTHOR_OBJ_ARRAY(2, w2);
		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_BIND, &b), PW_EINVAL);
		CHECK(mock_gpu_translate(e.os, as, 0x100000, 'r', NULL, NULL, NULL) >= 0xc0);
		op[0].syncs = (struct drm_panthor_obj_array)DRM_PANTHOR_OBJ_ARRAY(1, s0);
	}
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_BIND, &b), PW_OK);
	CHECK_EQ(mock_gpu_translate(e.os, as, 0x100000, 'r', NULL, NULL, NULL), 0);
	CHECK_EQ(mock_gpu_translate(e.os, as, 0x200000, 'r', NULL, NULL, NULL), 0);
	CHECK_EQ(mock_gpu_translate(e.os, as, 0x300000, 'r', NULL, NULL, NULL), 0);
	CHECK(tl_signalled(&e, tl, 1));
	CHECK(tl_signalled(&e, tl, 2));
	{
		struct drm_syncobj_timeline_array q;
		uint64_t pt = 0;

		q = (struct drm_syncobj_timeline_array){ .handles = (uintptr_t)&tl,
			.points = (uintptr_t)&pt, .count_handles = 1 };
		CHECK_EQ(IOC(&e, DRM_IOCTL_SYNCOBJ_QUERY, &q), PW_OK);
		CHECK_EQ(pt, 2);
	}

	/* the same (handle, point) signalled by two ops is published once, with the last op's
	 * fence (panthor's deduplicated signal list) */
	s0[0] = (struct drm_panthor_sync_op){ .flags = DRM_PANTHOR_SYNC_OP_SIGNAL |
		DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ, .handle = tl, .timeline_value = 3 };
	op[0] = (struct drm_panthor_vm_bind_op){ .flags = UNMAP, .va = 0x100000, .size = 4096,
		.syncs = DRM_PANTHOR_OBJ_ARRAY(1, s0) };
	op[1] = (struct drm_panthor_vm_bind_op){ .flags = UNMAP, .va = 0x200000, .size = 4096,
		.syncs = DRM_PANTHOR_OBJ_ARRAY(1, s0) };
	b.ops = (struct drm_panthor_obj_array)DRM_PANTHOR_OBJ_ARRAY(2, op);
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_BIND, &b), PW_OK);
	CHECK(tl_signalled(&e, tl, 3));
	CHECK(mock_gpu_translate(e.os, as, 0x200000, 'r', NULL, NULL, NULL) >= 0xc0);

	/* an empty async batch succeeds even on an unusable VM (no job is created) */
	mock_inject_mmu_fault(e.os, (unsigned int)as, 0x000003c1u, 0x200000);
	CHECK(wait_until(vm_unusable, &e, 2000));
	b.ops = (struct drm_panthor_obj_array)DRM_PANTHOR_OBJ_ARRAY(0, op);
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_BIND, &b), PW_OK);
	b.ops = (struct drm_panthor_obj_array)DRM_PANTHOR_OBJ_ARRAY(1, op);
	CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_BIND, &b), PW_EINVAL);	/* mmu.c:3067 */
	menv_fini(&e);
}

/* VM_DESTROY racing binds/kernel mappings on the same VM, and GEM_CLOSE racing MMAP_OFFSET on
 * the same handle: no crash, no leak, and nothing stays mapped in the destroyed VM. */
struct race_arg {
	struct menv *e;
	uint32_t vm, h;
	struct pw_uvm *uvm;
	volatile int stop;
	int map_ok, map_einval;
};

static void *race_bind_thread(void *p)
{
	struct race_arg *a = p;

	for (int i = 0; !a->stop && i < 2000; i++) {
		int r = bind1(a->e, a->vm, MAP, a->h, 0, 0x400000 + (uint64_t)(i % 8) * 0x10000,
			      8192);

		if (r == PW_OK)
			a->map_ok++;
		else if (r == PW_EINVAL)
			a->map_einval++;
		else
			CHECK_EQ(r, PW_OK);
		if (i & 1)
			bind1(a->e, a->vm, UNMAP, 0, 0, 0x400000 + (uint64_t)(i % 8) * 0x10000, 8192);
		if (a->map_einval > 20)
			break;
	}
	return NULL;
}

static void *race_kmap_thread(void *p)
{
	struct race_arg *a = p;
	struct pw_bo *k;

	if (pw_bo_create_kernel(a->e->dev, 4096, 0, &k))
		return NULL;
	for (int i = 0; !a->stop && i < 500; i++) {
		uint64_t va;

		if (pw_uvm_map_kernel_bo(a->uvm, k, 0, &va) == PW_OK)
			pw_uvm_unmap_kernel_bo(a->uvm, va);
	}
	pw_bo_put(k);
	return NULL;
}

static void *race_mmap_thread(void *p)
{
	struct race_arg *a = p;

	for (int i = 0; i < 300; i++) {
		struct drm_panthor_bo_mmap_offset mo = { .handle = a->h };
		struct pw_mmap_target t;
		int r = IOC(a->e, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &mo);

		if (r == PW_OK && pw_file_mmap_lookup(a->e->file, mo.offset, 4096, &t) == PW_OK)
			pw_bo_put(t.bo);
		else if (r != PW_OK)
			CHECK_EQ(r, PW_ENOENT);
	}
	return NULL;
}

static void test_mem_destroy_races(void)
{
	struct menv e;
	struct race_arg a;
	pthread_t t1, t2, t3;
	struct pw_bo *bo;
	int as;

	if (!menv_init(&e))
		return;
	memset(&a, 0, sizeof(a));
	a.e = &e;
	a.vm = vm_new(&e, 0);
	a.h = bo_new(&e, 8192, 0);
	as = vm_as(&e, a.vm, &a.uvm);
	REQUIRE(as >= 1);
	bo = pw_file_bo_get(e.file, a.h);
	REQUIRE(bo);

	REQUIRE(pthread_create(&t1, NULL, race_bind_thread, &a) == 0);
	REQUIRE(pthread_create(&t2, NULL, race_kmap_thread, &a) == 0);
	usleep(5000);
	{
		struct drm_panthor_vm_destroy d = { .id = a.vm };

		CHECK_EQ(IOC(&e, DRM_IOCTL_PANTHOR_VM_DESTROY, &d), PW_OK);
	}
	pthread_join(t1, NULL);
	a.stop = 1;
	pthread_join(t2, NULL);
	/* destroyed: nothing user- or kernel-mapped any more, binds by id fail */
	CHECK_EQ(bind1(&e, a.vm, MAP, a.h, 0, 0x400000, 8192), PW_EINVAL);
	for (int i = 0; i < 8; i++)
		CHECK(mock_gpu_translate(e.os, as, 0x400000 + (uint64_t)i * 0x10000, 'r', NULL,
					 NULL, NULL) >= 0xc0);
	{
		uint64_t ks, ke;

		pw_uvm_kernel_va_range(a.uvm, &ks, &ke);
		CHECK(mock_gpu_translate(e.os, as, ks, 'r', NULL, NULL, NULL) >= 0xc0);
	}
	/* only our lookup ref + the handle's remain on the BO */
	CHECK_EQ(bo_refs(bo), 2);
	pw_uvm_put(a.uvm);

	/* GEM_CLOSE vs MMAP_OFFSET on the same handle */
	REQUIRE(pthread_create(&t3, NULL, race_mmap_thread, &a) == 0);
	usleep(1000);
	{
		struct drm_gem_close gc = { .handle = a.h };

		CHECK_EQ(IOC(&e, DRM_IOCTL_GEM_CLOSE, &gc), PW_OK);
	}
	pthread_join(t3, NULL);
	CHECK_EQ(bo_refs(bo), 1);		/* offset revoked, handle gone: only ours */
	pw_bo_put(bo);
	menv_fini(&e);
}

const struct test_case mem_tests[] = {
	{ "mem_version_caps", test_mem_version_caps },
	{ "mem_dev_query", test_mem_dev_query },
	{ "mem_ioctl_size_ext", test_mem_ioctl_size_ext },
	{ "mem_obj_array", test_mem_obj_array },
	{ "mem_bo_basic", test_mem_bo_basic },
	{ "mem_user_mmio", test_mem_user_mmio },
	{ "mem_bo_label", test_mem_bo_label },
	{ "mem_bo_wb_sync_query", test_mem_bo_wb_sync_query },
	{ "mem_vm_create", test_mem_vm_create },
	{ "mem_vm_bind_map", test_mem_vm_bind_map },
	{ "mem_vm_bind_errors", test_mem_vm_bind_errors },
	{ "mem_vm_bind_2m_split", test_mem_vm_bind_2m_split },
	{ "mem_kernel_mappings", test_mem_kernel_mappings },
	{ "mem_vm_unusable", test_mem_vm_unusable },
	{ "mem_vm_bind_async", test_mem_vm_bind_async },
	{ "mem_close_with_live_objects", test_mem_close_with_live_objects },
	{ "mem_threads", test_mem_threads },
	{ "mem_vm_bind_async_blocking", test_mem_vm_bind_async_blocking },
	{ "mem_vm_bind_async_syncs", test_mem_vm_bind_async_syncs },
	{ "mem_destroy_races", test_mem_destroy_races },
	{ NULL, NULL },
};
