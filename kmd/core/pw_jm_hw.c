// SPDX-License-Identifier: MIT
/*
 * pw_jm_hw.c - job-manager GPU hardware (pw_jm_hw.h): JM feature registers and GPU quirks,
 * shader/tiler power, DRM_IOCTL_PANFROST_GET_PARAM, the job-slot registers, the job interrupt,
 * GPU recovery and the address-space LRU shared by all files. Owner: jm-hw.
 *
 * Behaviour reference: Linux drivers/gpu/drm/panfrost (v7.3), GPL-2.0-only. It was read for
 * the behaviour, register meanings and errnos only; no code or comments were copied. The
 * functions followed are cited as file:function. The Bifrost feature/erratum facts below are
 * hardware facts (which GPU has which feature), restated here in our own table.
 *
 * Locking:
 *  - jm->reset_lock: pw_jm_hw_reset() against GET_PARAM SYSTEM_TIMESTAMP (Linux reset.lock).
 *  - jm->cc_lock: the cycle-counter refcount and its GPU_CMDs.
 *  - dev->mmu.slots_lock: the AS table (jm->as[]), always taken after the pw_vm op_lock of the
 *    VM being bound (pw_mmu.h). Eviction unbinds an idle victim without its op_lock, which
 *    pw_mmu_as_unbind_locked() allows.
 *  - The slot helpers touch registers only; the scheduler serialises them per slot.
 */

#include "pw_jm_hw.h"
#include "pw_jm_mem.h"
#include "pw_jm_sched.h"
#include "pw_mmu.h"

#define PW_JM_SHADER_POWER_TIMEOUT_US	20000	/* panfrost_gpu.c:panfrost_gpu_power_on */
#define PW_JM_TILER_POWER_TIMEOUT_US	1000	/* same */
#define PW_JM_POWER_OFF_TIMEOUT_US	2000	/* panfrost_gpu.c:panfrost_gpu_power_off */
#define PW_JM_STOP_TIMEOUT_US		10000	/* panfrost_job.c:panfrost_reset, 10 ms */
#define PW_JM_IRQ_MAX_PASSES		64	/* bound for a stuck JOB_INT bit */
#define PW_JM_AS_ENTRIES		(2 * PW_MAX_AS)

/* Bifrost features that change what the driver programs (panfrost_features.h names). */
#define JM_FEAT_JOBCHAIN_DISAMBIGUATION	PW_BIT(0)
#define JM_FEAT_FLUSH_REDUCTION		PW_BIT(1)
#define JM_FEAT_TLS_HASHING		PW_BIT(2)
#define JM_FEAT_IDVS_GROUP_SIZE		PW_BIT(3)
/* Errata (panfrost_issues.h names). */
#define JM_ISSUE_T76X_3953		PW_BIT(16)	/* tiler clock gate override */

struct jm_model {
	uint16_t id;		/* GPU_ID >> 16 with the arch minor/rev nibbles masked: & 0xf00f */
	uint16_t rev;		/* GPU_ID & 0xffff for a revision-specific erratum, 0xffff = any */
	const char *name;
	uint32_t flags;
};

/*
 * The Bifrost parts Linux panfrost knows (panfrost_gpu.c gpu_models, panfrost_features.h,
 * panfrost_issues.h). Only flags with an effect here are listed: all of them also have
 * PWRON_DURING_PWROFF_TRANS, XAFFINITY, THREAD_GROUP_SPLIT, PROTECTED_MODE, COHERENCY_REG and
 * AARCH64_MMU. G31/G52/G76 have 3BIT_EXT_RW_L2_MMU_CONFIG too, but Linux never writes
 * L2_MMU_CONFIG for it, so neither do we. G31 r1p0's TGOX_R1_1234 concerns protected mode,
 * which is not used. G71's TMIX_8463 (wait for L2 transitions before a reset) is not handled:
 * G71 is not a target.
 */
static const struct jm_model jm_models[] = {
	{ 0x6000, 0x0001, "g71", JM_FEAT_JOBCHAIN_DISAMBIGUATION | JM_FEAT_FLUSH_REDUCTION |
				 JM_ISSUE_T76X_3953 },	/* r0p0 status 1 (05dev0) */
	{ 0x6000, 0xffff, "g71", JM_FEAT_JOBCHAIN_DISAMBIGUATION | JM_FEAT_FLUSH_REDUCTION },
	{ 0x6001, 0xffff, "g72", JM_FEAT_JOBCHAIN_DISAMBIGUATION | JM_FEAT_FLUSH_REDUCTION },
	{ 0x7000, 0xffff, "g51", JM_FEAT_JOBCHAIN_DISAMBIGUATION | JM_FEAT_FLUSH_REDUCTION },
	{ 0x7001, 0xffff, "g76", JM_FEAT_JOBCHAIN_DISAMBIGUATION | JM_FEAT_FLUSH_REDUCTION |
				 JM_FEAT_TLS_HASHING | JM_FEAT_IDVS_GROUP_SIZE },
	{ 0x7002, 0xffff, "g52", JM_FEAT_JOBCHAIN_DISAMBIGUATION | JM_FEAT_FLUSH_REDUCTION |
				 JM_FEAT_IDVS_GROUP_SIZE },
	{ 0x7003, 0xffff, "g31", JM_FEAT_JOBCHAIN_DISAMBIGUATION | JM_FEAT_FLUSH_REDUCTION |
				 JM_FEAT_TLS_HASHING },
};

/* One user of the AS LRU (a pw_jm_vm, or a bare pw_vm registered with pw_jm_as_get_vm). An
 * entry exists while its VM is bound to a slot or still has users (after a reset forgot the
 * binding of running jobs). Under dev->mmu.slots_lock. */
struct pw_jm_as_entry {
	struct pw_jm_vm *key;		/* NULL = free entry */
	struct pw_vm *vm;
	bool is_jvm;			/* key is a real pw_jm_vm (pw_jm_as_vm_get may return it) */
	int as;				/* bound slot, -1 if none */
	uint32_t users;			/* jobs holding the slot (pw_jm_as_get/put) */
	uint64_t lru;			/* last pw_jm_as_get, higher = more recent */
};

struct pw_jm_hw {
	struct pw_jm_gpu_info info;
	const char *model;
	uint32_t flags;			/* JM_FEAT_* | JM_ISSUE_* */
	uint32_t slot_mask;		/* JS_PRESENT & the slots the core uses */

	bool powered;			/* shader + tiler on */
	bool irq_ready;			/* job line set up */
	bool stopped;			/* pw_jm_hw_fini() ran */
	uint32_t dead;			/* __atomic: a reset failed, the GPU is unusable */
	/* __atomic, (review): a reset or fini has the job line quiesced. A bottom half that runs
	 * late (the ISR only queued it) must not touch JOB_INT then, see pw_jm_job_irq_handler. */
	uint32_t quiesced;

	struct pw_os_mutex *reset_lock;
	struct pw_os_mutex *cc_lock;
	uint32_t cc_users;		/* under cc_lock */

	pw_jm_irq_hook_t hook;		/* __atomic */
	void *hook_ctx;			/* __atomic */

	struct pw_jm_as_entry as[PW_JM_AS_ENTRIES];
	uint64_t lru_clock;
};

static bool jm_has(const struct pw_jm_hw *jm, uint32_t flag)
{
	return (jm->flags & flag) != 0;
}

static bool jm_slot_ok(struct pw_device *dev, uint32_t js)
{
	struct pw_jm_hw *jm = dev->jm;

	return jm && js < PW_JM_NUM_SLOTS && (jm->slot_mask & PW_BIT(js));
}

/* ------------------------------------------------------------------------------------------ */
/* features, quirks, power                                                                    */
/* ------------------------------------------------------------------------------------------ */

/* panfrost_device.h:panfrost_model_cmp: product ids with arch bits keep only arch major and
 * product major. */
static uint32_t jm_model_id(uint32_t gpu_id)
{
	uint32_t id = gpu_id >> 16;

	return (id & 0xf000) ? id & 0xf00f : id;
}

static const struct jm_model *jm_find_model(uint32_t gpu_id)
{
	uint32_t id = jm_model_id(gpu_id), rev = gpu_id & 0xffff;
	const struct jm_model *best = NULL;
	for (size_t i = 0; i < PW_ARRAY_SIZE(jm_models); i++) {
		if (jm_models[i].id != id)
			continue;
		if (jm_models[i].rev == rev)
			return &jm_models[i];
		if (jm_models[i].rev == 0xffff && !best)
			best = &jm_models[i];
	}
	return best;
}

/* panfrost_gpu.c:panfrost_gpu_init_features, JM-only part (the shared registers are already
 * in dev->gpu_info, read by pw_gpu_init). */
static void jm_read_features(struct pw_device *dev)
{
	struct pw_jm_hw *jm = dev->jm;
	struct pw_jm_gpu_info *info = &jm->info;
	const struct drm_panthor_gpu_info *gi = &dev->gpu_info;
	unsigned int num_js;

	info->gpu_id = gi->gpu_id;
	info->l2_features = gi->l2_features;
	info->core_features = gi->core_features;
	info->tiler_features = gi->tiler_features;
	info->mem_features = gi->mem_features;
	info->mmu_features = gi->mmu_features;
	info->thread_features = gi->thread_features;
	info->max_threads = gi->max_threads;
	info->thread_max_workgroup_sz = gi->thread_max_workgroup_size;
	info->thread_max_barrier_sz = gi->thread_max_barrier_size;
	/* Every Bifrost part has HW_FEATURE_COHERENCY_REG, so the register is valid. */
	info->coherency_features = gi->coherency_features;
	info->selected_coherency = dev->coherent ? gi->selected_coherency :
				   DRM_PANFROST_GPU_COHERENCY_NONE;
	for (unsigned int i = 0; i < 4; i++)
		info->texture_features[i] = gi->texture_features[i];
	info->as_present = gi->as_present;
	info->shader_present = gi->shader_present;
	info->tiler_present = gi->tiler_present;
	info->l2_present = gi->l2_present;

	info->js_present = gpu_read(dev, GPU_JS_PRESENT);
	/* Linux reads hweight(JS_PRESENT) entries, indexed by count, not by slot bit. */
	num_js = pw_hweight64(info->js_present);
	for (unsigned int i = 0; i < num_js && i < PW_ARRAY_SIZE(info->js_features); i++)
		info->js_features[i] = gpu_read(dev, GPU_JS_FEATURES(i));
	info->stack_present = gpu_read64(dev, GPU_STACK_PRESENT);
	info->thread_tls_alloc = gpu_read(dev, GPU_THREAD_TLS_ALLOC);
	info->afbc_features = gpu_read(dev, GPU_AFBC_FEATURES);
	info->nr_core_groups = pw_hweight64(info->l2_present);

	jm->slot_mask = info->js_present & PW_GENMASK(PW_JM_NUM_SLOTS - 1, 0);
}

/* panfrost_gpu.c:panfrost_gpu_init_quirks, restricted to the rules that can match Bifrost:
 * the Midgard SHADER_CONFIG rules (8443/11035/10327/10797, GPUCORE_1619 by product range) and
 * the T86x/T88x throttle rule cannot. No vendor quirk: the Amlogic PWR_OVERRIDE1 one is not
 * applied to Allwinner parts. */
static void jm_apply_quirks(struct pw_device *dev)
{
	struct pw_jm_hw *jm = dev->jm;
	uint32_t sc = 0, tc, jmc = 0;

	if (jm_has(jm, JM_FEAT_TLS_HASHING))
		sc |= SC_TLS_HASH_ENABLE;
	if (sc)
		gpu_write(dev, GPU_SHADER_CONFIG, sc);

	tc = gpu_read(dev, GPU_TILER_CONFIG);
	if (jm_has(jm, JM_ISSUE_T76X_3953))
		tc |= TC_CLOCK_GATE_OVERRIDE;
	gpu_write(dev, GPU_TILER_CONFIG, tc);

	/* G71 that only advertises ACE coherency must be told it can also do ACE-Lite. */
	if (jm_model_id(jm->info.gpu_id) == 0x6000 &&
	    jm->info.coherency_features == PW_BIT(GPU_COHERENCY_ACE))
		jmc |= (PW_BIT(GPU_COHERENCY_ACE_LITE) | PW_BIT(GPU_COHERENCY_ACE)) <<
		       JM_FORCE_COHERENCY_FEATURES_SHIFT;
	if (jm_has(jm, JM_FEAT_IDVS_GROUP_SIZE))
		jmc |= JM_DEFAULT_IDVS_GROUP_SIZE << JM_IDVS_GROUP_SIZE_SHIFT;
	if (jmc)
		gpu_write(dev, GPU_JM_CONFIG, jmc);
}

/* panfrost_gpu.c:panfrost_get_core_mask: with one L2 every core; otherwise only the first
 * core group's cores are powered. */
static uint64_t jm_core_mask(const struct pw_jm_gpu_info *info)
{
	if (info->l2_present == 1)
		return ~0ull;
	return ~(info->l2_present - 1) & (info->l2_present - 2);
}

/* Wait out a transition, request power-up, wait for READY (like pw_gpu_l2_power_on). */
static int jm_block_power_on(struct pw_device *dev, const char *name, uint32_t pwrtrans,
			     uint32_t pwron, uint32_t ready, uint64_t mask, uint32_t timeout_us)
{
	uint64_t val;
	int ret;

	if (!mask)
		return PW_OK;
	ret = pw_poll_reg64(dev, pwrtrans, mask, 0, 10, timeout_us, &val);
	if (ret) {
		pw_err(dev, "timeout waiting on %s:%llx power transition (PWRTRANS 0x%llx)", name,
		       (unsigned long long)mask, (unsigned long long)val);
		return ret;
	}
	gpu_write64(dev, pwron, mask);
	ret = pw_poll_reg64(dev, ready, mask, mask, 10, timeout_us, &val);
	if (ret) {
		pw_err(dev, "error powering up gpu %s (READY 0x%llx, wanted 0x%llx)", name,
		       (unsigned long long)val, (unsigned long long)mask);
		return ret;
	}
	return PW_OK;
}

/* panfrost_gpu.c:panfrost_gpu_power_on after the L2 (done by the shared pw_gpu_l2_power_on):
 * quirks first, then shader cores, then the tiler. The core stacks are not touched (Linux
 * does not either; they follow the shader cores). */
static int jm_power_on(struct pw_device *dev)
{
	struct pw_jm_hw *jm = dev->jm;
	const struct pw_jm_gpu_info *info = &jm->info;
	int ret;

	jm_apply_quirks(dev);
	ret = jm_block_power_on(dev, "shader", SHADER_PWRTRANS, SHADER_PWRON, SHADER_READY,
				info->shader_present & jm_core_mask(info),
				PW_JM_SHADER_POWER_TIMEOUT_US);
	if (ret)
		return ret;
	/* (review) The shader cores are on: from here fini must power them off, also when the
	 * tiler then times out. */
	jm->powered = true;
	return jm_block_power_on(dev, "tiler", TILER_PWRTRANS, TILER_PWRON, TILER_READY,
				 info->tiler_present, PW_JM_TILER_POWER_TIMEOUT_US);
}

/* panfrost_gpu.c:panfrost_gpu_power_off, minus the L2 (pw_gpu_l2_power_off does it). */
static void jm_power_off(struct pw_device *dev)
{
	struct pw_jm_hw *jm = dev->jm;
	uint64_t val;

	gpu_write64(dev, SHADER_PWROFF, jm->info.shader_present);
	if (pw_poll_reg64(dev, SHADER_PWRTRANS, ~0ull, 0, 1, PW_JM_POWER_OFF_TIMEOUT_US, &val))
		pw_err(dev, "shader power transition timeout (0x%llx)", (unsigned long long)val);
	gpu_write64(dev, TILER_PWROFF, jm->info.tiler_present);
	if (pw_poll_reg64(dev, TILER_PWRTRANS, ~0ull, 0, 1, PW_JM_POWER_OFF_TIMEOUT_US, &val))
		pw_err(dev, "tiler power transition timeout (0x%llx)", (unsigned long long)val);
	jm->powered = false;
}

static uint32_t jm_irq_mask(const struct pw_jm_hw *jm)
{
	uint32_t mask = 0;

	for (uint32_t js = 0; js < PW_JM_NUM_SLOTS; js++) {
		if (jm->slot_mask & PW_BIT(js))
			mask |= JOB_INT_SLOT_MASK(js);
	}
	return mask;
}

/* ------------------------------------------------------------------------------------------ */
/* device lifetime                                                                            */
/* ------------------------------------------------------------------------------------------ */

int pw_jm_hw_init(struct pw_device *dev)
{
	struct pw_jm_hw *jm;
	const struct jm_model *model;
	const struct pw_jm_gpu_info *info;
	int ret;

	jm = pw_os_zalloc(dev->os, sizeof(*jm));
	if (!jm)
		return PW_ENOMEM;
	dev->jm = jm;
	for (unsigned int i = 0; i < PW_JM_AS_ENTRIES; i++)
		jm->as[i].as = -1;
	jm->reset_lock = pw_os_mutex_create(dev->os);
	jm->cc_lock = pw_os_mutex_create(dev->os);
	if (!jm->reset_lock || !jm->cc_lock)
		return PW_ENOMEM;

	jm_read_features(dev);
	info = &jm->info;
	model = jm_find_model(info->gpu_id);
	if (model) {
		jm->model = model->name;
		jm->flags = model->flags;
	} else {
		/* Linux refuses unknown models (-ENODEV); every Bifrost part has these, but stay
		 * conservative: no chain flag, no flush reduction. */
		jm->model = "unknown";
		pw_warn(dev, "GPU model 0x%x rev 0x%x not in the Bifrost table: no optional "
			"features used", info->gpu_id >> 16, info->gpu_id & 0xffff);
	}

	/* panfrost_gpu.c:panfrost_gpu_init_features log lines. */
	pw_info(dev, "mali-%s id 0x%x major 0x%x minor 0x%x status 0x%x", jm->model,
		info->gpu_id >> 16, GPU_VER_MAJOR(info->gpu_id), GPU_VER_MINOR(info->gpu_id),
		GPU_VER_STATUS(info->gpu_id));
	pw_info(dev, "Features: L2:0x%08x Shader:0x%08x Tiler:0x%08x Mem:0x%x MMU:0x%08x AS:0x%x "
		"JS:0x%x", info->l2_features, info->core_features, info->tiler_features,
		info->mem_features, info->mmu_features, info->as_present, info->js_present);
	pw_info(dev, "shader_present=0x%llx l2_present=0x%llx stack_present=0x%llx "
		"tls_alloc=%u afbc=0x%x", (unsigned long long)info->shader_present,
		(unsigned long long)info->l2_present, (unsigned long long)info->stack_present,
		info->thread_tls_alloc, info->afbc_features);
	if (info->l2_present != 1)
		pw_info(dev, "using only 1st core group (%u cores from %u)",
			pw_hweight64(info->shader_present & jm_core_mask(info)),
			pw_hweight64(info->shader_present));
	if (jm->slot_mask != PW_GENMASK(PW_JM_NUM_SLOTS - 1, 0))
		pw_warn(dev, "JS_PRESENT 0x%x lacks some of JS0-JS%u", info->js_present,
			PW_JM_NUM_SLOTS - 1);
	if (!(jm->slot_mask & PW_BIT(0)) || !(jm->slot_mask & PW_BIT(1))) {
		pw_err(dev, "JS0/JS1 missing (JS_PRESENT 0x%x)", info->js_present);
		return PW_EOPNOTSUPP;
	}

	ret = jm_power_on(dev);
	if (ret)
		return ret;

	/* panfrost_job.c:panfrost_jm_init: clear, then unmask done|err of every used slot. */
	ret = pw_irq_init(dev, PW_IRQ_JOB, JOB_INT_BASE, pw_jm_job_irq_handler);
	if (ret)
		return ret;
	pw_irq_enable_events(dev, PW_IRQ_JOB, jm_irq_mask(jm));
	pw_irq_resume(dev, PW_IRQ_JOB);
	jm->irq_ready = true;
	return PW_OK;
}

void pw_jm_hw_fini(struct pw_device *dev)
{
	struct pw_jm_hw *jm = dev->jm;
	struct pw_mmu *mmu = &dev->mmu;

	if (!jm || jm->stopped)
		return;
	jm->stopped = true;

	pw_irq_suspend(dev, PW_IRQ_JOB);
	__atomic_store_n(&jm->quiesced, 1u, __ATOMIC_RELEASE);	/* (review) late bottom halves */
	if (jm->irq_ready) {
		for (uint32_t js = 0; js < PW_JM_NUM_SLOTS; js++) {
			if (!(jm->slot_mask & PW_BIT(js)))
				continue;
			gpu_write(dev, JS_COMMAND_NEXT(js), JS_COMMAND_NOP);
			gpu_write(dev, JS_COMMAND(js), JS_COMMAND_HARD_STOP);
		}
	}

	if (mmu->slots_lock) {
		pw_os_mutex_lock(mmu->slots_lock);
		for (unsigned int i = 0; i < PW_JM_AS_ENTRIES; i++) {
			struct pw_jm_as_entry *e = &jm->as[i];

			if (!e->key)
				continue;
			if (e->as >= 0 && mmu->ready)
				pw_mmu_as_unbind_locked(e->vm);
			if (e->users)
				pw_warn(dev, "AS user %p still has %u users at teardown",
					(void *)e->key, e->users);
			pw_memset(e, 0, sizeof(*e));
			e->as = -1;
		}
		pw_os_mutex_unlock(mmu->slots_lock);
	}

	if (jm->powered)
		jm_power_off(dev);
	if (jm->cc_lock) {
		pw_os_mutex_lock(jm->cc_lock);
		if (jm->cc_users)
			gpu_write(dev, GPU_CMD, GPU_JM_CMD_CYCLE_COUNT_STOP);
		jm->cc_users = 0;
		pw_os_mutex_unlock(jm->cc_lock);
	}
}

void pw_jm_hw_release(struct pw_device *dev)
{
	struct pw_jm_hw *jm = dev->jm;

	if (!jm)
		return;
	if (jm->reset_lock)
		pw_os_mutex_destroy(dev->os, jm->reset_lock);
	if (jm->cc_lock)
		pw_os_mutex_destroy(dev->os, jm->cc_lock);
	pw_os_free(dev->os, jm);
	dev->jm = NULL;
}

/* ------------------------------------------------------------------------------------------ */
/* GET_PARAM                                                                                  */
/* ------------------------------------------------------------------------------------------ */

const struct pw_jm_gpu_info *pw_jm_gpu_info(struct pw_device *dev)
{
	return dev && dev->jm ? &dev->jm->info : NULL;
}

/* panfrost_drv.c:panfrost_ioctl_query_timestamp: cycle counter held around the read, no
 * reset in between. */
static uint64_t jm_read_timestamp(struct pw_device *dev)
{
	struct pw_jm_hw *jm = dev->jm;
	uint64_t ts;

	pw_os_mutex_lock(jm->reset_lock);
	pw_jm_cycle_counter_get(dev);
	ts = gpu_read64_counter(dev, GPU_TIMESTAMP);
	pw_jm_cycle_counter_put(dev);
	pw_os_mutex_unlock(jm->reset_lock);
	return ts;
}

/* panfrost_drv.c:panfrost_ioctl_get_param. */
int pw_jm_ioctl_get_param(struct pw_file *file, void *arg)
{
	struct drm_panfrost_get_param *p = arg;
	struct pw_device *dev = file->dev;
	const struct pw_jm_gpu_info *info = pw_jm_gpu_info(dev);
	uint32_t param = p->param;

	if (p->pad != 0)
		return PW_EINVAL;
	if (!info)
		return PW_EINVAL;

	if (param >= DRM_PANFROST_PARAM_TEXTURE_FEATURES0 &&
	    param <= DRM_PANFROST_PARAM_TEXTURE_FEATURES3) {
		p->value = info->texture_features[param - DRM_PANFROST_PARAM_TEXTURE_FEATURES0];
		return PW_OK;
	}
	if (param >= DRM_PANFROST_PARAM_JS_FEATURES0 &&
	    param <= DRM_PANFROST_PARAM_JS_FEATURES15) {
		p->value = info->js_features[param - DRM_PANFROST_PARAM_JS_FEATURES0];
		return PW_OK;
	}

	switch (param) {
	case DRM_PANFROST_PARAM_GPU_PROD_ID:
		p->value = info->gpu_id >> 16;
		break;
	case DRM_PANFROST_PARAM_GPU_REVISION:
		p->value = info->gpu_id & 0xffff;
		break;
	case DRM_PANFROST_PARAM_SHADER_PRESENT:
		p->value = info->shader_present;
		break;
	case DRM_PANFROST_PARAM_TILER_PRESENT:
		p->value = info->tiler_present;
		break;
	case DRM_PANFROST_PARAM_L2_PRESENT:
		p->value = info->l2_present;
		break;
	case DRM_PANFROST_PARAM_STACK_PRESENT:
		p->value = info->stack_present;
		break;
	case DRM_PANFROST_PARAM_AS_PRESENT:
		p->value = info->as_present;
		break;
	case DRM_PANFROST_PARAM_JS_PRESENT:
		p->value = info->js_present;
		break;
	case DRM_PANFROST_PARAM_L2_FEATURES:
		p->value = info->l2_features;
		break;
	case DRM_PANFROST_PARAM_CORE_FEATURES:
		p->value = info->core_features;
		break;
	case DRM_PANFROST_PARAM_TILER_FEATURES:
		p->value = info->tiler_features;
		break;
	case DRM_PANFROST_PARAM_MEM_FEATURES:
		p->value = info->mem_features;
		break;
	case DRM_PANFROST_PARAM_MMU_FEATURES:
		p->value = info->mmu_features;
		break;
	case DRM_PANFROST_PARAM_THREAD_FEATURES:
		p->value = info->thread_features;
		break;
	case DRM_PANFROST_PARAM_MAX_THREADS:
		p->value = info->max_threads;
		break;
	case DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ:
		p->value = info->thread_max_workgroup_sz;
		break;
	case DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ:
		p->value = info->thread_max_barrier_sz;
		break;
	case DRM_PANFROST_PARAM_COHERENCY_FEATURES:
		p->value = info->coherency_features;
		break;
	case DRM_PANFROST_PARAM_AFBC_FEATURES:
		p->value = info->afbc_features;
		break;
	case DRM_PANFROST_PARAM_NR_CORE_GROUPS:
		p->value = info->nr_core_groups;
		break;
	case DRM_PANFROST_PARAM_THREAD_TLS_ALLOC:
		p->value = info->thread_tls_alloc;
		break;
	case DRM_PANFROST_PARAM_SELECTED_COHERENCY:
		p->value = info->selected_coherency;
		break;
	case DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP:
		p->value = jm_read_timestamp(dev);
		break;
	case DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP_FREQUENCY:
		/* CNTFRQ_EL0: the GPU timestamp counts the SoC system counter. fw.timer_hz is the
		 * shared copy of pw_device_params.timer_hz (0 = the 24 MHz default; the H616's
		 * counter runs at 24 MHz too). */
		p->value = dev->fw.timer_hz ? dev->fw.timer_hz : 24000000u;
		break;
	case DRM_PANFROST_PARAM_ALLOWED_JM_CTX_PRIORITIES:
		/* HIGH needs CAP_SYS_NICE / DRM master (panfrost_high_prio_allowed): never here. */
		p->value = PW_BIT(PANFROST_JM_CTX_PRIORITY_LOW) |
			   PW_BIT(PANFROST_JM_CTX_PRIORITY_MEDIUM);
		break;
	default:
		return PW_EINVAL;
	}
	return PW_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* job slots                                                                                  */
/* ------------------------------------------------------------------------------------------ */

uint64_t pw_jm_slot_affinity(struct pw_device *dev)
{
	return dev && dev->jm ? dev->jm->info.shader_present : 0;
}

/* (jm-sched) panfrost_job.c:panfrost_jm_init: credit_limit 2, or 1 without
 * JOBCHAIN_DISAMBIGUATION. */
uint32_t pw_jm_slot_depth(struct pw_device *dev)
{
	if (!dev || !dev->jm)
		return 0;
	return jm_has(dev->jm, JM_FEAT_JOBCHAIN_DISAMBIGUATION) ? 2u : 1u;
}

/* panfrost_job.c:panfrost_job_hw_submit, register part. */
void pw_jm_slot_submit(struct pw_device *dev, uint32_t js, uint64_t jc, uint32_t as,
		       uint32_t flush_id, bool chain_flag)
{
	struct pw_jm_hw *jm = dev->jm;
	uint32_t cfg;

	if (!jm_slot_ok(dev, js)) {
		pw_err(dev, "submit on JS%u: slot not present", js);
		return;
	}
	if (__atomic_load_n(&jm->dead, __ATOMIC_ACQUIRE)) {
		pw_err(dev, "submit on JS%u after a failed GPU reset: dropped", js);
		return;
	}
	if (as >= PW_MAX_AS) {
		pw_err(dev, "submit on JS%u with bad AS %u: dropped", js, as);
		return;
	}

	cfg = JS_CONFIG_ADDRESS_SPACE(as) | JS_CONFIG_THREAD_PRI(8) |
	      JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE | JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE;
	if (chain_flag && jm_has(jm, JM_FEAT_JOBCHAIN_DISAMBIGUATION))
		cfg |= JS_CONFIG_JOB_CHAIN_FLAG;
	if (jm_has(jm, JM_FEAT_FLUSH_REDUCTION))
		cfg |= JS_CONFIG_ENABLE_FLUSH_REDUCTION;

	gpu_write64(dev, JS_HEAD_NEXT(js), jc);
	gpu_write64(dev, JS_AFFINITY_NEXT(js), jm->info.shader_present);
	gpu_write(dev, JS_CONFIG_NEXT(js), cfg);
	if (jm_has(jm, JM_FEAT_FLUSH_REDUCTION))
		gpu_write(dev, JS_FLUSH_ID_NEXT(js), flush_id);
	/* The job descriptors the caller wrote must reach memory before the GPU reads them. */
	pw_wmb();
	gpu_write(dev, JS_COMMAND_NEXT(js), JS_COMMAND_START);
	pw_dbg(dev, "JS%u: submit head 0x%llx AS%u cfg 0x%x flush id %u", js,
	       (unsigned long long)jc, as, cfg, flush_id);
}

bool pw_jm_slot_next_free(struct pw_device *dev, uint32_t js)
{
	if (!jm_slot_ok(dev, js))
		return false;
	return gpu_read(dev, JS_COMMAND_NEXT(js)) == JS_COMMAND_NOP;
}

void pw_jm_slot_stop(struct pw_device *dev, uint32_t js, bool hard, bool chain_flag)
{
	uint32_t cmd;

	if (!jm_slot_ok(dev, js))
		return;
	if (jm_has(dev->jm, JM_FEAT_JOBCHAIN_DISAMBIGUATION)) {
		if (hard)
			cmd = chain_flag ? JS_COMMAND_HARD_STOP_1 : JS_COMMAND_HARD_STOP_0;
		else
			cmd = chain_flag ? JS_COMMAND_SOFT_STOP_1 : JS_COMMAND_SOFT_STOP_0;
	} else {
		cmd = hard ? JS_COMMAND_HARD_STOP : JS_COMMAND_SOFT_STOP;
	}
	gpu_write(dev, JS_COMMAND(js), cmd);
}

void pw_jm_slot_cancel_next(struct pw_device *dev, uint32_t js)
{
	if (jm_slot_ok(dev, js))
		gpu_write(dev, JS_COMMAND_NEXT(js), JS_COMMAND_NOP);
}

uint32_t pw_jm_slot_status(struct pw_device *dev, uint32_t js)
{
	return jm_slot_ok(dev, js) ? gpu_read(dev, JS_STATUS(js)) : 0;
}

uint64_t pw_jm_slot_head(struct pw_device *dev, uint32_t js)
{
	return jm_slot_ok(dev, js) ? gpu_read64(dev, JS_HEAD(js)) : 0;
}

uint64_t pw_jm_slot_tail(struct pw_device *dev, uint32_t js)
{
	return jm_slot_ok(dev, js) ? gpu_read64(dev, JS_TAIL(js)) : 0;
}

/* panfrost_gpu.c:panfrost_gpu_get_latest_flush_id (the GPU is always powered here). */
uint32_t pw_jm_latest_flush_id(struct pw_device *dev)
{
	if (!dev->jm || !jm_has(dev->jm, JM_FEAT_FLUSH_REDUCTION))
		return 0;
	return gpu_read(dev, GPU_LATEST_FLUSH_ID);
}

/* panfrost_gpu.c:panfrost_cycle_counter_get/put. */
void pw_jm_cycle_counter_get(struct pw_device *dev)
{
	struct pw_jm_hw *jm = dev->jm;

	if (!jm)
		return;
	pw_os_mutex_lock(jm->cc_lock);
	if (jm->cc_users++ == 0)
		gpu_write(dev, GPU_CMD, GPU_JM_CMD_CYCLE_COUNT_START);
	pw_os_mutex_unlock(jm->cc_lock);
}

void pw_jm_cycle_counter_put(struct pw_device *dev)
{
	struct pw_jm_hw *jm = dev->jm;

	if (!jm)
		return;
	pw_os_mutex_lock(jm->cc_lock);
	if (!jm->cc_users)
		pw_warn(dev, "cycle counter put without a get");
	else if (--jm->cc_users == 0)
		gpu_write(dev, GPU_CMD, GPU_JM_CMD_CYCLE_COUNT_STOP);
	pw_os_mutex_unlock(jm->cc_lock);
}

struct jm_exc_name {
	uint8_t code;
	const char *name;
};

/* JM meanings of the codes that differ from (or are missing in) the CSF table; the names are
 * the uAPI's enum drm_panfrost_exception_type names. */
static const struct jm_exc_name jm_exc_names[] = {
	{ PW_JM_EXC_DONE, "DONE" },
	{ PW_JM_EXC_INTERRUPTED, "INTERRUPTED" },
	{ PW_JM_EXC_STOPPED, "STOPPED" },
	{ PW_JM_EXC_JOB_CONFIG_FAULT, "JOB_CONFIG_FAULT" },
	{ PW_JM_EXC_JOB_POWER_FAULT, "JOB_POWER_FAULT" },
	{ PW_JM_EXC_JOB_READ_FAULT, "JOB_READ_FAULT" },
	{ PW_JM_EXC_JOB_WRITE_FAULT, "JOB_WRITE_FAULT" },
	{ PW_JM_EXC_JOB_AFFINITY_FAULT, "JOB_AFFINITY_FAULT" },
	{ PW_JM_EXC_JOB_BUS_FAULT, "JOB_BUS_FAULT" },
	{ PW_JM_EXC_INSTR_TYPE_MISMATCH, "INSTR_TYPE_MISMATCH" },
	{ PW_JM_EXC_INSTR_OPERAND_FAULT, "INSTR_OPERAND_FAULT" },
	{ PW_JM_EXC_INSTR_TLS_FAULT, "INSTR_TLS_FAULT" },
	{ PW_JM_EXC_INSTR_ALIGN_FAULT, "INSTR_ALIGN_FAULT" },
	{ PW_JM_EXC_OOM_AFBC, "OOM_AFBC" },
	{ PW_JM_EXC_UNKNOWN, "UNKNOWN" },
	{ PW_JM_EXC_DELAYED_BUS_FAULT, "DELAYED_BUS_FAULT" },
	{ PW_JM_EXC_TRANSLATION_FAULT_IDENTITY, "TRANSLATION_FAULT_IDENTITY" },
	{ PW_JM_EXC_TRANSTAB_BUS_FAULT_0, "TRANSTAB_BUS_FAULT_0" },
	{ PW_JM_EXC_TRANSTAB_BUS_FAULT_1, "TRANSTAB_BUS_FAULT_1" },
	{ PW_JM_EXC_TRANSTAB_BUS_FAULT_2, "TRANSTAB_BUS_FAULT_2" },
	{ PW_JM_EXC_TRANSTAB_BUS_FAULT_3, "TRANSTAB_BUS_FAULT_3" },
	{ PW_JM_EXC_ACCESS_FLAG_0, "ACCESS_FLAG_0" },
};

const char *pw_jm_exception_name(uint32_t code)
{
	for (size_t i = 0; i < PW_ARRAY_SIZE(jm_exc_names); i++) {
		if (jm_exc_names[i].code == code)
			return jm_exc_names[i].name;
	}
	/* 0x40-0x4f are CS_* faults on CSF: never show those names for a JM code. */
	if (code >= 0x40 && code <= 0x4f)
		return "Unknown exception type";
	return pw_exception_name(code);
}

/* ------------------------------------------------------------------------------------------ */
/* job IRQ                                                                                    */
/* ------------------------------------------------------------------------------------------ */

void pw_jm_hw_set_irq_hook(struct pw_device *dev, pw_jm_irq_hook_t fn, void *ctx)
{
	if (!dev || !dev->jm)
		return;
	__atomic_store_n(&dev->jm->hook_ctx, ctx, __ATOMIC_RELEASE);
	__atomic_store_n(&dev->jm->hook, fn, __ATOMIC_RELEASE);
}

/*
 * panfrost_job.c:panfrost_jm_handle_irq, register part: per pass, cancel NEXT on failed slots
 * and read their status before clearing (the hardware keeps NEXT from starting while the
 * error is pending, and the status registers stay valid until then), clear, sample JS_STATE
 * for the slots just cleared, and repeat until RAWSTAT is empty so a DONE followed by an ERR
 * on the same slot is not mistaken for two completions. Fills *@ev (zeroed by the caller).
 */
static void jm_collect_events(struct pw_device *dev, uint32_t status, struct pw_jm_irq_event *ev)
{
	struct pw_jm_hw *jm = dev->jm;
	uint32_t mask, js_state = 0;

	mask = jm_irq_mask(jm);
	status &= mask;

	for (unsigned int pass = 0; status && pass < PW_JM_IRQ_MAX_PASSES; pass++) {
		uint32_t state_mask = 0;

		for (uint32_t js = 0; js < PW_JM_NUM_SLOTS; js++) {
			if (!(status & JOB_INT_SLOT_MASK(js)))
				continue;
			state_mask |= JOB_JS_STATE_SLOT_MASK(js);
			if (status & JOB_INT_DONE(js))
				ev->done |= PW_BIT(js);
			if (status & JOB_INT_ERR(js)) {
				gpu_write(dev, JS_COMMAND_NEXT(js), JS_COMMAND_NOP);
				ev->failed |= PW_BIT(js);
				ev->status[js] = gpu_read(dev, JS_STATUS(js));
				ev->head[js] = gpu_read64(dev, JS_HEAD(js));
				ev->tail[js] = gpu_read64(dev, JS_TAIL(js));
			}
		}
		gpu_write(dev, JOB_INT_BASE + INT_CLEAR, status);
		js_state &= ~state_mask;
		js_state |= gpu_read(dev, JOB_INT_JS_STATE) & state_mask;
		status = gpu_read(dev, JOB_INT_BASE + INT_RAWSTAT) & mask;
	}
	ev->js_state = js_state;

	for (uint32_t js = 0; js < PW_JM_NUM_SLOTS; js++) {
		if (!(ev->failed & PW_BIT(js)))
			continue;
		if (ev->status[js] > PW_EXC_MAX_NON_FAULT)
			pw_err(dev, "js fault, js=%u, status=%s (0x%x), head=0x%llx, tail=0x%llx", js,
			       pw_jm_exception_name(ev->status[js]), ev->status[js],
			       (unsigned long long)ev->head[js], (unsigned long long)ev->tail[js]);
		else
			pw_dbg(dev, "js event, js=%u, status=%s, head=0x%llx, tail=0x%llx", js,
			       pw_jm_exception_name(ev->status[js]),
			       (unsigned long long)ev->head[js], (unsigned long long)ev->tail[js]);
	}
}

void pw_jm_job_irq_handler(struct pw_device *dev, uint32_t status)
{
	struct pw_jm_hw *jm = dev->jm;
	struct pw_jm_irq_event ev;
	pw_jm_irq_hook_t hook;

	if (!jm)
		return;
	/*
	 * (review) pw_irq_suspend() only masks the line: a bottom half the ISR queued before
	 * that still runs pw_irq_bottom(), which calls us for RAWSTAT & mask. pw_jm_hw_reset()
	 * sets quiesced before it waits for the running bottom half (we run under bh_lock), so
	 * every later call sees the flag and leaves JOB_INT alone; the reset reports or clears
	 * those bits itself. Without this, the hard stop's ERR could reach the scheduler in the
	 * middle of the reset (Linux has synchronize_irq() and a disabled handler instead).
	 */
	if (__atomic_load_n(&jm->quiesced, __ATOMIC_ACQUIRE))
		return;
	pw_memset(&ev, 0, sizeof(ev));
	jm_collect_events(dev, status, &ev);

	if (!ev.done && !ev.failed)
		return;
	hook = __atomic_load_n(&jm->hook, __ATOMIC_ACQUIRE);
	if (hook)
		hook(dev, &ev, __atomic_load_n(&jm->hook_ctx, __ATOMIC_ACQUIRE));
	else
		pw_jm_sched_job_irq(dev, &ev);
}

/* ------------------------------------------------------------------------------------------ */
/* recovery                                                                                   */
/* ------------------------------------------------------------------------------------------ */

/* Wait for a bottom half of @line that is running right now (synchronize_irq). The line is
 * already suspended, so a later one does nothing. */
static void jm_sync_bottom_half(struct pw_device *dev, enum pw_irq_line line)
{
	struct pw_irq *irq = &dev->irq[line];

	if (!irq->bh_lock)
		return;
	pw_os_mutex_lock(irq->bh_lock);
	pw_os_mutex_unlock(irq->bh_lock);
}

/* Forget every AS binding after a reset (panfrost_mmu.c:panfrost_mmu_reset). The users
 * counts stay: the scheduler drops the references of the jobs it fails or resubmits. */
static void jm_as_forget_all(struct pw_device *dev)
{
	struct pw_jm_hw *jm = dev->jm;
	struct pw_mmu *mmu = &dev->mmu;

	pw_os_mutex_lock(mmu->slots_lock);
	for (unsigned int i = 0; i < PW_JM_AS_ENTRIES; i++) {
		struct pw_jm_as_entry *e = &jm->as[i];

		if (!e->key)
			continue;
		/* Programs UNMAPPED (already the post-reset state) and releases the slot. The
		 * VM's op_lock is not held: a page-table update racing with a reset can only
		 * fail (and needs no AS any more). */
		if (e->as >= 0)
			pw_mmu_as_unbind_locked(e->vm);
		e->as = -1;
		if (!e->users) {
			pw_memset(e, 0, sizeof(*e));
			e->as = -1;
		}
	}
	pw_os_mutex_unlock(mmu->slots_lock);
}

/* panfrost_job.c:panfrost_reset (register part) + panfrost_device.c:panfrost_device_reset. */
int pw_jm_hw_reset_collect(struct pw_device *dev, struct pw_jm_irq_event *ev)
{
	struct pw_jm_hw *jm = dev->jm;
	uint32_t state_mask, js_state;
	int ret;

	if (ev)
		pw_memset(ev, 0, sizeof(*ev));
	if (!jm || !jm->irq_ready || jm->stopped)
		return PW_EINVAL;

	pw_os_mutex_lock(jm->reset_lock);
	pw_warn(dev, "GPU reset");

	pw_irq_suspend(dev, PW_IRQ_JOB);
	pw_irq_suspend(dev, PW_IRQ_MMU);
	__atomic_store_n(&jm->quiesced, 1u, __ATOMIC_RELEASE);
	jm_sync_bottom_half(dev, PW_IRQ_JOB);
	jm_sync_bottom_half(dev, PW_IRQ_MMU);

	/* Cancel what waits in NEXT and stop what runs. Linux soft-stops here; the contract
	 * is a hard stop, since the caller resubmits from scratch anyway. */
	state_mask = 0;
	for (uint32_t js = 0; js < PW_JM_NUM_SLOTS; js++) {
		if (!(jm->slot_mask & PW_BIT(js)))
			continue;
		state_mask |= JOB_JS_STATE_SLOT_MASK(js);
		gpu_write(dev, JS_COMMAND_NEXT(js), JS_COMMAND_NOP);
		gpu_write(dev, JS_COMMAND(js), JS_COMMAND_HARD_STOP);
	}
	if (pw_poll_reg32(dev, JOB_INT_JS_STATE, state_mask, 0, 10, PW_JM_STOP_TIMEOUT_US,
			  &js_state))
		pw_err(dev, "Hard-stop failed (JS_STATE 0x%08x), resetting anyway", js_state);

	/* (review) Like Linux's panfrost_jm_handle_irqs() before the reset: what the slots did up
	 * to the stop (chains that completed, the TERMINATED of the stopped ones) is pending in
	 * JOB_INT, and the soft reset would lose it. Report it to the caller instead. */
	if (ev)
		jm_collect_events(dev, gpu_read(dev, JOB_INT_BASE + INT_RAWSTAT), ev);

	ret = pw_gpu_soft_reset(dev);
	if (!ret)
		ret = pw_gpu_l2_power_on(dev);
	jm->powered = false;
	if (!ret)
		ret = jm_power_on(dev);

	/* The reset stopped the cycle counter: restart it for the jobs that hold it. */
	pw_os_mutex_lock(jm->cc_lock);
	if (!ret && jm->cc_users)
		gpu_write(dev, GPU_CMD, GPU_JM_CMD_CYCLE_COUNT_START);
	pw_os_mutex_unlock(jm->cc_lock);

	jm_as_forget_all(dev);

	if (ret) {
		pw_err(dev, "GPU reset failed (%d): the GPU is unusable", ret);
		__atomic_store_n(&jm->dead, 1u, __ATOMIC_RELEASE);
	} else {
		/* Nothing runs now: drop stale JOB_INT bits before a late bottom half may look. */
		gpu_write(dev, JOB_INT_BASE + INT_CLEAR, jm_irq_mask(jm));
		__atomic_store_n(&jm->quiesced, 0u, __ATOMIC_RELEASE);
		pw_irq_resume(dev, PW_IRQ_MMU);
		pw_irq_resume(dev, PW_IRQ_JOB);
	}
	pw_os_mutex_unlock(jm->reset_lock);
	return ret;
}

int pw_jm_hw_reset(struct pw_device *dev)
{
	return pw_jm_hw_reset_collect(dev, NULL);
}

/* ------------------------------------------------------------------------------------------ */
/* AS slots (panfrost_mmu.c:panfrost_mmu_as_get/put, the LRU over all files)                  */
/* ------------------------------------------------------------------------------------------ */

static struct pw_jm_as_entry *as_find(struct pw_jm_hw *jm, struct pw_jm_vm *key)
{
	for (unsigned int i = 0; i < PW_JM_AS_ENTRIES; i++) {
		if (jm->as[i].key == key)
			return &jm->as[i];
	}
	return NULL;
}

static void as_entry_clear(struct pw_jm_as_entry *e)
{
	pw_memset(e, 0, sizeof(*e));
	e->as = -1;
}

/* A slot for a VM without one: free first (lowest number, Linux ffz), else the least recently
 * used slot whose VM has no users, which loses its binding. PW_EBUSY if all are busy. */
static int as_pick_locked(struct pw_device *dev)
{
	struct pw_jm_hw *jm = dev->jm;
	struct pw_mmu *mmu = &dev->mmu;
	uint32_t present = dev->gpu_info.as_present & PW_GENMASK(PW_MAX_AS - 1, 0);
	uint32_t free = present & ~mmu->alloc_mask;
	struct pw_jm_as_entry *victim = NULL;
	int as;

	if (free)
		return (int)pw_ffs32(free) - 1;

	for (unsigned int i = 0; i < PW_JM_AS_ENTRIES; i++) {
		struct pw_jm_as_entry *e = &jm->as[i];

		if (!e->key || e->as < 0 || e->users)
			continue;
		if (!victim || e->lru < victim->lru)
			victim = e;
	}
	if (!victim)
		return PW_EBUSY;
	as = victim->as;
	pw_dbg(dev, "AS%d: evicting idle user %p", as, (void *)victim->key);
	pw_mmu_as_unbind_locked(victim->vm);
	as_entry_clear(victim);
	return as;
}

static int as_get(struct pw_device *dev, struct pw_jm_vm *key, struct pw_vm *vm, bool is_jvm)
{
	struct pw_jm_hw *jm = dev->jm;
	struct pw_mmu *mmu = &dev->mmu;
	struct pw_jm_as_entry *e;
	int as, ret;

	if (!jm || !key || !vm)
		return PW_EINVAL;

	pw_os_mutex_lock(vm->op_lock);
	pw_os_mutex_lock(mmu->slots_lock);

	e = as_find(jm, key);
	if (e && e->as >= 0 && vm->as != e->as)
		e->as = -1;	/* the slot was taken away behind our back (MMU teardown) */
	if (e && e->vm != vm) {
		pw_err(dev, "AS user %p changed page tables", (void *)key);
		ret = PW_EINVAL;
		goto out;
	}
	if (e && e->as >= 0) {
		/* Already bound: one more user, most recently used. An unhandled fault disabled
		 * the AS: bind again to clear and re-enable it (panfrost_mmu_as_get). */
		as = e->as;
		if (mmu->faulty_mask & PW_BIT(as)) {
			ret = pw_mmu_as_bind_locked(vm, (uint32_t)as);
			if (ret) {
				/* bind released the slot */
				e->as = -1;
				if (!e->users)
					as_entry_clear(e);
				goto out;
			}
		}
		e->users++;
		e->lru = ++jm->lru_clock;
		ret = as;
		goto out;
	}

	if (!e) {
		e = as_find(jm, NULL);
		if (!e) {
			pw_err(dev, "AS LRU table full (%u users)", PW_JM_AS_ENTRIES);
			ret = PW_EBUSY;
			goto out;
		}
	}

	/* Someone bound this pw_vm with the primitives directly: keep its slot. */
	as = vm->as >= 0 ? vm->as : as_pick_locked(dev);
	if (as < 0) {
		ret = as;	/* PW_EBUSY: every slot has running users */
		goto out_unused;
	}
	ret = pw_mmu_as_bind_locked(vm, (uint32_t)as);
	if (ret)
		goto out_unused;
	e->key = key;
	e->vm = vm;
	e->is_jvm = is_jvm;
	e->as = as;
	e->users++;
	e->lru = ++jm->lru_clock;
	pw_dbg(dev, "AS%d: assigned to %p, alloc_mask 0x%x", as, (void *)key, mmu->alloc_mask);
	ret = as;
	goto out;

out_unused:
	if (!e->users)
		as_entry_clear(e);
out:
	pw_os_mutex_unlock(mmu->slots_lock);
	pw_os_mutex_unlock(vm->op_lock);
	return ret;
}

int pw_jm_as_get(struct pw_device *dev, struct pw_jm_vm *jvm)
{
	return as_get(dev, jvm, jvm ? pw_jm_vm_mmu(jvm) : NULL, true);
}

int pw_jm_as_get_vm(struct pw_device *dev, struct pw_jm_vm *key, struct pw_vm *vm)
{
	return as_get(dev, key, vm, false);
}

void pw_jm_as_put(struct pw_device *dev, struct pw_jm_vm *jvm)
{
	struct pw_jm_hw *jm = dev->jm;
	struct pw_jm_as_entry *e;

	if (!jm || !jvm)
		return;
	pw_os_mutex_lock(dev->mmu.slots_lock);
	e = as_find(jm, jvm);
	if (!e || !e->users) {
		pw_warn(dev, "AS put without a get (%p)", (void *)jvm);
	} else if (--e->users == 0 && e->as < 0) {
		as_entry_clear(e);	/* the reset forgot its slot: nothing left to track */
	}
	pw_os_mutex_unlock(dev->mmu.slots_lock);
}

void pw_jm_as_release(struct pw_device *dev, struct pw_jm_vm *jvm)
{
	struct pw_jm_hw *jm = dev->jm;
	struct pw_mmu *mmu = &dev->mmu;
	struct pw_jm_as_entry *e;
	struct pw_vm *vm;

	if (!jm || !jvm)
		return;
	/* Find the page tables, then take their op_lock before slots_lock (pw_mmu.h order). The
	 * key cannot be bound again meanwhile: its last reference is gone. */
	pw_os_mutex_lock(mmu->slots_lock);
	e = as_find(jm, jvm);
	vm = e ? e->vm : NULL;
	pw_os_mutex_unlock(mmu->slots_lock);
	if (!vm)
		return;

	pw_os_mutex_lock(vm->op_lock);
	pw_os_mutex_lock(mmu->slots_lock);
	e = as_find(jm, jvm);
	if (e) {
		if (e->users)
			pw_warn(dev, "AS user %p released with %u users", (void *)jvm, e->users);
		if (e->as >= 0)
			pw_mmu_as_unbind_locked(vm);
		as_entry_clear(e);
	}
	pw_os_mutex_unlock(mmu->slots_lock);
	pw_os_mutex_unlock(vm->op_lock);
}

struct pw_jm_vm *pw_jm_as_vm_get(struct pw_device *dev, uint32_t as)
{
	struct pw_jm_hw *jm = dev->jm;
	struct pw_jm_vm *r = NULL;

	if (!jm || as >= PW_MAX_AS)
		return NULL;
	pw_os_mutex_lock(dev->mmu.slots_lock);
	for (unsigned int i = 0; i < PW_JM_AS_ENTRIES; i++) {
		struct pw_jm_as_entry *e = &jm->as[i];

		if (e->key && e->is_jvm && e->as == (int)as && dev->mmu.slots[as] == e->vm) {
			r = pw_jm_vm_tryget(e->key);
			break;
		}
	}
	pw_os_mutex_unlock(dev->mmu.slots_lock);
	return r;
}
