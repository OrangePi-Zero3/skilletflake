// SPDX-License-Identifier: MIT
/*
 * pw_core_stub.c - trivial implementation of kmd/core/pw_core.h, built with -Dcore=stub so that
 * panwin.sys links and loads without the real core. Useful as an M1 smoke test: it reads GPU_ID
 * and the *_PRESENT/feature registers, keeps all three interrupt lines masked, and never boots
 * firmware. Register offsets are the public Mali CSF layout (see docs/research/kernel-panthor.md).
 *
 * Per-file API (M3+ contract): enough DRM for the bring-up tools - VERSION ("panthor" 1.0.0),
 * GET_CAP, PANTHOR_DEV_QUERY GPU_INFO (CSIF_INFO fails: no firmware) - plus the user MMIO
 * (LATEST_FLUSH_ID) page through pw_file_mmap_lookup(), so the glue's MMIO mapping path can be
 * exercised on hardware without the real core. No BOs, no syncobjs (ENOTTY / EINVAL).
 *
 * (jm-glue) The backend follows GPU_ID like the real core (pw_backend_for_gpu_id). On a
 * job-manager GPU (Bifrost, e.g. the Zero3's Mali-G31) VERSION says "panfrost" 1.0.0, the
 * driver ioctls are the panfrost numbers and only DRM_IOCTL_PANFROST_GET_PARAM is implemented:
 * the raw feature registers, read when asked, with the meanings of Linux panfrost_drv.c
 * panfrost_ioctl_get_param() / panfrost_gpu.c panfrost_gpu_init_features() (behaviour only,
 * no code taken). That is the Z1 register dump without the real JM core; SYSTEM_TIMESTAMP is
 * the raw TIMESTAMP register (the stub never starts the cycle counter, so it may read 0). There
 * is no user-MMIO page on JM. Everything is read-only apart from the interrupt masks.
 * UNTESTED ON HARDWARE.
 */
#include "pw_core.h"
#include "panthor_drm.h"
#include "panfrost_drm.h"
#include "pw_fmt.h"

#define STUB_PAGE_SIZE 4096u
/* panthor_fw_regs.h CSF_GPU_LATEST_FLUSH_ID: the page mapped at DRM_PANTHOR_USER_MMIO_OFFSET
 * (panthor_device.c panthor_mmio_vm_fault). */
#define STUB_LATEST_FLUSH_ID 0x10000u
#define STUB_OFFSETOFEND(type, field) (offsetof(type, field) + sizeof(((type *)0)->field))

#define STUB_GPU_ID           0x000
#define STUB_L2_FEATURES      0x004
#define STUB_CORE_FEATURES    0x008
#define STUB_TILER_FEATURES   0x00C
#define STUB_MEM_FEATURES     0x010
#define STUB_MMU_FEATURES     0x014
#define STUB_AS_PRESENT       0x018
#define STUB_CSF_ID           0x01C
#define STUB_SHADER_PRESENT   0x100 /* LO/HI */
#define STUB_TILER_PRESENT    0x110
#define STUB_L2_PRESENT       0x120
/* Job-manager only (Linux panfrost_regs.h meanings; 0x1C is CSF_ID on CSF). */
#define STUB_JS_PRESENT       0x01C
#define STUB_AFBC_FEATURES    0x04C
#define STUB_TIMESTAMP        0x098 /* LO/HI */
#define STUB_THREAD_MAX_THREADS 0x0A0
#define STUB_THREAD_MAX_WG    0x0A4
#define STUB_THREAD_MAX_BARRIER 0x0A8
#define STUB_THREAD_FEATURES  0x0AC
#define STUB_TEXTURE_FEATURES(n) (0x0B0 + (n) * 4)
#define STUB_JS_FEATURES(n)   (0x0C0 + (n) * 4)
#define STUB_COHERENCY_FEATURES 0x300
#define STUB_THREAD_TLS_ALLOC 0x310
#define STUB_STACK_PRESENT    0xE00 /* LO/HI */
#define STUB_DEFAULT_TIMER_HZ 24000000u

/* Interrupt blocks: RAWSTAT +0x0, CLEAR +0x4, MASK +0x8, STAT +0xC. */
static const uint32_t stub_irq_base[PW_IRQ_COUNT] = {
	[PW_IRQ_JOB] = 0x1000,
	[PW_IRQ_MMU] = 0x2000,
	[PW_IRQ_GPU] = 0x0020,
};

struct pw_device {
	struct pw_os *os;
	bool boot_firmware;
	uint32_t timer_hz;
	uint32_t gpu_id; /* 0 until probed */
	enum pw_backend backend;
	uint32_t irq_count[PW_IRQ_COUNT];
};

/* Same rule as kmd/core pw_gpu.c (pw_core.h, enum pw_backend). */
enum pw_backend pw_backend_for_gpu_id(uint32_t gpu_id)
{
	uint32_t arch = gpu_id >> 28;

	if (arch == 6 || arch == 7)
		return PW_BACKEND_JM;
	if (arch >= 10 && arch <= 13)
		return PW_BACKEND_CSF;
	return PW_BACKEND_NONE;
}

enum pw_backend pw_device_backend(const struct pw_device *dev)
{
	return dev ? dev->backend : PW_BACKEND_NONE;
}

static uint64_t stub_read64(struct pw_os *os, uint32_t reg)
{
	return pw_os_reg_read32(os, reg) | ((uint64_t)pw_os_reg_read32(os, reg + 4) << 32);
}

static void stub_mask_all(struct pw_os *os)
{
	for (int i = 0; i < PW_IRQ_COUNT; i++) {
		pw_os_reg_write32(os, stub_irq_base[i] + 0x8, 0);
		pw_os_reg_write32(os, stub_irq_base[i] + 0x4, 0xFFFFFFFFu);
	}
}

int pw_device_alloc(const struct pw_device_params *params, struct pw_device **out)
{
	struct pw_device *dev = pw_os_zalloc(params->os, sizeof(*dev));

	*out = NULL;
	if (!dev)
		return PW_ENOMEM;
	dev->os = params->os;
	dev->boot_firmware = params->boot_firmware;
	dev->timer_hz = params->timer_hz ? params->timer_hz : STUB_DEFAULT_TIMER_HZ;
	*out = dev;
	return PW_OK;
}

int pw_device_probe(struct pw_device *dev)
{
	struct pw_os *os = dev->os;
	uint32_t id;

	id = pw_os_reg_read32(os, STUB_GPU_ID);
	pw_os_log(os, PW_LOG_INFO,
		  "core stub: GPU_ID 0x%08x (arch %u.%u rev %u, product major %u, r%up%u)", id,
		  id >> 28, (id >> 24) & 0xF, (id >> 20) & 0xF, (id >> 16) & 0xF, (id >> 12) & 0xF,
		  (id >> 4) & 0xFF);
	if (id == 0 || id == 0xFFFFFFFFu) {
		pw_os_log(os, PW_LOG_ERR, "core stub: GPU not responding (power domain/clock off?)");
		return PW_EIO;
	}
	dev->backend = pw_backend_for_gpu_id(id);
	if (dev->boot_firmware && dev->backend != PW_BACKEND_JM)
		pw_os_log(os, PW_LOG_WARN, "core stub: BootFirmware ignored (built with core=stub)");
	if (dev->backend == PW_BACKEND_NONE)
		pw_os_log(os, PW_LOG_WARN, "core stub: arch %u is neither Bifrost (JM) nor Valhall "
			  "CSF; VERSION reports panthor", id >> 28);
	dev->gpu_id = id;
	stub_mask_all(os);
	return PW_OK;
}

int pw_device_create(const struct pw_device_params *params, struct pw_device **out)
{
	int ret = pw_device_alloc(params, out);

	if (ret)
		return ret;
	ret = pw_device_probe(*out);
	if (ret) {
		pw_device_destroy(*out);
		*out = NULL;
	}
	return ret;
}

void pw_device_destroy(struct pw_device *dev)
{
	if (!dev)
		return;
	if (dev->gpu_id)
		stub_mask_all(dev->os);
	pw_os_free(dev->os, dev);
}

bool pw_irq_top(struct pw_device *dev, enum pw_irq_line line)
{
	uint32_t base;

	if (!dev || !dev->gpu_id || (unsigned int)line >= PW_IRQ_COUNT)
		return false;
	base = stub_irq_base[line];
	if (!pw_os_reg_read32(dev->os, base + 0xC))
		return false;
	pw_os_reg_write32(dev->os, base + 0x8, 0); /* stays masked: the stub handles nothing */
	return true;
}

void pw_irq_bottom(struct pw_device *dev, enum pw_irq_line line)
{
	uint32_t base = stub_irq_base[line];
	uint32_t raw = pw_os_reg_read32(dev->os, base + 0x0);

	pw_os_reg_write32(dev->os, base + 0x4, raw);
	dev->irq_count[line]++;
	pw_os_log(dev->os, PW_LOG_DBG, "core stub: irq line %d rawstat 0x%08x (left masked)",
		  (int)line, raw);
}

void pw_device_get_gpu_info(struct pw_device *dev, struct drm_panthor_gpu_info *info)
{
	struct pw_os *os = dev->os;

	*info = (struct drm_panthor_gpu_info){ 0 };
	info->gpu_id = dev->gpu_id;
	info->csf_id = dev->backend == PW_BACKEND_JM ? 0 : pw_os_reg_read32(os, STUB_CSF_ID);
	info->l2_features = pw_os_reg_read32(os, STUB_L2_FEATURES);
	info->core_features = pw_os_reg_read32(os, STUB_CORE_FEATURES);
	info->tiler_features = pw_os_reg_read32(os, STUB_TILER_FEATURES);
	info->mem_features = pw_os_reg_read32(os, STUB_MEM_FEATURES);
	info->mmu_features = pw_os_reg_read32(os, STUB_MMU_FEATURES);
	info->as_present = pw_os_reg_read32(os, STUB_AS_PRESENT);
	info->shader_present = stub_read64(os, STUB_SHADER_PRESENT);
	info->tiler_present = stub_read64(os, STUB_TILER_PRESENT);
	info->l2_present = stub_read64(os, STUB_L2_PRESENT);
}

int pw_device_get_csif_info(struct pw_device *dev, struct drm_panthor_csif_info *info)
{
	(void)dev;
	*info = (struct drm_panthor_csif_info){ 0 };
	return PW_ENOSYS; /* needs the firmware interface */
}

bool pw_device_fw_booted(struct pw_device *dev)
{
	(void)dev;
	return false;
}

int pw_device_fw_ping(struct pw_device *dev, uint32_t timeout_ms)
{
	(void)dev;
	(void)timeout_ms;
	return PW_EINVAL; /* no firmware */
}

void pw_device_get_mmu_info(struct pw_device *dev, struct drm_panthor_mmu_info *info)
{
	(void)dev;
	info->page_size_bitmap = (1ull << 12) | (1ull << 21); /* 4K | 2M, as panthor reports */
}

void pw_device_describe(struct pw_device *dev, char *buf, size_t len)
{
	pw_win_snprintf(buf, len, "core stub: GPU_ID 0x%08x arch %u.%u, %s", dev->gpu_id,
		    dev->gpu_id >> 28, (dev->gpu_id >> 24) & 0xF,
		    dev->backend == PW_BACKEND_JM ? "job manager (GET_PARAM only)" : "no firmware");
}

/* ------------------------------------------------------------- per-file API (stub) */

struct pw_file {
	struct pw_device *dev;
};

void pw_core_drm_version(uint32_t *major, uint32_t *minor, uint32_t *patch)
{
	*major = 1;
	*minor = 0;
	*patch = 0;
}

void pw_device_drm_version(const struct pw_device *dev, uint32_t *major, uint32_t *minor,
			   uint32_t *patch, const char **name)
{
	pw_core_drm_version(major, minor, patch);
	if (name)
		*name = (dev && dev->backend == PW_BACKEND_JM) ? "panfrost" : "panthor";
}

int pw_file_open(struct pw_device *dev, struct pw_file **out)
{
	struct pw_file *f = pw_os_zalloc(dev->os, sizeof(*f));

	*out = NULL;
	if (!f)
		return PW_ENOMEM;
	f->dev = dev;
	*out = f;
	return PW_OK;
}

void pw_file_close(struct pw_file *file)
{
	if (file)
		pw_os_free(file->dev->os, file);
}

/* drm_copy_field(): copy min(len, strlen) bytes (no NUL), report the full length. */
static int stub_copy_field(struct pw_os *os, uint64_t ubuf, __kernel_size_t *ulen, const char *v)
{
	size_t len = 0, n;

	while (v[len])
		len++;
	n = len < *ulen ? len : *ulen;
	*ulen = len;
	return (n && ubuf) ? pw_os_copy_to_user(os, ubuf, v, n) : PW_OK;
}

/* panthor_set_uobj(): user size >= minimal size; copy min(user, kernel), zero the rest. */
static int stub_set_uobj(struct pw_os *os, uint64_t uptr, uint32_t usize, size_t min_size,
			 const void *src, size_t ksize)
{
	static const uint8_t zeros[64];
	int ret;

	if (usize < min_size)
		return PW_EINVAL;
	ret = pw_os_copy_to_user(os, uptr, src, usize < ksize ? usize : ksize);
	for (size_t off = ksize; !ret && off < usize; off += sizeof(zeros)) {
		size_t n = usize - off < sizeof(zeros) ? usize - off : sizeof(zeros);

		ret = pw_os_copy_to_user(os, uptr + off, zeros, n);
	}
	return ret;
}

static int stub_dev_query(struct pw_file *file, struct drm_panthor_dev_query *q)
{
	struct drm_panthor_gpu_info info;

	switch (q->type) {
	case DRM_PANTHOR_DEV_QUERY_GPU_INFO:
		if (!q->pointer) {
			q->size = sizeof(info);
			return PW_OK;
		}
		pw_device_get_gpu_info(file->dev, &info);
		return stub_set_uobj(file->dev->os, q->pointer, q->size,
				     STUB_OFFSETOFEND(struct drm_panthor_gpu_info, tiler_present),
				     &info, sizeof(info));
	case DRM_PANTHOR_DEV_QUERY_CSIF_INFO:
		if (!q->pointer) {
			q->size = sizeof(struct drm_panthor_csif_info);
			return PW_OK;
		}
		return PW_ENOSYS; /* needs the firmware interface */
	default:
		return PW_EINVAL;
	}
}

static uint32_t stub_popcount64(uint64_t v)
{
	uint32_t n = 0;

	for (; v; v &= v - 1)
		n++;
	return n;
}

/* DRM_IOCTL_PANFROST_GET_PARAM on the raw registers (see the file comment). */
static int stub_get_param(struct pw_device *dev, struct drm_panfrost_get_param *p)
{
	struct pw_os *os = dev->os;
	uint32_t prm = p->param;

	if (p->pad)
		return PW_EINVAL;
	if (prm >= DRM_PANFROST_PARAM_TEXTURE_FEATURES0 && prm <= DRM_PANFROST_PARAM_TEXTURE_FEATURES3) {
		p->value = pw_os_reg_read32(os, STUB_TEXTURE_FEATURES(prm -
						DRM_PANFROST_PARAM_TEXTURE_FEATURES0));
		return PW_OK;
	}
	if (prm >= DRM_PANFROST_PARAM_JS_FEATURES0 && prm <= DRM_PANFROST_PARAM_JS_FEATURES15) {
		uint32_t js = prm - DRM_PANFROST_PARAM_JS_FEATURES0;
		uint32_t nr_js = stub_popcount64(pw_os_reg_read32(os, STUB_JS_PRESENT));

		/* panfrost_gpu_init_features: only the first popcount(JS_PRESENT) are read. */
		p->value = js < nr_js ? pw_os_reg_read32(os, STUB_JS_FEATURES(js)) : 0;
		return PW_OK;
	}
	switch (prm) {
	case DRM_PANFROST_PARAM_GPU_PROD_ID:
		p->value = dev->gpu_id >> 16;
		return PW_OK;
	case DRM_PANFROST_PARAM_GPU_REVISION:
		p->value = dev->gpu_id & 0xFFFFu;
		return PW_OK;
	case DRM_PANFROST_PARAM_SHADER_PRESENT:
		p->value = stub_read64(os, STUB_SHADER_PRESENT);
		return PW_OK;
	case DRM_PANFROST_PARAM_TILER_PRESENT:
		p->value = stub_read64(os, STUB_TILER_PRESENT);
		return PW_OK;
	case DRM_PANFROST_PARAM_L2_PRESENT:
		p->value = stub_read64(os, STUB_L2_PRESENT);
		return PW_OK;
	case DRM_PANFROST_PARAM_STACK_PRESENT:
		p->value = stub_read64(os, STUB_STACK_PRESENT);
		return PW_OK;
	case DRM_PANFROST_PARAM_AS_PRESENT:
		p->value = pw_os_reg_read32(os, STUB_AS_PRESENT);
		return PW_OK;
	case DRM_PANFROST_PARAM_JS_PRESENT:
		p->value = pw_os_reg_read32(os, STUB_JS_PRESENT);
		return PW_OK;
	case DRM_PANFROST_PARAM_L2_FEATURES:
		p->value = pw_os_reg_read32(os, STUB_L2_FEATURES);
		return PW_OK;
	case DRM_PANFROST_PARAM_CORE_FEATURES:
		p->value = pw_os_reg_read32(os, STUB_CORE_FEATURES);
		return PW_OK;
	case DRM_PANFROST_PARAM_TILER_FEATURES:
		p->value = pw_os_reg_read32(os, STUB_TILER_FEATURES);
		return PW_OK;
	case DRM_PANFROST_PARAM_MEM_FEATURES:
		p->value = pw_os_reg_read32(os, STUB_MEM_FEATURES);
		return PW_OK;
	case DRM_PANFROST_PARAM_MMU_FEATURES:
		p->value = pw_os_reg_read32(os, STUB_MMU_FEATURES);
		return PW_OK;
	case DRM_PANFROST_PARAM_THREAD_FEATURES:
		p->value = pw_os_reg_read32(os, STUB_THREAD_FEATURES);
		return PW_OK;
	case DRM_PANFROST_PARAM_MAX_THREADS:
		p->value = pw_os_reg_read32(os, STUB_THREAD_MAX_THREADS);
		return PW_OK;
	case DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ:
		p->value = pw_os_reg_read32(os, STUB_THREAD_MAX_WG);
		return PW_OK;
	case DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ:
		p->value = pw_os_reg_read32(os, STUB_THREAD_MAX_BARRIER);
		return PW_OK;
	case DRM_PANFROST_PARAM_COHERENCY_FEATURES:
		p->value = pw_os_reg_read32(os, STUB_COHERENCY_FEATURES);
		return PW_OK;
	case DRM_PANFROST_PARAM_NR_CORE_GROUPS:
		p->value = stub_popcount64(stub_read64(os, STUB_L2_PRESENT));
		return PW_OK;
	case DRM_PANFROST_PARAM_THREAD_TLS_ALLOC:
		p->value = pw_os_reg_read32(os, STUB_THREAD_TLS_ALLOC);
		return PW_OK;
	case DRM_PANFROST_PARAM_AFBC_FEATURES:
		p->value = pw_os_reg_read32(os, STUB_AFBC_FEATURES);
		return PW_OK;
	case DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP:
		p->value = stub_read64(os, STUB_TIMESTAMP);
		return PW_OK;
	case DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP_FREQUENCY:
		p->value = dev->timer_hz;
		return PW_OK;
	case DRM_PANFROST_PARAM_ALLOWED_JM_CTX_PRIORITIES:
		p->value = (1u << PANFROST_JM_CTX_PRIORITY_LOW) |
			   (1u << PANFROST_JM_CTX_PRIORITY_MEDIUM);
		return PW_OK;
	case DRM_PANFROST_PARAM_SELECTED_COHERENCY:
		p->value = DRM_PANFROST_GPU_COHERENCY_NONE; /* _CCA 0 on both boards */
		return PW_OK;
	default:
		return PW_EINVAL;
	}
}

int pw_file_ioctl(struct pw_file *file, uint32_t nr, void *arg, uint32_t size)
{
	struct pw_os *os = file->dev->os;
	bool jm = file->dev->backend == PW_BACKEND_JM;

	if (nr == _IOC_NR(DRM_IOCTL_VERSION)) {
		struct drm_version *v = arg;
		int ret;

		if (size != sizeof(*v))
			return PW_EINVAL;
		v->version_major = 1;
		v->version_minor = 0;
		v->version_patchlevel = 0;
		ret = stub_copy_field(os, (uint64_t)(uintptr_t)v->name, &v->name_len,
				      jm ? "panfrost" : "panthor");
		if (!ret)
			ret = stub_copy_field(os, (uint64_t)(uintptr_t)v->date, &v->date_len, "0");
		if (!ret)
			ret = stub_copy_field(os, (uint64_t)(uintptr_t)v->desc, &v->desc_len,
					      jm ? "panfrost DRM (panfrost-win core stub)" :
						   "Panthor DRM driver (panfrost-win core stub)");
		return ret;
	}
	if (jm) {
		if (nr == _IOC_NR(DRM_IOCTL_PANFROST_GET_PARAM)) {
			if (size != sizeof(struct drm_panfrost_get_param))
				return PW_EINVAL;
			return stub_get_param(file->dev, arg);
		}
		if (nr != _IOC_NR(DRM_IOCTL_GET_CAP))
			return PW_ENOTTY;
	}
	if (nr == _IOC_NR(DRM_IOCTL_GET_CAP)) {
		struct drm_get_cap *c = arg;

		if (size != sizeof(*c))
			return PW_EINVAL;
		/* drm_getcap() for a render-only driver without PRIME or syncobjs. */
		switch (c->capability) {
		case DRM_CAP_TIMESTAMP_MONOTONIC:
			c->value = 1;
			return PW_OK;
		case DRM_CAP_PRIME:
		case DRM_CAP_SYNCOBJ:
		case DRM_CAP_SYNCOBJ_TIMELINE:
			c->value = 0;
			return PW_OK;
		default:
			c->value = 0;
			return PW_EOPNOTSUPP;
		}
	}
	if (nr == _IOC_NR(DRM_IOCTL_PANTHOR_DEV_QUERY)) {
		if (size != sizeof(struct drm_panthor_dev_query))
			return PW_EINVAL;
		return stub_dev_query(file, arg);
	}
	return PW_ENOTTY;
}

int pw_file_mmap_lookup(struct pw_file *file, uint64_t offset, uint64_t size,
			struct pw_mmap_target *t)
{
	if (file->dev->backend == PW_BACKEND_JM)
		return PW_EINVAL; /* no user-MMIO page on JM, and no BOs in the stub */
	if (offset != DRM_PANTHOR_USER_MMIO_OFFSET_32BIT &&
	    offset != DRM_PANTHOR_USER_MMIO_OFFSET_64BIT)
		return PW_EINVAL; /* no BOs in the stub */
	if (size != STUB_PAGE_SIZE)
		return PW_EINVAL;
	*t = (struct pw_mmap_target){
		.kind = PW_MMAP_MMIO,
		.mmio_offset = STUB_LATEST_FLUSH_ID,
		.read_only = true,
		.cached = false,
	};
	return PW_OK;
}

struct pw_os_mem *pw_bo_os_mem(struct pw_bo *bo)
{
	(void)bo;
	return NULL;
}

void pw_bo_put(struct pw_bo *bo)
{
	(void)bo;
}

int pw_file_syncobj_notify(struct pw_file *file, uint32_t handle, uint64_t point, uint32_t flags,
			   pw_notify_fn fn, void *ctx, void **cookie)
{
	(void)file;
	(void)handle;
	(void)point;
	(void)flags;
	(void)fn;
	(void)ctx;
	*cookie = NULL;
	return PW_EINVAL; /* no syncobjs: "no such syncobj" */
}

bool pw_file_syncobj_notify_cancel(struct pw_file *file, void *cookie)
{
	(void)file;
	(void)cookie;
	return false;
}
