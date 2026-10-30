/* SPDX-License-Identifier: MIT */
/*
 * pw_jm_sched.h - job-manager scheduling: SUBMIT, JM contexts, per-slot queues, dependencies,
 * completion, timeouts and reset policy. Implemented in pw_jm_sched.c. Owner: jm-sched.
 * Design: docs/status/jm-arch.md.
 *
 * Behaviour reference: Linux drivers/gpu/drm/panfrost (panfrost_drv.c:panfrost_ioctl_submit,
 * panfrost_job.c) on top of drm_sched. GPL-2.0-only: read it for behaviour, never copy code.
 * Citations are file:function, Linux v7.3. drm_sched itself is replaced by a small queue per
 * slot here (the CSF scheduler in pw_sched.c is a different design and is not reused).
 *
 * Model (panfrost_job.c): a job is one job chain (jc) for one slot. JS0 runs fragment chains
 * (PANFROST_JD_REQ_FS), JS1 everything else (panfrost_job_get_slot). Each slot has the running
 * job and at most one queued in the NEXT registers (pfdev->jobs[slot][2]). A job becomes ready
 * when all its dependencies signalled: the in_syncs fences and the implicit-sync fences of its
 * BOs. It then needs its file's AS bound (pw_jm_as_get; PW_EBUSY keeps it waiting). Its
 * pw_fence signals when the chain completes: error 0 on DONE, -EINVAL on a fault, -ECANCELED
 * when stopped/terminated by a context destroy or reset (panfrost_job.c:panfrost_job_handle_err).
 * A job that was STOPPED (soft stop) resumes from JS_TAIL. Timeout 500 ms
 * (panfrost_job.c JOB_TIMEOUT_MS): hard-stop, pw_jm_hw_reset(), the guilty job fails,
 * innocent ones are resubmitted.
 *
 * Contexts: ioctls at passive level in the caller's thread; pw_jm_sched_job_irq() from the job
 * IRQ bottom half (must not sleep on job completion). Releasing a finished job's resources
 * (BO, VM and AS references) must happen in a work item (pw_os_work), not in the bottom half:
 * the last BO reference can unmap pages, which waits on GPU cache flushes (drm_sched frees jobs
 * from its own workqueue too).
 */
#ifndef PW_JM_SCHED_H
#define PW_JM_SCHED_H

#include "pw_file.h"
#include "pw_jm_hw.h"

#include "panfrost_drm.h"

#define PW_JM_JOB_TIMEOUT_MS	500	/* panfrost_job.c JOB_TIMEOUT_MS */

/* ---- device lifetime (pw_device.c, JM only, after pw_jm_mem_init) ---- */

/* dev->jm_sched: slot queues, locks, the timeout/reset work. PW_OK / PW_ENOMEM. */
int  pw_jm_sched_init(struct pw_device *dev);
/* Stop: cancel the timeout work, fail every queued and running job with -ECANCELED (all files
 * are closed by now, so normally none). Idempotent, no-op if init never ran. */
void pw_jm_sched_fini(struct pw_device *dev);
/* Free dev->jm_sched (no bottom half or work item can run any more). */
void pw_jm_sched_release(struct pw_device *dev);
/* For tests and bring-up: timeout for jobs submitted afterwards. No-op without a scheduler. */
void pw_jm_sched_set_job_timeout(struct pw_device *dev, uint32_t timeout_ms);

/* ---- per file ---- */

/* Create the context table with the default context, handle 0, priority MEDIUM
 * (panfrost_job.c:panfrost_jm_open). Runs after pw_sync_file_init(). */
int  pw_jm_sched_file_init(struct pw_file *file);
/* Destroy all contexts of @file: queued jobs are dropped (fence error -ECANCELED), running ones
 * hard-stopped (panfrost_job.c:panfrost_jm_close / panfrost_jm_ctx_destroy). Returns once no
 * job of the file can touch the hardware any more; jobs may still be finishing their
 * bookkeeping in a work item and keep BO/VM references until then. */
void pw_jm_sched_file_fini(struct pw_file *file);

/* ---- ioctl handlers (pw_ioctl_fn, panfrost structs; errnos as Linux panfrost) ---- */

/*
 * SUBMIT (panfrost_drv.c:panfrost_ioctl_submit), in this order:
 *   pad != 0 -> PW_EINVAL; jc == 0 -> PW_EINVAL; requirements & ~(PANFROST_JD_REQ_FS |
 *   PANFROST_JD_REQ_CYCLE_COUNT) -> PW_EINVAL (CYCLE_COUNT is a 1.3 feature, always accepted
 *   at our 1.5); out_sync != 0 and not a syncobj -> PW_ENODEV; jm_ctx_handle unknown ->
 *   PW_EINVAL; in_syncs (u32 handles, binary syncobjs, point 0) copy -> PW_EFAULT, a handle
 *   that is not a syncobj -> PW_ENOENT, a syncobj without a fence -> PW_EINVAL
 *   (drm_syncobj_find_fence; pw_syncobj_find_fence() returns PW_EINVAL for both, so check with
 *   pw_syncobj_lookup() first); bo_handles -> pw_jm_bos_lookup() errors; PW_ENOMEM.
 * Then: slot from requirements; flush id = pw_jm_latest_flush_id(); dependencies = in_syncs
 * fences + every BO's pw_jm_bo_fence_get(); under pw_jm_resv_lock: publish the job fence with
 * pw_jm_bo_fence_set() on all BOs; queue the job on the context's slot; replace out_sync's
 * fence with the job fence (pw_syncobj_replace_fence; binary only, panfrost has no timelines).
 * Nothing user-visible changes on failure. The job holds references on its BOs, its file VM
 * (pw_jm_vm_get(pw_jm_file_vm(file))), its context and, with CYCLE_COUNT, the cycle counter.
 */
int pw_jm_ioctl_submit(struct pw_file *file, void *arg);
/*
 * JM_CTX_CREATE (1.5, panfrost_job.c:panfrost_jm_ctx_create): priority LOW or MEDIUM; HIGH
 * -> PW_EACCES (needs CAP_SYS_NICE / DRM master: never granted on Windows, matching
 * GET_PARAM ALLOWED_JM_CTX_PRIORITIES); anything else -> PW_EINVAL. Returns a new handle >= 1
 * (0 is the default context). PW_ENOMEM.
 */
int pw_jm_ioctl_jm_ctx_create(struct pw_file *file, void *arg);
/* JM_CTX_DESTROY (1.5, panfrost_drv.c:panfrost_ioctl_jm_ctx_destroy): pad != 0 -> PW_EINVAL;
 * handle 0 (default context) -> PW_EINVAL; unknown -> PW_EINVAL. Kills the context's jobs:
 * queued ones fail with -ECANCELED, a job in the NEXT registers is cancelled, a running one
 * hard-stopped (with the JOB_CHAIN_FLAG-selected variant). */
int pw_jm_ioctl_jm_ctx_destroy(struct pw_file *file, void *arg);

/* ---- from pw_jm_hw.c ---- */

/*
 * Job IRQ bottom half (pw_jm_job_irq_handler, after it read and cleared JOB_INT): for each slot
 * in ev->done signal the running job's fence (error 0), promote the NEXT job and queue more;
 * for each slot in ev->failed decode ev->status[js] (STOPPED: requeue from ev->tail; TERMINATED:
 * -ECANCELED; a fault: -EINVAL, log with pw_jm_exception_name) and recover the slot; a fault that
 * leaves the slot unusable schedules a reset (pw_jm_hw_reset from a work item, never from here).
 * Drop the AS user reference of each finished job (pw_jm_as_put) here; free the rest from a
 * work item.
 */
void pw_jm_sched_job_irq(struct pw_device *dev, const struct pw_jm_irq_event *ev);

#endif /* PW_JM_SCHED_H */
