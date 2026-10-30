// SPDX-License-Identifier: MIT
/*
 * pw_gpu.c - GPU_CONTROL block: identification, soft reset, L2 power, cache flush, GPU IRQ.
 *
 * Derived from Linux panthor (dual GPL-2.0/MIT, used under MIT):
 *   panthor_hw.c   (GPU_ID / feature registers, model names; Copyright 2025 ARM Limited)
 *   panthor_gpu.c  (reset, L2, flush, IRQ; Copyright 2018 Marty E. Plummer,
 *                   2019 Linaro Ltd / Rob Herring, 2019 Collabora ltd.)
 *   panthor_device.c:330-400 (exception names)
 */

#include "pw_priv.h"

/* gpu.c:51-58 */
#define GPU_INTERRUPTS_MASK \
	(GPU_IRQ_FAULT | GPU_IRQ_PROTM_FAULT | GPU_IRQ_RESET_COMPLETED | \
	 GPU_IRQ_CLEAN_CACHES_COMPLETED)
/* (jm-arch) JM: no protected-mode fault bit; MULTIPLE_FAULT instead (the error set of the Mali
 * JM GPU_INT block). POWER_CHANGED stays masked as on CSF. */
#define GPU_INTERRUPTS_MASK_JM \
	(GPU_IRQ_FAULT | GPU_IRQ_MULTIPLE_FAULT | GPU_IRQ_RESET_COMPLETED | \
	 GPU_IRQ_CLEAN_CACHES_COMPLETED)

#define PW_GPU_RESET_TIMEOUT_MS		100	/* gpu.c:419 */
#define PW_GPU_FLUSH_TIMEOUT_MS		100	/* gpu.c:363 */
#define PW_L2_POWER_TIMEOUT_US		20000	/* gpu.c:294,322 */

#define GPU_PROD_ID_MAKE(arch_major, prod_major) (((arch_major) << 24) | (prod_major))

/* (jm-arch) Bifrost names: Mesa src/panfrost/model/pan_model.c (MIT), PAN_PROD_ID(arch major,
 * arch minor, product major). Arch major + product major is unique among Bifrost parts. */
static const char *pw_gpu_bifrost_name(uint32_t gpu_id)
{
	switch (GPU_PROD_ID_MAKE(GPU_ARCH_MAJOR(gpu_id), GPU_PROD_MAJOR(gpu_id))) {
	case GPU_PROD_ID_MAKE(6, 0):
		return "Mali-G71";
	case GPU_PROD_ID_MAKE(6, 1):
		return "Mali-G72";
	case GPU_PROD_ID_MAKE(7, 0):
		return "Mali-G51";
	case GPU_PROD_ID_MAKE(7, 1):
		return "Mali-G76";
	case GPU_PROD_ID_MAKE(7, 2):
		return "Mali-G52";
	case GPU_PROD_ID_MAKE(7, 3):
		return "Mali-G31";
	}
	return NULL;
}

/* panthor_hw.c:116-176 */
const char *pw_gpu_model_name(const struct drm_panthor_gpu_info *info)
{
	const uint32_t product_id = GPU_PROD_ID_MAKE(GPU_ARCH_MAJOR(info->gpu_id),
						     GPU_PROD_MAJOR(info->gpu_id));
	const bool ray_intersection = !!(info->gpu_features & GPU_FEATURES_RAY_INTERSECTION);
	const unsigned int cores = pw_hweight64(info->shader_present);
	const char *bifrost = pw_gpu_bifrost_name(info->gpu_id);

	if (bifrost)
		return bifrost;
	switch (product_id) {
	case GPU_PROD_ID_MAKE(10, 2):
		return "Mali-G710";
	case GPU_PROD_ID_MAKE(10, 3):
		return "Mali-G510";
	case GPU_PROD_ID_MAKE(10, 4):
		return "Mali-G310";
	case GPU_PROD_ID_MAKE(10, 7):
		return "Mali-G610";
	case GPU_PROD_ID_MAKE(11, 2):
		if (cores >= 10 && ray_intersection)
			return "Mali-G715-Immortalis";
		else if (cores >= 7)
			return "Mali-G715";
		return "Mali-G615";
	case GPU_PROD_ID_MAKE(11, 3):
		return "Mali-G615";
	case GPU_PROD_ID_MAKE(12, 0):
		if (cores >= 10 && ray_intersection)
			return "Mali-G720-Immortalis";
		else if (cores >= 6)
			return "Mali-G720";
		return "Mali-G620";
	case GPU_PROD_ID_MAKE(12, 1):
		return "Mali-G620";
	case GPU_PROD_ID_MAKE(13, 0):
		if (cores >= 10 && ray_intersection)
			return "Mali-G925-Immortalis";
		else if (cores >= 6)
			return "Mali-G725";
		return "Mali-G625";
	case GPU_PROD_ID_MAKE(13, 1):
		return "Mali-G625";
	case GPU_PROD_ID_MAKE(14, 0):
		return "Mali-G1-Ultra";
	case GPU_PROD_ID_MAKE(14, 1):
		return "Mali-G1-Premium";
	case GPU_PROD_ID_MAKE(14, 3):
		return "Mali-G1-Pro";
	}
	return "(Unknown Mali GPU)";
}

struct pw_exc_name {
	uint8_t code;
	const char *name;
};

static const struct pw_exc_name pw_exc_names[] = {
	{ PW_EXC_OK, "OK" },
	{ PW_EXC_TERMINATED, "TERMINATED" },
	{ PW_EXC_KABOOM, "KABOOM" },
	{ PW_EXC_EUREKA, "EUREKA" },
	{ PW_EXC_ACTIVE, "ACTIVE" },
	{ PW_EXC_CS_RES_TERM, "CS_RES_TERM" },
	{ PW_EXC_CS_CONFIG_FAULT, "CS_CONFIG_FAULT" },
	{ PW_EXC_CS_UNRECOVERABLE, "CS_UNRECOVERABLE" },
	{ PW_EXC_CS_ENDPOINT_FAULT, "CS_ENDPOINT_FAULT" },
	{ PW_EXC_CS_BUS_FAULT, "CS_BUS_FAULT" },
	{ PW_EXC_CS_INSTR_INVALID, "CS_INSTR_INVALID" },
	{ PW_EXC_CS_CALL_STACK_OVERFLOW, "CS_CALL_STACK_OVERFLOW" },
	{ PW_EXC_CS_INHERIT_FAULT, "CS_INHERIT_FAULT" },
	{ PW_EXC_INSTR_INVALID_PC, "INSTR_INVALID_PC" },
	{ PW_EXC_INSTR_INVALID_ENC, "INSTR_INVALID_ENC" },
	{ PW_EXC_INSTR_BARRIER_FAULT, "INSTR_BARRIER_FAULT" },
	{ PW_EXC_DATA_INVALID_FAULT, "DATA_INVALID_FAULT" },
	{ PW_EXC_TILE_RANGE_FAULT, "TILE_RANGE_FAULT" },
	{ PW_EXC_ADDR_RANGE_FAULT, "ADDR_RANGE_FAULT" },
	{ PW_EXC_IMPRECISE_FAULT, "IMPRECISE_FAULT" },
	{ PW_EXC_OOM, "OOM" },
	{ PW_EXC_CSF_FW_INTERNAL_ERROR, "CSF_FW_INTERNAL_ERROR" },
	{ PW_EXC_CSF_RES_EVICTION_TIMEOUT, "CSF_RES_EVICTION_TIMEOUT" },
	{ PW_EXC_GPU_BUS_FAULT, "GPU_BUS_FAULT" },
	{ PW_EXC_GPU_SHAREABILITY_FAULT, "GPU_SHAREABILITY_FAULT" },
	{ PW_EXC_SYS_SHAREABILITY_FAULT, "SYS_SHAREABILITY_FAULT" },
	{ PW_EXC_GPU_CACHEABILITY_FAULT, "GPU_CACHEABILITY_FAULT" },
	{ PW_EXC_TRANSLATION_FAULT_0, "TRANSLATION_FAULT_0" },
	{ PW_EXC_TRANSLATION_FAULT_1, "TRANSLATION_FAULT_1" },
	{ PW_EXC_TRANSLATION_FAULT_2, "TRANSLATION_FAULT_2" },
	{ PW_EXC_TRANSLATION_FAULT_3, "TRANSLATION_FAULT_3" },
	{ PW_EXC_TRANSLATION_FAULT_4, "TRANSLATION_FAULT_4" },
	{ PW_EXC_PERM_FAULT_0, "PERM_FAULT_0" },
	{ PW_EXC_PERM_FAULT_1, "PERM_FAULT_1" },
	{ PW_EXC_PERM_FAULT_2, "PERM_FAULT_2" },
	{ PW_EXC_PERM_FAULT_3, "PERM_FAULT_3" },
	{ PW_EXC_ACCESS_FLAG_1, "ACCESS_FLAG_1" },
	{ PW_EXC_ACCESS_FLAG_2, "ACCESS_FLAG_2" },
	{ PW_EXC_ACCESS_FLAG_3, "ACCESS_FLAG_3" },
	{ PW_EXC_ADDR_SIZE_FAULT_IN, "ADDR_SIZE_FAULT_IN" },
	{ PW_EXC_ADDR_SIZE_FAULT_OUT0, "ADDR_SIZE_FAULT_OUT0" },
	{ PW_EXC_ADDR_SIZE_FAULT_OUT1, "ADDR_SIZE_FAULT_OUT1" },
	{ PW_EXC_ADDR_SIZE_FAULT_OUT2, "ADDR_SIZE_FAULT_OUT2" },
	{ PW_EXC_ADDR_SIZE_FAULT_OUT3, "ADDR_SIZE_FAULT_OUT3" },
	{ PW_EXC_MEM_ATTR_FAULT_0, "MEM_ATTR_FAULT_0" },
	{ PW_EXC_MEM_ATTR_FAULT_1, "MEM_ATTR_FAULT_1" },
	{ PW_EXC_MEM_ATTR_FAULT_2, "MEM_ATTR_FAULT_2" },
	{ PW_EXC_MEM_ATTR_FAULT_3, "MEM_ATTR_FAULT_3" },
};

const char *pw_exception_name(uint32_t code)
{
	for (size_t i = 0; i < PW_ARRAY_SIZE(pw_exc_names); i++) {
		if (pw_exc_names[i].code == code)
			return pw_exc_names[i].name;
	}
	return "Unknown exception type";
}

/* panthor_hw.c:197-236 (panthor_gpu_info_init), arch < 14 path (no PWR_CONTROL block). */
static void pw_gpu_read_info(struct pw_device *dev)
{
	struct drm_panthor_gpu_info *info = &dev->gpu_info;

	/* (jm-arch) 0x1C is JS_PRESENT on JM: the JM modules read it themselves. */
	info->csf_id = dev->backend == PW_BACKEND_CSF ? gpu_read(dev, GPU_CSF_ID) : 0;
	info->gpu_rev = gpu_read(dev, GPU_REVID);
	info->core_features = gpu_read(dev, GPU_CORE_FEATURES);
	info->l2_features = gpu_read(dev, GPU_L2_FEATURES);
	info->tiler_features = gpu_read(dev, GPU_TILER_FEATURES);
	info->mem_features = gpu_read(dev, GPU_MEM_FEATURES);
	info->mmu_features = gpu_read(dev, GPU_MMU_FEATURES);
	info->thread_features = gpu_read(dev, GPU_THREAD_FEATURES);
	info->max_threads = gpu_read(dev, GPU_THREAD_MAX_THREADS);
	info->thread_max_workgroup_size = gpu_read(dev, GPU_THREAD_MAX_WORKGROUP_SIZE);
	info->thread_max_barrier_size = gpu_read(dev, GPU_THREAD_MAX_BARRIER_SIZE);
	info->coherency_features = gpu_read(dev, GPU_COHERENCY_FEATURES);
	for (unsigned int i = 0; i < 4; i++)
		info->texture_features[i] = gpu_read(dev, GPU_TEXTURE_FEATURES(i));
	info->as_present = gpu_read(dev, GPU_AS_PRESENT);
	/* "Introduced in arch 11.x"; panthor reads it unconditionally (hw.c:221). */
	/* (jm-arch) 0x60 is the perf-counter base on JM: CSF only. */
	info->gpu_features = dev->backend == PW_BACKEND_CSF ? gpu_read64(dev, GPU_FEATURES) : 0;
	info->shader_present = gpu_read64(dev, GPU_SHADER_PRESENT);
	info->tiler_present = gpu_read64(dev, GPU_TILER_PRESENT);
	info->l2_present = gpu_read64(dev, GPU_L2_PRESENT);

	/* panthor_gpu_coherency_init() (gpu.c:486): RK3588 is not dma-coherent (no `dma-coherent`
	 * in rk3588-base.dtsi, _CCA 0 in edk2-rk3588), so we always select NONE. The H616 (Zero3)
	 * is not either (_CCA 0 in firmware/zero3), same register and value on JM. */
	dev->coherent = false;
	info->selected_coherency = GPU_COHERENCY_NONE;
}

/* (jm-arch) Backend selection, see enum pw_backend in pw_core.h. */
enum pw_backend pw_backend_for_gpu_id(uint32_t gpu_id)
{
	uint32_t arch = GPU_ARCH_MAJOR(gpu_id);

	if (gpu_id == 0 || gpu_id == 0xffffffffu)
		return PW_BACKEND_NONE;
	if (arch == 6 || arch == 7)
		return PW_BACKEND_JM;
	if (arch >= 10 && arch <= 13)
		return PW_BACKEND_CSF;
	return PW_BACKEND_NONE;
}

int pw_gpu_init(struct pw_device *dev)
{
	struct drm_panthor_gpu_info *info = &dev->gpu_info;
	uint32_t id, arch;
	int ret;

	if (!dev->gpu.reqs_lock)
		dev->gpu.reqs_lock = pw_os_mutex_create(dev->os);
	if (!dev->gpu.flush_lock)
		dev->gpu.flush_lock = pw_os_mutex_create(dev->os);
	if (!dev->gpu.reqs_event)
		dev->gpu.reqs_event = pw_os_event_create(dev->os);
	if (!dev->gpu.reqs_lock || !dev->gpu.flush_lock || !dev->gpu.reqs_event)
		return PW_ENOMEM;

	/* hw.c:279: GPU_ID == 0 means the block is not there. All-ones is what an unpowered or
	 * unclocked AXI slave often returns on Rockchip (if it doesn't SError outright). */
	id = gpu_read(dev, GPU_ID);
	if (id == 0 || id == 0xffffffffu) {
		pw_err(dev, "GPU_ID reads 0x%08x: GPU not powered/clocked?", id);
		return PW_EIO;
	}
	info->gpu_id = id;

	/* The OS may already have connected all three lines, and the JOB/MMU lines are only set up
	 * later (or never, in M1 mode). Mask them now so state left by a previous owner (UEFI, a
	 * crashed driver instance) cannot make an unclaimed level-triggered line storm. */
	gpu_write(dev, JOB_INT_BASE + INT_MASK, 0);
	gpu_write(dev, MMU_INT_BASE + INT_MASK, 0);

	/* hw.c:31-58: arch 10-13 use GPU_CONTROL power paths; 14+ needs PWR_CONTROL (not done).
	 * (jm-arch) Bifrost (6, 7) is the job-manager backend. */
	arch = GPU_ARCH_MAJOR(id);
	dev->backend = pw_backend_for_gpu_id(id);
	if (dev->backend == PW_BACKEND_NONE) {
		pw_err(dev, "unsupported GPU arch %u (GPU_ID 0x%08x)", arch, id);
		return PW_EOPNOTSUPP;
	}

	pw_gpu_read_info(dev);

	pw_info(dev, "%s id 0x%x major 0x%x minor 0x%x status 0x%x", pw_gpu_model_name(info),
		id >> 16, GPU_VER_MAJOR(id), GPU_VER_MINOR(id), GPU_VER_STATUS(id));
	pw_info(dev, "Features: L2:%#x Tiler:%#x Mem:%#x MMU:%#x AS:%#x", info->l2_features,
		info->tiler_features, info->mem_features, info->mmu_features, info->as_present);
	pw_info(dev, "shader_present=0x%llx l2_present=0x%llx tiler_present=0x%llx",
		(unsigned long long)info->shader_present, (unsigned long long)info->l2_present,
		(unsigned long long)info->tiler_present);

	ret = pw_irq_init(dev, PW_IRQ_GPU, GPU_INT_BASE, pw_gpu_irq_handler);
	if (ret)
		return ret;
	/* gpu.c:178-179 */
	pw_irq_enable_events(dev, PW_IRQ_GPU, dev->backend == PW_BACKEND_JM ?
			     GPU_INTERRUPTS_MASK_JM : GPU_INTERRUPTS_MASK);
	pw_irq_resume(dev, PW_IRQ_GPU);
	return PW_OK;
}

void pw_gpu_fini(struct pw_device *dev)
{
	pw_irq_suspend(dev, PW_IRQ_GPU);
	if (dev->gpu.reqs_lock) {
		/* Wake up waiters (gpu.c:128). */
		pw_os_mutex_lock(dev->gpu.reqs_lock);
		dev->gpu.pending_reqs = 0;
		pw_os_mutex_unlock(dev->gpu.reqs_lock);
	}
	if (dev->gpu.reqs_event)
		pw_os_event_signal(dev->gpu.reqs_event);
}

void pw_gpu_release(struct pw_device *dev)
{
	if (dev->gpu.reqs_lock)
		pw_os_mutex_destroy(dev->os, dev->gpu.reqs_lock);
	if (dev->gpu.flush_lock)
		pw_os_mutex_destroy(dev->os, dev->gpu.flush_lock);
	if (dev->gpu.reqs_event)
		pw_os_event_destroy(dev->os, dev->gpu.reqs_event);
	dev->gpu.reqs_lock = NULL;
	dev->gpu.flush_lock = NULL;
	dev->gpu.reqs_event = NULL;
}

/*
 * Completion bits that a thread waits for (soft reset, cache flush). They are only read and
 * cleared under gpu.reqs_lock - by the bottom half and by pw_gpu_wait_req()'s RAWSTAT fast path -
 * so a stale status sampled by a late bottom half can never complete a newer request.
 */
#define GPU_WAITABLE_IRQS (GPU_IRQ_RESET_COMPLETED | GPU_IRQ_CLEAN_CACHES_COMPLETED)

/* gpu.c:83-113 */
void pw_gpu_irq_handler(struct pw_device *dev, uint32_t status)
{
	uint32_t raw;

	if (status & ~GPU_WAITABLE_IRQS)
		gpu_write(dev, GPU_INT_BASE + INT_CLEAR, status & ~GPU_WAITABLE_IRQS);

	if (status & GPU_IRQ_FAULT) {
		uint32_t fault_status = gpu_read(dev, GPU_FAULT_STATUS);
		uint64_t address = gpu_read64(dev, GPU_FAULT_ADDR);

		pw_warn(dev, "GPU Fault 0x%08x (%s) at 0x%016llx", fault_status,
			pw_exception_name(fault_status & 0xff), (unsigned long long)address);
	}
	if (status & GPU_IRQ_PROTM_FAULT)
		pw_warn(dev, "GPU Fault in protected mode");

	pw_os_mutex_lock(dev->gpu.reqs_lock);
	raw = gpu_read(dev, GPU_INT_BASE + INT_RAWSTAT) & GPU_WAITABLE_IRQS;
	if (raw) {
		gpu_write(dev, GPU_INT_BASE + INT_CLEAR, raw);
		if (raw & dev->gpu.pending_reqs) {
			dev->gpu.pending_reqs &= ~raw;
			pw_os_event_signal(dev->gpu.reqs_event);
		}
	}
	pw_os_mutex_unlock(dev->gpu.reqs_lock);
}

/*
 * panthor waits on a waitqueue woken by the IRQ thread and, on timeout, accepts the request as
 * done if RAWSTAT shows the bit (gpu.c:362-372, 418-428). We check RAWSTAT on every iteration so
 * the core also works before the OS layer has connected the interrupt (or if the ACPI IRQ
 * routing is wrong - a real risk during bring-up) at the cost of 1 ms granularity.
 */
int pw_gpu_wait_req(struct pw_device *dev, uint32_t bit, uint32_t timeout_ms)
{
	uint64_t deadline = pw_os_time_ns(dev->os) + (uint64_t)timeout_ms * 1000000u;

	for (;;) {
		bool expired = pw_os_time_ns(dev->os) > deadline;
		bool pending;

		pw_os_mutex_lock(dev->gpu.reqs_lock);
		pending = dev->gpu.pending_reqs & bit;
		if (pending && (gpu_read(dev, GPU_INT_BASE + INT_RAWSTAT) & bit)) {
			gpu_write(dev, GPU_INT_BASE + INT_CLEAR, bit);
			dev->gpu.pending_reqs &= ~bit;
			pending = false;
		}
		pw_os_mutex_unlock(dev->gpu.reqs_lock);

		if (!pending)
			return PW_OK;
		if (expired)
			break;
		pw_os_event_wait(dev->gpu.reqs_event, 1);
	}

	pw_os_mutex_lock(dev->gpu.reqs_lock);
	dev->gpu.pending_reqs &= ~bit;
	pw_os_mutex_unlock(dev->gpu.reqs_lock);
	return PW_ETIMEDOUT;
}

/* gpu.c:402-445 */
int pw_gpu_soft_reset(struct pw_device *dev)
{
	int ret;

	pw_os_mutex_lock(dev->gpu.reqs_lock);
	dev->gpu.pending_reqs |= GPU_IRQ_RESET_COMPLETED;
	pw_os_event_reset(dev->gpu.reqs_event);
	gpu_write(dev, GPU_INT_BASE + INT_CLEAR, GPU_IRQ_RESET_COMPLETED);
	/* (jm-arch) JM GPU_CMD is a bare command number (pw_regs_jm.h). */
	gpu_write(dev, GPU_CMD, dev->backend == PW_BACKEND_JM ? GPU_JM_CMD_SOFT_RESET :
							      GPU_SOFT_RESET);
	pw_os_mutex_unlock(dev->gpu.reqs_lock);

	ret = pw_gpu_wait_req(dev, GPU_IRQ_RESET_COMPLETED, PW_GPU_RESET_TIMEOUT_MS);
	if (ret) {
		pw_err(dev, "Soft reset timeout (GPU_INT_RAWSTAT 0x%08x, GPU_STATUS 0x%08x)",
		       gpu_read(dev, GPU_INT_BASE + INT_RAWSTAT), gpu_read(dev, GPU_STATUS));
		return ret;
	}

	pw_os_mutex_lock(dev->gpu.reqs_lock);
	dev->gpu.pending_reqs = 0;
	pw_os_mutex_unlock(dev->gpu.reqs_lock);
	dev->gpu.l2_on = false;

	/* A reset may clear interrupt masks; restore the ones we own. */
	pw_irq_rearm_all(dev);
	return PW_OK;
}

/* gpu.c:233-274 (panthor_gpu_block_power_on) for the L2. */
int pw_gpu_l2_power_on(struct pw_device *dev)
{
	const uint64_t mask = 1;	/* gpu.c:322: only L2 slice 0 (one core group) */
	uint64_t val;
	int ret;

	if (dev->gpu_info.l2_present != 1) {
		/* gpu.c:304-318: only the first core group is used. */
		pw_info(dev, "l2_present=0x%llx: using only the first core group",
			(unsigned long long)dev->gpu_info.l2_present);
	}

	/* Coherency mode (and L2 config, ASN hash: MT8196 only) must be set before L2 power-up
	 * (gpu.c:320). */
	gpu_write(dev, GPU_COHERENCY_PROTOCOL, dev->gpu_info.selected_coherency);

	ret = pw_poll_reg64(dev, L2_PWRTRANS, mask, 0, 100, PW_L2_POWER_TIMEOUT_US, &val);
	if (ret) {
		pw_err(dev, "timeout waiting on L2:%llx power transition (PWRTRANS 0x%llx)",
		       (unsigned long long)mask, (unsigned long long)val);
		return ret;
	}

	gpu_write64(dev, L2_PWRON, mask);

	/*
	 * Deviation: panthor's condition is "(mask & val) == val" (gpu.c:265), which is already true
	 * while L2_READY still reads 0. We wait for every requested bit to be set.
	 */
	ret = pw_poll_reg64(dev, L2_READY, mask, mask, 100, PW_L2_POWER_TIMEOUT_US, &val);
	if (ret) {
		pw_err(dev, "timeout waiting on L2:%llx readiness (L2_READY 0x%llx, PWRTRANS 0x%llx)",
		       (unsigned long long)mask, (unsigned long long)val,
		       (unsigned long long)gpu_read64(dev, L2_PWRTRANS));
		return ret;
	}

	dev->gpu.l2_on = true;
	return PW_OK;
}

/* gpu.c:196-224 (panthor_gpu_block_power_off) */
void pw_gpu_l2_power_off(struct pw_device *dev)
{
	const uint64_t mask = dev->gpu_info.l2_present;
	uint64_t val;

	if (!dev->gpu.l2_on)
		return;
	if (pw_poll_reg64(dev, L2_PWRTRANS, mask, 0, 100, PW_L2_POWER_TIMEOUT_US, &val)) {
		pw_err(dev, "timeout waiting on L2 power transition before power-off");
		return;
	}
	gpu_write64(dev, L2_PWROFF, mask);
	if (pw_poll_reg64(dev, L2_PWRTRANS, mask, 0, 100, PW_L2_POWER_TIMEOUT_US, &val))
		pw_err(dev, "timeout waiting on L2 power-off");
	dev->gpu.l2_on = false;
}

/*
 * (jm-arch) GPU_CMD value for a cache flush. CSF: FLUSH_CACHES with per-cache clean/invalidate
 * fields. JM has no per-cache control, only CLEAN_CACHES (clean L2 + LSC) and CLEAN_INV_CACHES
 * (clean + invalidate L2 + LSC; also what panfrost's job-slot flushes use), so we pick the
 * smallest JM command that covers the request: CLEAN_INV if anything asks for invalidation.
 * Both raise CLEAN_CACHES_COMPLETED.
 */
static uint32_t pw_gpu_flush_cmd(struct pw_device *dev, uint32_t l2, uint32_t lsc, uint32_t other)
{
	if (dev->backend != PW_BACKEND_JM)
		return GPU_FLUSH_CACHES(l2, lsc, other);
	if ((l2 | lsc | other) & CACHE_INV)
		return GPU_JM_CMD_CLEAN_INV_CACHES;
	return GPU_JM_CMD_CLEAN_CACHES;
}

/* gpu.c:337-387 */
int pw_gpu_flush_caches(struct pw_device *dev, uint32_t l2, uint32_t lsc, uint32_t other)
{
	int ret = PW_OK;

	pw_os_mutex_lock(dev->gpu.flush_lock);

	pw_os_mutex_lock(dev->gpu.reqs_lock);
	if (dev->gpu.pending_reqs & GPU_IRQ_CLEAN_CACHES_COMPLETED) {
		ret = PW_EIO;
	} else {
		dev->gpu.pending_reqs |= GPU_IRQ_CLEAN_CACHES_COMPLETED;
		pw_os_event_reset(dev->gpu.reqs_event);
		/* Deviation: clear a stale completion first so the RAWSTAT fast path in
		 * pw_gpu_wait_req() can't mistake it for this flush. */
		gpu_write(dev, GPU_INT_BASE + INT_CLEAR, GPU_IRQ_CLEAN_CACHES_COMPLETED);
		gpu_write(dev, GPU_CMD, pw_gpu_flush_cmd(dev, l2, lsc, other));
	}
	pw_os_mutex_unlock(dev->gpu.reqs_lock);

	if (!ret)
		ret = pw_gpu_wait_req(dev, GPU_IRQ_CLEAN_CACHES_COMPLETED, PW_GPU_FLUSH_TIMEOUT_MS);

	pw_os_mutex_unlock(dev->gpu.flush_lock);

	if (ret)
		pw_err(dev, "Flush caches timeout (%d)", ret);	/* panthor schedules a reset */
	return ret;
}
