/* SPDX-License-Identifier: MIT */
/*
 * pw_jm_hw.h - job-manager GPU hardware (Bifrost v6/v7: Mali-G31 on the Orange Pi Zero3),
 * implemented in pw_jm_hw.c. Owner: jm-hw. Design: docs/status/jm-arch.md.
 *
 * Behaviour reference: Linux drivers/gpu/drm/panfrost (panfrost_gpu.c, panfrost_job.c IRQ and
 * register parts, panfrost_mmu.c AS LRU, panfrost_device.c reset). That driver is
 * GPL-2.0-only: read it for behaviour and register meanings, never copy code. Citations below
 * are file:function, Linux v7.3.
 *
 * What already exists and is shared with CSF (do not duplicate): GPU_ID/feature read, backend
 * choice, soft reset, L2 power, cache flush (pw_gpu.c, JM command encodings included), the
 * interrupt top/bottom halves (pw_irq.c), page tables and AS programming (pw_mmu.c). On JM,
 * pw_device_probe() runs: pw_gpu_init -> pw_gpu_soft_reset -> pw_gpu_l2_power_on ->
 * pw_mmu_init -> pw_jm_hw_init -> pw_jm_mem_init -> pw_jm_sched_init.
 *
 * Contexts: everything here is passive level unless noted. "IRQ bottom half" means
 * pw_irq_bottom() of that line, which is passive level but must not block on anything that
 * needs the same line's bottom half to make progress.
 */
#ifndef PW_JM_HW_H
#define PW_JM_HW_H

#include "pw_file.h"

#include "panfrost_drm.h"

struct pw_jm_vm;

/* Job slots the core uses (panfrost_job.h NUM_JOB_SLOTS): JS0 fragment, JS1 vertex/tiler/
 * compute, JS2 compute-only (unused by panfrost, panfrost_job.c:panfrost_job_get_slot). */
#define PW_JM_NUM_SLOTS		3

/* ---- device lifetime (called by pw_device.c on JM only) ---- */

/*
 * After the shared GPU init (GPU_ID, soft reset, L2 on) and pw_mmu_init():
 *  - allocate dev->jm and read the JM-only feature registers into struct pw_jm_gpu_info
 *    (panfrost_gpu.c:panfrost_gpu_init_features: JS_PRESENT, JS_FEATURES[n] for n <
 *    hweight(JS_PRESENT), STACK_PRESENT, THREAD_TLS_ALLOC, AFBC_FEATURES, nr_core_groups =
 *    hweight(L2_PRESENT)); log them like panfrost does;
 *  - apply the GPU quirks that apply to G31 (panfrost_gpu.c:panfrost_gpu_init_quirks: the
 *    G31 feature set has 3BIT_EXT_RW_L2_MMU_CONFIG; check each SHADER/TILER/JM/L2_MMU_CONFIG
 *    rule against panfrost_features.h/panfrost_issues.h for 0x7093 r0p0);
 *  - power on the shader cores and the tiler (panfrost_gpu.c:panfrost_gpu_power_on; on G610 the
 *    CSF firmware does this): poll PWRTRANS/READY like pw_gpu_l2_power_on(), PW_ETIMEDOUT;
 *  - set up the job interrupt: pw_irq_init(dev, PW_IRQ_JOB, JOB_INT_BASE, pw_jm_job_irq_handler),
 *    unmask JOB_INT_SLOT_MASK(js) for every present slot used, pw_irq_resume();
 *  - initialise the AS slot table (all free).
 * Returns PW_OK, PW_ENOMEM, PW_ETIMEDOUT. On failure pw_device.c calls pw_jm_hw_fini() and later
 * pw_jm_hw_release(), so both must cope with a partial init.
 */
int  pw_jm_hw_init(struct pw_device *dev);
/* Quiesce: mask the job line (pw_irq_suspend), hard-stop every slot, unbind all AS slots,
 * power the shader cores and tiler off. Idempotent; a no-op if init never ran. Bottom halves
 * may still run afterwards (the OS layer flushes them before pw_jm_hw_release()). */
void pw_jm_hw_fini(struct pw_device *dev);
/* Free dev->jm and its sync objects. No bottom half is running or queued any more. */
void pw_jm_hw_release(struct pw_device *dev);

/* ---- GET_PARAM ---- */

/* Raw feature values behind DRM_PANFROST_PARAM_* (panfrost_device.h struct
 * panfrost_features). Valid after pw_jm_hw_init(). The shared ones also sit in dev->gpu_info. */
struct pw_jm_gpu_info {
	uint32_t gpu_id;		/* full GPU_ID; PROD_ID = >> 16, REVISION = & 0xffff */
	uint32_t l2_features, core_features, tiler_features, mem_features, mmu_features;
	uint32_t thread_features, max_threads, thread_max_workgroup_sz, thread_max_barrier_sz;
	uint32_t coherency_features, selected_coherency;	/* DRM_PANFROST_GPU_COHERENCY_* */
	uint32_t texture_features[4];
	uint32_t js_features[16];
	uint32_t as_present, js_present;
	uint32_t nr_core_groups, thread_tls_alloc, afbc_features;
	uint64_t shader_present, tiler_present, l2_present, stack_present;
};
const struct pw_jm_gpu_info *pw_jm_gpu_info(struct pw_device *dev);

/*
 * DRM_IOCTL_PANFROST_GET_PARAM (panfrost_drv.c:panfrost_ioctl_get_param). pad != 0 -> PW_EINVAL;
 * unknown param -> PW_EINVAL. GPU_PROD_ID = GPU_ID >> 16, GPU_REVISION = GPU_ID & 0xffff,
 * JS_FEATURESn / TEXTURE_FEATURESn index the arrays (0 past js_present). SYSTEM_TIMESTAMP reads
 * GPU_TIMESTAMP with the cycle counter held on (pw_jm_cycle_counter_get/put) and must not race a
 * reset (Linux takes reset.lock for reading); SYSTEM_TIMESTAMP_FREQUENCY is the arch timer rate
 * (pw_device_params.timer_hz, 24 MHz default; also 24 MHz on H616). ALLOWED_JM_CTX_PRIORITIES =
 * LOW | MEDIUM (HIGH needs CAP_SYS_NICE / DRM master, which Windows has no equivalent of).
 * SELECTED_COHERENCY = DRM_PANFROST_GPU_COHERENCY_NONE. Values Mesa reads at >= 1.6 can be
 * answered at any version (Linux does).
 */
int pw_jm_ioctl_get_param(struct pw_file *file, void *arg);

/* ---- job-slot registers (used by pw_jm_sched.c; it owns all job state) ---- */

/*
 * Register-level helpers. The scheduler serialises all calls for one slot (its slot lock);
 * none of these sleep, so they may be called from the job IRQ bottom half.
 * submit: program the NEXT registers of @js and write JS_COMMAND_NEXT = START
 *   (panfrost_job.c:panfrost_job_hw_submit): HEAD_NEXT = @jc, AFFINITY_NEXT = the shader cores
 *   (panfrost_job_write_affinity), CONFIG_NEXT = @as | START/END_FLUSH_CLEAN_INVALIDATE
 *   | THREAD_PRI(8) | (FLUSH_REDUCTION + FLUSH_ID_NEXT = @flush_id when the GPU has it) |
 *   JOB_CHAIN_FLAG if @chain_flag (JOBCHAIN_DISAMBIGUATION). (jm-hw fix: START_MMU is only
 *   set on GPUs with HW_ISSUE_10649, a Midgard T60x erratum, as Linux does; never on Bifrost.)
 *   The caller checked pw_jm_slot_next_free(). The AS must stay bound (pw_jm_as_get) until
 *   the job completed. A store barrier orders the caller's job-descriptor writes before START.
 *   No-op (logged) for a slot outside JS_PRESENT / PW_JM_NUM_SLOTS or after a failed reset.
 * next_free: JS_COMMAND_NEXT reads NOP, i.e. the NEXT registers can take a job.
 * stop: SOFT_STOP / HARD_STOP the job on @js; with JOBCHAIN_DISAMBIGUATION the _0/_1 variant
 *   selected by @chain_flag only stops the job that carries that flag.
 * cancel_next: JS_COMMAND_NEXT = NOP (drop a queued job that has not started).
 * status/head/tail: JS_STATUS, JS_HEAD, JS_TAIL of the job that last ran on @js.
 */
void     pw_jm_slot_submit(struct pw_device *dev, uint32_t js, uint64_t jc, uint32_t as,
			   uint32_t flush_id, bool chain_flag);
bool     pw_jm_slot_next_free(struct pw_device *dev, uint32_t js);
void     pw_jm_slot_stop(struct pw_device *dev, uint32_t js, bool hard, bool chain_flag);
void     pw_jm_slot_cancel_next(struct pw_device *dev, uint32_t js);
uint32_t pw_jm_slot_status(struct pw_device *dev, uint32_t js);
uint64_t pw_jm_slot_head(struct pw_device *dev, uint32_t js);
uint64_t pw_jm_slot_tail(struct pw_device *dev, uint32_t js);
/* GPU_LATEST_FLUSH_ID (panfrost_gpu.c:panfrost_gpu_get_latest_flush_id; 0 without
 * FLUSH_REDUCTION). Sampled by SUBMIT. */
uint32_t pw_jm_latest_flush_id(struct pw_device *dev);
/* Cycle counter refcount (GPU_JM_CMD_CYCLE_COUNT_START/STOP, panfrost_gpu.c:
 * panfrost_cycle_counter_get/put): jobs with PANFROST_JD_REQ_CYCLE_COUNT hold it while queued. */
void     pw_jm_cycle_counter_get(struct pw_device *dev);
void     pw_jm_cycle_counter_put(struct pw_device *dev);
/* JM exception name (JS_STATUS, AS_FAULTSTATUS): JM meanings of 0x40-0x4b, then the shared
 * pw_exception_name() table. */
const char *pw_jm_exception_name(uint32_t code);

/* ---- job interrupt ---- */

/*
 * What the job IRQ bottom half hands the scheduler, per pass. (jm-hw clarifications:)
 *  - A pass repeats "read RAWSTAT, clear, sample JS_STATE" until RAWSTAT is 0, as
 *    panfrost_job.c:panfrost_jm_handle_irq does, so one slot can have BOTH a done and a failed
 *    bit: the current job completed, then the job that followed from NEXT failed. Done always
 *    happened first (after a failure the slot stops until the scheduler resubmits).
 *  - For every slot in @failed the handler has already written JS_COMMAND_NEXT = NOP before
 *    clearing JOB_INT (the hardware holds NEXT back while the error is pending), so a job that
 *    was waiting in NEXT has not started: the scheduler resubmits it with pw_jm_slot_submit().
 *  - @js_state holds the raw JOB_INT_JS_STATE bits (bit js = current job active, bit js + 16 =
 *    a job waiting in NEXT) of the slots that had events in this pass, 0 for the others.
 *  - (review) A done bit means "at least one chain completed": the JM starts NEXT without an
 *    ack, so the current chain and the one from NEXT can both finish before the bit is
 *    cleared and show up as one done bit. If the slot had a job in NEXT, no failed bit, and
 *    @js_state shows neither bit of the slot, both completed (the rule Linux applies in
 *    panfrost_job.c:panfrost_jm_handle_irq).
 *  - (review) No event is delivered while pw_jm_hw_reset() or pw_jm_hw_fini() has the line
 *    quiesced, even by a bottom half that was queued before (pw_jm_hw_reset_collect()).
 */
struct pw_jm_irq_event {
	uint32_t done;		/* slots whose chain completed (JOB_INT bits 0-15) */
	uint32_t failed;	/* slots whose chain failed or stopped (bits 16-31, shifted down) */
	uint32_t js_state;	/* JOB_INT_JS_STATE after clearing: slots still active */
	uint32_t status[PW_JM_NUM_SLOTS];	/* JS_STATUS of failed slots, else 0 */
	uint64_t head[PW_JM_NUM_SLOTS];	/* JS_HEAD of failed slots (fault address) */
	uint64_t tail[PW_JM_NUM_SLOTS];	/* JS_TAIL of failed slots (resume point if STOPPED) */
};

/*
 * pw_irq_handler_t for PW_IRQ_JOB (pw_irq.c calls it from the job bottom half with
 * RAWSTAT & mask). Reads the per-slot status of failed slots, clears the bits it consumed
 * (JOB_INT_CLEAR), samples JOB_INT_JS_STATE and calls pw_jm_sched_job_irq() once with the
 * result (panfrost_job.c:panfrost_job_irq_handler_thread + panfrost_job_handle_irq order:
 * read status before clearing). Does not touch job state itself.
 */
void pw_jm_job_irq_handler(struct pw_device *dev, uint32_t status);

/* ---- recovery ---- */

/*
 * Full GPU recovery after a timeout or a fault the scheduler cannot clear
 * (panfrost_job.c:panfrost_reset + panfrost_device.c:panfrost_device_reset): mask the job
 * and MMU lines, hard-stop every slot, soft reset (pw_gpu_soft_reset; it re-arms the IRQ masks),
 * re-apply quirks, power L2 + shader + tiler back on, forget every AS binding (VMs rebind on
 * their next pw_jm_as_get; the users count of each slot is NOT reset - running jobs are
 * failed by the caller, who drops their references), unmask the lines.
 * The caller (pw_jm_sched.c) has stopped feeding the slots and holds its reset lock; it
 * resubmits or fails the jobs afterwards. Returns PW_OK or the reset/power error (the device is
 * then unusable: every later submit fails). Passive level, may sleep, not from a bottom half.
 * (jm-hw) Like synchronize_irq() in Linux, it waits for a job or MMU bottom half that is
 * running when the lines are masked, so pw_jm_sched_job_irq() must never block on a lock the
 * caller of pw_jm_hw_reset() holds. No job event of the killed jobs is delivered: the reset
 * clears JOB_INT. The cycle counter is restarted if it had users.
 */
int  pw_jm_hw_reset(struct pw_device *dev);

/*
 * (review) pw_jm_hw_reset() that also hands back, in *@ev (zeroed first, also on failure), the
 * job events still pending in JOB_INT when the slots had stopped, collected exactly like one
 * pw_jm_job_irq_handler() pass but not delivered (Linux handles these interrupts before the
 * reset, panfrost_job.c:panfrost_reset). @ev->done: the current chain completed - do not
 * resubmit it. @ev->failed with status TERMINATED: the hard stop killed it; another status:
 * it had faulted. Slots without bits had nothing running or their events were already
 * delivered. Caveat: NEXT is cancelled just before the stop, so when a slot also had a job in
 * NEXT and only done is set, that job either completed too (the JM had already promoted it)
 * or never ran; the registers cannot tell which (Linux then counts it as done). @ev may be
 * NULL (= pw_jm_hw_reset()).
 */
int  pw_jm_hw_reset_collect(struct pw_device *dev, struct pw_jm_irq_event *ev);

/* ---- address-space slots (all files share the GPU's few AS: LRU, as panfrost) ---- */

/*
 * Bind @jvm's page tables to an AS slot for a job (panfrost_mmu.c:panfrost_mmu_as_get): if it
 * already has one, take another user reference and move it to the LRU head (re-enabling the AS
 * if the fault handler disabled it); otherwise use a free slot of AS_PRESENT, or evict the least
 * recently used slot whose users count is 0 (its VM keeps its tables, just loses the slot). Uses
 * pw_mmu_as_bind_locked()/pw_mmu_as_unbind_locked() under dev->mmu.slots_lock, with @jvm's
 * pw_vm op_lock taken first. Returns the AS number (>= 0) or PW_EBUSY when every slot has
 * running users (Linux WARNs; the scheduler then keeps the job queued). Slot 0 is usable on JM
 * (no MCU). The binding itself holds no reference on @jvm: the users count is what keeps the
 * slot, and the caller (a job) holds its own VM reference.
 */
int  pw_jm_as_get(struct pw_device *dev, struct pw_jm_vm *jvm);
/* Drop one user reference (job finished or failed). The slot stays bound for reuse. */
void pw_jm_as_put(struct pw_device *dev, struct pw_jm_vm *jvm);
/* @jvm is being destroyed (pw_jm_mem.c, last reference gone apart from the binding): unbind
 * its slot if it has one. Its users count must be 0. */
void pw_jm_as_release(struct pw_device *dev, struct pw_jm_vm *jvm);
/* The VM bound to @as, with a reference taken by pw_jm_vm_tryget() under slots_lock (so a VM
 * whose last reference is being dropped is skipped), or NULL. For the MMU fault path
 * (pw_jm_mmu_fault): called from the MMU bottom half, must not wait for the job or GPU bottom
 * halves. The caller drops the reference with pw_jm_vm_put(). */
struct pw_jm_vm *pw_jm_as_vm_get(struct pw_device *dev, uint32_t as);

/* ---- additions (jm-hw) ---- */

/*
 * The AS LRU with the page tables given explicitly. pw_jm_as_get(dev, jvm) is
 * pw_jm_as_get_vm(dev, jvm, pw_jm_vm_mmu(jvm)) with @key marked as a real pw_jm_vm. Here @key
 * only identifies the user (pw_jm_as_put/pw_jm_as_release take the same key) and is never
 * dereferenced; pw_jm_as_vm_get() returns NULL for slots bound this way. For kernel-built jobs
 * on a bare pw_vm (Z2 bring-up self test, host tests) whose address space is not a pw_jm_vm.
 * Same locking and return values as pw_jm_as_get(); PW_EINVAL if @vm is NULL.
 */
int  pw_jm_as_get_vm(struct pw_device *dev, struct pw_jm_vm *key, struct pw_vm *vm);

/*
 * Bring-up / tests: when @fn is set, pw_jm_job_irq_handler() hands each event to @fn instead
 * of pw_jm_sched_job_irq(), so jobs submitted with the pw_jm_slot_* helpers directly never
 * reach the scheduler. Same context as pw_jm_sched_job_irq() (job bottom half). NULL restores
 * the scheduler. Set it while no job of the scheduler is in flight.
 */
typedef void (*pw_jm_irq_hook_t)(struct pw_device *dev, const struct pw_jm_irq_event *ev,
				 void *ctx);
void pw_jm_hw_set_irq_hook(struct pw_device *dev, pw_jm_irq_hook_t fn, void *ctx);

/* The JS_AFFINITY_NEXT value pw_jm_slot_submit() programs: every present shader core
 * (panfrost_job.c:panfrost_job_write_affinity "use all cores for now"). 0 before init. */
uint64_t pw_jm_slot_affinity(struct pw_device *dev);

/* ---- additions (jm-sched) ---- */

/*
 * How many chains the scheduler may keep in one slot (current + NEXT): 2 on GPUs with
 * JOBCHAIN_DISAMBIGUATION (every Bifrost part in the model table), 1 without it, because the
 * _0/_1 stop variants are needed to stop the right one of two chains
 * (panfrost_job.c:panfrost_jm_init credit_limit). 0 before pw_jm_hw_init().
 */
uint32_t pw_jm_slot_depth(struct pw_device *dev);

#endif /* PW_JM_HW_H */
