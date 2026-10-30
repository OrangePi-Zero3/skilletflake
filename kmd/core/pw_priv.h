/* SPDX-License-Identifier: MIT */
/*
 * pw_priv.h - internal state of the OS-neutral Mali CSF core. Not for use by the OS layer
 * (host unit tests include it to inspect state).
 */
#ifndef PW_PRIV_H
#define PW_PRIV_H

#include "pw_core.h"
#include "pw_fw_iface.h"
#include "pw_regs_fw.h"
#include "pw_regs_gpu.h"
#include "pw_regs_jm.h"
#include "pw_regs_mmu.h"
#include "pw_util.h"

#include "panthor_drm.h"

struct pw_vm;

/* ---- logging ---- */
#define pw_err(dev, ...)  pw_os_log((dev)->os, PW_LOG_ERR, __VA_ARGS__)
#define pw_warn(dev, ...) pw_os_log((dev)->os, PW_LOG_WARN, __VA_ARGS__)
#define pw_info(dev, ...) pw_os_log((dev)->os, PW_LOG_INFO, __VA_ARGS__)
#define pw_dbg(dev, ...)  pw_os_log((dev)->os, PW_LOG_DBG, __VA_ARGS__)

/* ---- interrupts (panthor_device.h:560-640 raw/threaded split) ---- */
enum pw_irq_state {
	PW_IRQ_STATE_SUSPENDED = 0,
	PW_IRQ_STATE_ACTIVE = 1,
	PW_IRQ_STATE_PROCESSING = 2,
};

struct pw_device;
typedef void (*pw_irq_handler_t)(struct pw_device *dev, uint32_t status);

struct pw_irq {
	uint32_t base;			/* GPU_INT_BASE / JOB_INT_BASE / MMU_INT_BASE */
	uint32_t mask;			/* events to unmask while ACTIVE; changed under @lock */
	uint32_t state;			/* enum pw_irq_state, accessed with __atomic builtins */
	struct pw_os_mutex *lock;	/* passive-level: mask/state transitions */
	struct pw_os_mutex *bh_lock;	/* serialises bottom halves of this line */
	pw_irq_handler_t handler;
	uint32_t top_count;		/* statistics, for debugging/tests */
	uint32_t bottom_count;
};

/* ---- GPU block ---- */
struct pw_gpu {
	struct pw_os_mutex *reqs_lock;
	uint32_t pending_reqs;		/* GPU_IRQ_* bits waited for, under reqs_lock */
	struct pw_os_event *reqs_event;
	struct pw_os_mutex *flush_lock;	/* serialises cache flushes */
	bool l2_on;
};

/* ---- MMU ---- */
struct pw_mmu {
	struct pw_os_mutex *slots_lock;
	struct pw_vm *slots[PW_MAX_AS];
	uint32_t alloc_mask;		/* AS slots in use */
	uint32_t faulty_mask;		/* AS with an unhandled fault (MMU disabled by us) */
	uint32_t va_bits;
	uint32_t pa_bits;
	uint64_t memattr;		/* AS_MEMATTR for every VM (derived from our MAIR) */
	bool ready;
	/* (added by jm-mem) Format requested with pw_device_set_pt_format() for VMs created
	 * from now on; only honoured on JM (pw_vm_create). Zero-initialised = AArch64. The
	 * AArch64 VMs use @memattr above, Mali LPAE ones PW_LPAE_MEMATTR (pw_vm_memattr()). */
	enum pw_pt_format pt_format;
};

/* ---- firmware ---- */
#define PW_FW_MAX_SECTIONS 32

struct pw_fw_section {
	uint32_t flags;			/* CSF_FW_BINARY_IFACE_ENTRY_* */
	uint32_t va;
	uint32_t size;
	struct pw_os_mem *mem;
	uint8_t *data;			/* copy of the initial contents (for reload after reset) */
	uint32_t data_size;
	bool mapped;
	char name[24];			/* sanitised, may be empty */
};

struct pw_fw_glb_iface {
	const struct pw_fw_global_control_iface *control;
	struct pw_fw_global_input_iface *input;
	const struct pw_fw_global_output_iface *output;
};

struct pw_fw_csg_iface {
	const struct pw_fw_csg_control_iface *control;
	struct pw_fw_csg_input_iface *input;
	const struct pw_fw_csg_output_iface *output;
};

struct pw_fw_cs_iface {
	const struct pw_fw_cs_control_iface *control;
	struct pw_fw_cs_input_iface *input;
	const struct pw_fw_cs_output_iface *output;
};

struct pw_fw {
	struct pw_vm *vm;		/* MCU VM, AS0 */
	struct pw_fw_section sections[PW_FW_MAX_SECTIONS];
	uint32_t nsections;
	struct pw_fw_section *shared;
	uint8_t hdr_major, hdr_minor;
	uint32_t version_hash;
	char git_sha[48];

	struct pw_os_event *req_event;	/* signalled from the job IRQ bottom half */
	/* Protects the req fields of all FW input interfaces (panthor uses a spinlock per
	 * interface, fw.h:388; the bottom half runs at passive level so a mutex is enough). */
	struct pw_os_mutex *req_lock;
	bool started;			/* pw_fw_init() ran, pw_fw_fini() not yet */
	uint32_t booted;		/* __atomic */
	uint32_t ifaces_ready;		/* __atomic: iface pointers below are valid */

	struct pw_fw_glb_iface glb;
	struct pw_fw_csg_iface csg[MAX_CSGS];
	struct pw_fw_cs_iface cs[MAX_CSGS][MAX_CS_PER_CSG];
	uint32_t timer_hz;

	/* (added by core-sched) MCU-VM kernel VA allocator: kernel objects the FW must see (queue
	 * ring-buffer interfaces, CSG suspend buffers) are mapped in the MCU VM's kernel range,
	 * [CSF_MCU_SHARED_REGION_START, +SIZE) minus the FW sections (fw.c:1525). */
	struct pw_os_mutex *kva_lock;
	struct pw_fw_kva *kva_list;	/* sorted by VA, under kva_lock */
};

struct pw_sched;
struct pw_jm_hw;
struct pw_jm_mem;
struct pw_jm_sched;

struct pw_device {
	struct pw_os *os;
	bool boot_firmware;
	char fw_name[64];
	bool probed;

	struct drm_panthor_gpu_info gpu_info;
	struct drm_panthor_csif_info csif_info;
	bool coherent;			/* always false on RK3588 */

	struct pw_irq irq[PW_IRQ_COUNT];
	struct pw_gpu gpu;
	struct pw_mmu mmu;
	struct pw_fw fw;
	struct pw_sched *sched;		/* (added by core-sched) NULL until pw_sched_init(); __atomic */

	/* (added by jm-arch) Backend chosen by pw_gpu_init() from GPU_ID; constant afterwards.
	 * On PW_BACKEND_JM, fw/sched/csif_info stay unused and the three JM modules own: */
	enum pw_backend backend;
	struct pw_jm_hw *jm;		/* pw_jm_hw.c: JM GPU state, job slots, AS LRU */
	struct pw_jm_mem *jm_mem;	/* pw_jm_mem.c: device-level memory state */
	struct pw_jm_sched *jm_sched;	/* pw_jm_sched.c: slot queues, timeouts, reset */
};

/* ---- register access (all GPU registers are 32-bit; 64-bit ones are LO/HI pairs) ---- */
static inline uint32_t gpu_read(struct pw_device *dev, uint32_t reg)
{
	return pw_os_reg_read32(dev->os, reg);
}

static inline void gpu_write(struct pw_device *dev, uint32_t reg, uint32_t val)
{
	pw_os_reg_write32(dev->os, reg, val);
}

/* panthor_device.h:520-530: LO first, then HI. */
static inline uint64_t gpu_read64(struct pw_device *dev, uint32_t reg)
{
	uint64_t lo = gpu_read(dev, reg);

	return lo | ((uint64_t)gpu_read(dev, reg + 4) << 32);
}

static inline void gpu_write64(struct pw_device *dev, uint32_t reg, uint64_t val)
{
	gpu_write(dev, reg, (uint32_t)val);
	gpu_write(dev, reg + 4, (uint32_t)(val >> 32));
}

/* Free-running counters: re-read HI until stable (panthor_device.h:542). */
static inline uint64_t gpu_read64_counter(struct pw_device *dev, uint32_t reg)
{
	uint32_t lo, hi1, hi2;

	do {
		hi1 = gpu_read(dev, reg + 4);
		lo = gpu_read(dev, reg);
		hi2 = gpu_read(dev, reg + 4);
	} while (hi1 != hi2);
	return lo | ((uint64_t)hi2 << 32);
}

/* Poll until (reg & mask) == expect. Busy-waits with pw_os_udelay; returns PW_OK/PW_ETIMEDOUT. */
int pw_poll_reg32(struct pw_device *dev, uint32_t reg, uint32_t mask, uint32_t expect,
		  uint32_t delay_us, uint32_t timeout_us, uint32_t *last);
int pw_poll_reg64(struct pw_device *dev, uint32_t reg, uint64_t mask, uint64_t expect,
		  uint32_t delay_us, uint32_t timeout_us, uint64_t *last);

/* ---- pw_irq.c ---- */
int  pw_irq_init(struct pw_device *dev, enum pw_irq_line line, uint32_t base,
		 pw_irq_handler_t handler);
void pw_irq_fini(struct pw_device *dev, enum pw_irq_line line);
void pw_irq_enable_events(struct pw_device *dev, enum pw_irq_line line, uint32_t mask);
void pw_irq_disable_events(struct pw_device *dev, enum pw_irq_line line, uint32_t mask);
void pw_irq_resume(struct pw_device *dev, enum pw_irq_line line);
void pw_irq_suspend(struct pw_device *dev, enum pw_irq_line line);
/* Re-write INT_MASK of every ACTIVE line (after a soft reset). */
void pw_irq_rearm_all(struct pw_device *dev);

/* ---- pw_gpu.c ---- */
int  pw_gpu_init(struct pw_device *dev);		/* GPU_ID, features, GPU IRQ */
void pw_gpu_fini(struct pw_device *dev);		/* quiesce (IRQs may still run) */
void pw_gpu_release(struct pw_device *dev);	/* free sync objects */
int  pw_gpu_soft_reset(struct pw_device *dev);
int  pw_gpu_l2_power_on(struct pw_device *dev);
void pw_gpu_l2_power_off(struct pw_device *dev);
int  pw_gpu_flush_caches(struct pw_device *dev, uint32_t l2, uint32_t lsc, uint32_t other);
void pw_gpu_irq_handler(struct pw_device *dev, uint32_t status);
const char *pw_gpu_model_name(const struct drm_panthor_gpu_info *info);
const char *pw_exception_name(uint32_t exception_code);
/* Wait until @bit leaves gpu.pending_reqs, or shows up in GPU_INT RAWSTAT (works without IRQs). */
int  pw_gpu_wait_req(struct pw_device *dev, uint32_t bit, uint32_t timeout_ms);

/* ---- pw_mmu.c ---- */
int  pw_mmu_init(struct pw_device *dev);
void pw_mmu_fini(struct pw_device *dev);
void pw_mmu_release(struct pw_device *dev);
void pw_mmu_irq_handler(struct pw_device *dev, uint32_t status);

/* ---- pw_fw.c ---- */
int  pw_fw_init(struct pw_device *dev);
void pw_fw_fini(struct pw_device *dev);
void pw_fw_release(struct pw_device *dev);
void pw_fw_job_irq_handler(struct pw_device *dev, uint32_t status);
int  pw_fw_glb_wait_acks(struct pw_device *dev, uint32_t req_mask, uint32_t *acked,
			 uint32_t timeout_ms);
int  pw_fw_csg_wait_acks(struct pw_device *dev, uint32_t csg, uint32_t req_mask,
			 uint32_t *acked, uint32_t timeout_ms);
void pw_fw_ring_doorbell(struct pw_device *dev, uint32_t id);
void pw_fw_ring_csg_doorbells(struct pw_device *dev, uint32_t csg_mask);
int  pw_fw_ping(struct pw_device *dev, uint32_t timeout_ms);
uint32_t pw_fw_conv_timeout(uint32_t timer_hz, uint32_t timeout_us);
/* (added by core-sched) Map @size bytes of @mem (page aligned, PW_VM_MAP_* @map_flags) at a free
 * MCU VA of the MCU VM's kernel range (PANTHOR_VM_KERNEL_AUTO_VA on ptdev->fw->vm). PW_EINVAL
 * without a booted FW, PW_ENOMEM when the range is full. */
int  pw_fw_map_kernel_mem(struct pw_device *dev, struct pw_os_mem *mem, uint64_t size,
			  uint32_t map_flags, uint32_t *mcu_va);
/* Undo pw_fw_map_kernel_mem(); unknown VAs are ignored. */
void pw_fw_unmap_kernel_mem(struct pw_device *dev, uint32_t mcu_va);

/* req/ack helpers (panthor_fw.h:452-510). Must be called with fw.req_lock held. */
static inline uint32_t pw_fw_toggle_val(uint32_t cur_req, uint32_t ack, uint32_t mask)
{
	return ((ack ^ mask) & mask) | (cur_req & ~mask);
}

static inline uint32_t pw_fw_update_val(uint32_t cur_req, uint32_t val, uint32_t mask)
{
	return (cur_req & ~mask) | (val & mask);
}

#define pw_fw_toggle_reqs(__dev, __iface, __in_reg, __out_reg, __mask)			\
	do {										\
		pw_os_mutex_lock((__dev)->fw.req_lock);					\
		PW_WRITE_ONCE((__iface)->input->__in_reg,				\
			      pw_fw_toggle_val(PW_READ_ONCE((__iface)->input->__in_reg),	\
					       PW_READ_ONCE((__iface)->output->__out_reg),	\
					       (__mask)));				\
		pw_os_mutex_unlock((__dev)->fw.req_lock);				\
	} while (0)

#define pw_fw_update_reqs(__dev, __iface, __in_reg, __val, __mask)			\
	do {										\
		pw_os_mutex_lock((__dev)->fw.req_lock);					\
		PW_WRITE_ONCE((__iface)->input->__in_reg,				\
			      pw_fw_update_val(PW_READ_ONCE((__iface)->input->__in_reg),	\
					       (__val), (__mask)));			\
		pw_os_mutex_unlock((__dev)->fw.req_lock);				\
	} while (0)

#endif /* PW_PRIV_H */
