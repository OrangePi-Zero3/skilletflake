// SPDX-License-Identifier: MIT
/*
 * pw_sched.c - CSF scheduling MVP: groups and queues (GROUP_CREATE/DESTROY/GET_STATE),
 * GROUP_SUBMIT, CSG/CS slot programming, job completion, CS fault/fatal, TILER_OOM dispatch and
 * job timeouts.
 *
 * Derived from Linux panthor_sched.c and the GROUP_* parts of panthor_drv.c (dual GPL-2.0/MIT,
 * used under MIT; Copyright 2023 Collabora ltd.). Line references are to the v7.3 sources in
 * references/linux/drivers/gpu/drm/panthor.
 *
 * MVP scope (docs/research/kernel-panthor.md 3.3) and the differences from panthor:
 *  - No tick and no rotation. A group is bound to a free CSG slot when its first job with a
 *    command stream becomes ready (group_bind_locked + csg_slot_prog_locked + CSG START, as
 *    sched.c:2440-2458 does from the tick), and keeps the slot until it is destroyed or can no
 *    longer run. When every slot is taken, ready jobs stay queued until a slot is released:
 *    nothing is returned to userspace, and those jobs have no timeout while they wait.
 *  - CSG priorities are unique per slot (MAX_CSG_PRIO - slot, like the tick's new_csg_prio--),
 *    so the group priority does not order groups against each other yet.
 *  - drm_sched is replaced by an in-order list of jobs per queue. A job waits for its in-fences
 *    (one fence callback at a time, like drm_sched_entity), then queue_run_job()'s instruction
 *    sequence is written into the ring buffer. The ring buffer's free space plays the role of
 *    drm_sched's credit limit. The job fence handed to out-syncobjs is the drm_sched "finished"
 *    fence equivalent: signalled when the queue's syncobj reaches the job's seqno.
 *  - No suspend/resume, GPU reset, devfreq, runtime PM, profiling or protected mode.
 *
 * Locking, outermost first:
 *   sched->lock         slot (un)binding, ring emission, termination, sched->groups
 *   sched->events_lock  job-IRQ processing (bottom half) vs. slot->group changes
 *   queue->lock         pending/in-flight job lists, deadline, last_err
 *   fw.req_lock         FW interface req fields (pw_fw_*_reqs macros)
 * Fence callbacks registered here only store a flag and queue the work item, so fences may be
 * signalled while sched->lock or events_lock is held.
 */

#include "pw_bo.h"
#include "pw_mmu.h"
#include "pw_sched.h"
#include "pw_sync.h"

#define PW_JOB_TIMEOUT_MS		5000	/* sched.c:94 */
#define PW_NUM_INSTRS_PER_CACHE_LINE	8	/* sched.c:98: 64 / sizeof(u64) */
#define PW_MAX_INSTRS_PER_JOB		24	/* sched.c:99 */
#define PW_MAX_GROUPS_PER_POOL		128	/* sched.c:3624 */
#define PW_CSG_UPD_TIMEOUT_MS		100	/* sched.c:2061 */
#define PW_QUEUE_IFACE_SIZE		0x2000u	/* fw.c:498: input at +0, output at +4K */
#define PW_MAX_QUEUE_PRIO		0xf	/* sched.c:390 CSF_MAX_QUEUE_PRIO */
#define PW_CSG_PRIORITY_COUNT		4	/* PANTHOR_CSG_PRIORITY_COUNT */

/* sched.c:330 */
struct pw_syncobj_64b {
	uint64_t seqno;
	uint32_t status;
	uint32_t pad;
};

enum pw_group_state {		/* sched.c:493 enum panthor_group_state */
	PW_GROUP_CREATED = 0,
	PW_GROUP_ACTIVE,
	PW_GROUP_SUSPENDED,
	PW_GROUP_TERMINATED,
	PW_GROUP_UNKNOWN,
};

enum { PW_CB_NONE = 0, PW_CB_ARMED = 1, PW_CB_FIRED = 2 };

struct pw_job {
	struct pw_job *next;
	struct pw_sched *sched;
	struct pw_group *group;		/* reference */
	uint32_t queue_idx;
	uint64_t stream_addr;
	uint32_t stream_size;
	uint32_t latest_flush;
	struct pw_fence *done;		/* drm_sched_fence::finished equivalent (out-syncs) */
	struct pw_fence **deps;		/* in-fences, references */
	uint32_t ndeps, next_dep;
	struct pw_fence_cb cb;		/* registered on deps[next_dep] while cb_state == ARMED */
	uint32_t cb_state;		/* PW_CB_*, __atomic */
	uint64_t seqno;			/* queue syncobj value that completes the job */
	uint64_t ring_start, ring_end;
	int err;			/* error @done is signalled with */
};

struct pw_queue {
	struct pw_group *group;
	uint32_t idx;
	uint8_t priority;
	uint32_t doorbell_id;		/* csg_id + 1 while bound (sched.c:1936) */
	struct pw_bo *ringbuf;
	uint64_t ringbuf_va;		/* in the group's VM */
	uint32_t ringbuf_size;
	uint8_t *ring_kva;
	struct pw_bo *iface_bo;		/* ring-buffer input/output interfaces, MCU VM */
	uint32_t iface_mcu_va;
	struct pw_fw_ringbuf_input_iface *input;
	const struct pw_fw_ringbuf_output_iface *output;

	struct pw_os_mutex *lock;
	struct pw_job *pending, *pending_tail;	/* submitted, not in the ring yet */
	struct pw_job *inflight, *inflight_tail;	/* in the ring (or sync-only), by seqno */
	uint64_t seqno;			/* fence_ctx.seqno: last seqno handed out */
	uint64_t insert;		/* ring write position (published as input->insert) */
	uint64_t deadline_ns;		/* job timeout, 0 = not armed */
	int last_err;			/* error of the last completed job with a stream */
};

struct pw_group {
	uint32_t refcount;		/* __atomic */
	struct pw_device *dev;
	struct pw_sched *sched;
	struct pw_heap_file *heaps;	/* the owning file's heaps (valid while in sched->groups) */
	struct pw_uvm *uvm;		/* reference */
	struct pw_group *next;		/* sched->groups, under sched->lock */

	uint64_t compute_core_mask, fragment_core_mask, tiler_core_mask;
	uint8_t max_compute_cores, max_fragment_cores, max_tiler_cores, priority;

	uint32_t queue_count;
	struct pw_queue *queues[MAX_CS_PER_CSG];

	struct pw_bo *syncobjs;
	uint64_t syncobjs_va;
	struct pw_syncobj_64b *syncobjs_kva;
	struct pw_bo *suspend_buf, *protm_suspend_buf;
	uint32_t suspend_mcu_va, protm_suspend_mcu_va;

	int csg_id;			/* -1 when unbound; changed under sched->lock + events_lock */
	/* enum pw_group_state and the destroyed flag: written under sched->lock with
	 * group_set_state()/__atomic stores, read locklessly by group_can_run() (job_create(),
	 * like panthor_job_create()), hence the atomics. */
	int state;
	bool destroyed;
	bool term_done;			/* termination post-processing ran */
	bool innocent;
	uint32_t fatal_queues;		/* __atomic */
	uint32_t timedout;		/* __atomic */
	uint32_t tiler_oom;		/* __atomic: queues with a TILER_OOM event to process */
};

struct pw_csg_slot {
	struct pw_group *group;		/* reference; sched->lock + events_lock to change */
};

struct pw_sched {
	struct pw_device *dev;
	struct pw_os_mutex *lock;
	struct pw_os_mutex *events_lock;
	struct pw_os_work *work;	/* run ready jobs, terminate, TILER_OOM, timeouts */
	struct pw_csg_slot slots[MAX_CSGS];
	uint32_t csg_slot_count, cs_slot_count, sb_slot_count;
	struct pw_group *groups;	/* live (not destroyed) groups, each holds a reference */
	uint32_t job_timeout_ms;	/* __atomic */
	uint32_t live_groups;		/* __atomic, allocated and not yet released */
};

struct pw_sched_file {
	struct pw_os_mutex *lock;
	struct pw_group *handles[PW_MAX_GROUPS_PER_POOL];	/* handle h at [h - 1] */
	struct pw_heap_file *heaps;
};

#define job_from_cb(c) ((struct pw_job *)((char *)(c) - offsetof(struct pw_job, cb)))

static struct pw_sched *dev_sched(struct pw_device *dev)
{
	return __atomic_load_n(&dev->sched, __ATOMIC_ACQUIRE);
}

/* ------------------------------------------------------------------------------------------ */
/* groups: lifetime                                                                           */
/* ------------------------------------------------------------------------------------------ */

static struct pw_group *group_get(struct pw_group *g)
{
	if (g)
		__atomic_add_fetch(&g->refcount, 1, __ATOMIC_RELAXED);
	return g;
}

static void job_free(struct pw_job *job);

static void queue_free(struct pw_group *g, struct pw_queue *q)
{
	struct pw_os *os = g->dev->os;

	if (!q)
		return;
	/* Every job holds a group reference, so none can be left; be defensive anyway. */
	while (q->pending || q->inflight) {
		struct pw_job *job = q->pending ? q->pending : q->inflight;

		if (job == q->pending)
			q->pending = job->next;
		else
			q->inflight = job->next;
		pw_err(g->dev, "sched: job left on a released queue");
		pw_fence_signal(job->done, PW_ECANCELED);
		job_free(job);
	}
	if (q->ringbuf_va)
		pw_uvm_unmap_kernel_bo(g->uvm, q->ringbuf_va);
	pw_bo_put(q->ringbuf);
	pw_fw_unmap_kernel_mem(g->dev, q->iface_mcu_va);
	pw_bo_put(q->iface_bo);
	if (q->lock)
		pw_os_mutex_destroy(os, q->lock);
	pw_os_free(os, q);
}

/* sched.c:944 group_release_work(). Passive level, no scheduler lock needed. */
static void group_release(struct pw_group *g)
{
	struct pw_device *dev = g->dev;

	for (uint32_t i = 0; i < MAX_CS_PER_CSG; i++)
		queue_free(g, g->queues[i]);
	if (g->syncobjs_va)
		pw_uvm_unmap_kernel_bo(g->uvm, g->syncobjs_va);
	pw_bo_put(g->syncobjs);
	pw_fw_unmap_kernel_mem(dev, g->suspend_mcu_va);
	pw_bo_put(g->suspend_buf);
	pw_fw_unmap_kernel_mem(dev, g->protm_suspend_mcu_va);
	pw_bo_put(g->protm_suspend_buf);
	pw_uvm_put(g->uvm);
	__atomic_sub_fetch(&g->sched->live_groups, 1, __ATOMIC_RELEASE);
	pw_os_free(dev->os, g);
}

static void group_put(struct pw_group *g)
{
	if (g && !__atomic_sub_fetch(&g->refcount, 1, __ATOMIC_ACQ_REL))
		group_release(g);
}

/* sched.c:1002 */
static bool group_can_run(struct pw_group *g)
{
	int state = __atomic_load_n(&g->state, __ATOMIC_ACQUIRE);

	return state != PW_GROUP_TERMINATED && state != PW_GROUP_UNKNOWN &&
	       !__atomic_load_n(&g->destroyed, __ATOMIC_ACQUIRE) &&
	       !__atomic_load_n(&g->fatal_queues, __ATOMIC_ACQUIRE) &&
	       !__atomic_load_n(&g->timedout, __ATOMIC_ACQUIRE);
}

/* sched->lock held (plain reads of g->state are fine under it). */
static void group_set_state(struct pw_group *g, int state)
{
	__atomic_store_n(&g->state, state, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------------------------------ */
/* jobs                                                                                       */
/* ------------------------------------------------------------------------------------------ */

static void job_free(struct pw_job *job)
{
	struct pw_os *os = job->group->dev->os;

	for (uint32_t i = 0; i < job->ndeps; i++)
		pw_fence_put(job->deps[i]);
	pw_os_free(os, job->deps);
	pw_fence_put(job->done);
	group_put(job->group);
	pw_os_free(os, job);
}

/* Fence callback: runs in the signaller's context, must not block. */
static void job_dep_cb(struct pw_fence *f, struct pw_fence_cb *cb)
{
	struct pw_job *job = job_from_cb(cb);
	struct pw_sched *s = job->sched;

	(void)f;
	/* Last access to @job: the canceller may free it as soon as it sees FIRED. */
	__atomic_store_n(&job->cb_state, PW_CB_FIRED, __ATOMIC_RELEASE);
	pw_os_work_queue(s->work);
}

/* Make sure the dependency callback is neither registered nor running. */
static void job_disarm(struct pw_job *job)
{
	uint32_t st = __atomic_load_n(&job->cb_state, __ATOMIC_ACQUIRE);

	if (st == PW_CB_ARMED && !pw_fence_remove_callback(job->deps[job->next_dep], &job->cb)) {
		/* Signalled: the callback ran or is running. Its window is a few instructions. */
		while (__atomic_load_n(&job->cb_state, __ATOMIC_ACQUIRE) != PW_CB_FIRED)
			pw_os_udelay(job->group->dev->os, 1);
	}
	__atomic_store_n(&job->cb_state, PW_CB_NONE, __ATOMIC_RELAXED);
}

/* True when every in-fence is signalled. Otherwise a callback is armed on the first pending one
 * (drm_sched_entity_add_dependency_cb() equivalent). queue->lock held. */
static bool job_deps_ready(struct pw_job *job)
{
	if (__atomic_load_n(&job->cb_state, __ATOMIC_ACQUIRE) == PW_CB_ARMED) {
		if (!pw_fence_is_signalled(job->deps[job->next_dep]))
			return false;
		job_disarm(job);
	} else {
		__atomic_store_n(&job->cb_state, PW_CB_NONE, __ATOMIC_RELAXED);
	}
	while (job->next_dep < job->ndeps) {
		struct pw_fence *f = job->deps[job->next_dep];

		if (pw_fence_is_signalled(f)) {
			job->next_dep++;
			continue;
		}
		__atomic_store_n(&job->cb_state, PW_CB_ARMED, __ATOMIC_RELEASE);
		if (pw_fence_add_callback(f, &job->cb, job_dep_cb) == PW_ENOENT) {
			__atomic_store_n(&job->cb_state, PW_CB_NONE, __ATOMIC_RELAXED);
			job->next_dep++;
			continue;
		}
		return false;
	}
	return true;
}

static void job_finish(struct pw_job *job, int err)
{
	pw_fence_signal(job->done, err);
	job_free(job);
}

/* Cancel a detached list of jobs that never reached the ring. */
static void job_cancel_list(struct pw_job *job, int err)
{
	while (job) {
		struct pw_job *next = job->next;

		job_disarm(job);
		job_finish(job, err);
		job = next;
	}
}

static void queue_cancel_pending(struct pw_queue *q, int err)
{
	struct pw_job *pending;

	pw_os_mutex_lock(q->lock);
	pending = q->pending;
	q->pending = q->pending_tail = NULL;
	pw_os_mutex_unlock(q->lock);
	job_cancel_list(pending, err);
}

static void list_append(struct pw_job **head, struct pw_job **tail, struct pw_job *job)
{
	job->next = NULL;
	if (*tail)
		(*tail)->next = job;
	else
		*head = job;
	*tail = job;
}

static struct pw_job *list_pop(struct pw_job **head, struct pw_job **tail)
{
	struct pw_job *job = *head;

	if (job) {
		*head = job->next;
		if (!*head)
			*tail = NULL;
		job->next = NULL;
	}
	return job;
}

static uint32_t job_timeout_ms(struct pw_sched *s)
{
	return __atomic_load_n(&s->job_timeout_ms, __ATOMIC_RELAXED);
}

/*
 * sched.c:1682 queue_check_job_completion(): signal the in-flight jobs whose seqno the queue's
 * syncobj reached. Jobs without a stream complete with the error of the last stream job before
 * them (drm_sched propagates the parent fence error). Returns true on progress. Any context
 * that may block briefly (bottom half, work, ioctl); takes queue->lock.
 */
static bool queue_check_completion(struct pw_queue *q)
{
	struct pw_group *g = q->group;
	struct pw_job *done = NULL, *done_tail = NULL, *job;
	uint64_t seqno;
	bool progress = false, kick;

	pw_rmb();
	seqno = PW_READ_ONCE(g->syncobjs_kva[q->idx].seqno);

	pw_os_mutex_lock(q->lock);
	while ((job = q->inflight) && job->seqno <= seqno) {
		list_pop(&q->inflight, &q->inflight_tail);
		if (job->stream_size)
			q->last_err = job->err;
		else
			job->err = q->last_err;
		list_append(&done, &done_tail, job);
	}
	if (!q->inflight) {
		q->deadline_ns = 0;	/* queue_suspend_timeout_locked() */
		progress = true;
	} else if (done) {
		/* queue_reset_timeout_locked() */
		q->deadline_ns = pw_os_time_ns(g->dev->os) +
				 (uint64_t)job_timeout_ms(g->sched) * 1000000u;
		progress = true;
	}
	/* Ring space was freed: jobs waiting for it can go (drm_sched credits). */
	kick = done && q->pending;
	pw_os_mutex_unlock(q->lock);

	while ((job = list_pop(&done, &done_tail)))
		job_finish(job, job->err);
	if (kick)
		pw_os_work_queue(g->sched->work);
	return progress;
}

static void group_check_completion(struct pw_group *g)
{
	for (uint32_t i = 0; i < g->queue_count; i++)
		queue_check_completion(g->queues[i]);
}

/* ------------------------------------------------------------------------------------------ */
/* CSG/CS slot programming                                                                    */
/* ------------------------------------------------------------------------------------------ */

/* sched.c:1108 cs_slot_prog_locked() */
static void cs_slot_prog_locked(struct pw_sched *s, uint32_t csg_id, uint32_t cs_id)
{
	struct pw_device *dev = s->dev;
	struct pw_group *g = s->slots[csg_id].group;
	struct pw_queue *q = g->queues[cs_id];
	struct pw_fw_cs_iface *cs = &dev->fw.cs[csg_id][cs_id];

	PW_WRITE_ONCE(q->input->extract, PW_READ_ONCE(q->output->extract));
	PW_WRITE_ONCE(cs->input->ringbuf_base, q->ringbuf_va);
	PW_WRITE_ONCE(cs->input->ringbuf_size, q->ringbuf_size);
	PW_WRITE_ONCE(cs->input->ringbuf_input, q->iface_mcu_va);
	PW_WRITE_ONCE(cs->input->ringbuf_output, (uint64_t)q->iface_mcu_va + PW_SZ_4K);
	PW_WRITE_ONCE(cs->input->config,
		      CS_CONFIG_PRIORITY(q->priority) | CS_CONFIG_DOORBELL(q->doorbell_id));
	PW_WRITE_ONCE(cs->input->ack_irq_mask, ~0u);
	pw_fw_update_reqs(dev, cs, req, CS_IDLE_SYNC_WAIT | CS_IDLE_EMPTY | CS_STATE_START,
			  CS_IDLE_SYNC_WAIT | CS_IDLE_EMPTY | CS_STATE_MASK);
}

/* fw.c:349 panthor_fw_csg_endpoint_req_set() */
static void csg_endpoint_req_set(struct pw_device *dev, struct pw_fw_csg_iface *csg, uint64_t v)
{
	if (PW_READ_ONCE(dev->fw.glb.control->version) >= CSF_IFACE_VERSION(4, 0, 0))
		PW_WRITE_ONCE(csg->input->endpoint_req2, v);
	else
		PW_WRITE_ONCE(csg->input->endpoint_req, (uint32_t)v);
}

/* sched.c:1339 csg_slot_prog_locked() */
static void csg_slot_prog_locked(struct pw_sched *s, uint32_t csg_id, uint32_t priority)
{
	struct pw_device *dev = s->dev;
	struct pw_group *g = s->slots[csg_id].group;
	struct pw_fw_csg_iface *csg = &dev->fw.csg[csg_id];
	uint32_t queue_mask = 0;

	for (uint32_t i = 0; i < g->queue_count; i++) {
		cs_slot_prog_locked(s, csg_id, i);
		queue_mask |= PW_BIT(i);
	}
	PW_WRITE_ONCE(csg->input->allow_compute, g->compute_core_mask);
	PW_WRITE_ONCE(csg->input->allow_fragment, g->fragment_core_mask);
	PW_WRITE_ONCE(csg->input->allow_other, (uint32_t)g->tiler_core_mask);
	csg_endpoint_req_set(dev, csg, CSG_EP_REQ_COMPUTE(g->max_compute_cores) |
				       CSG_EP_REQ_FRAGMENT(g->max_fragment_cores) |
				       CSG_EP_REQ_TILER(g->max_tiler_cores) |
				       CSG_EP_REQ_PRIORITY(priority));
	PW_WRITE_ONCE(csg->input->config, (uint32_t)pw_vm_as(pw_uvm_vm(g->uvm)));
	PW_WRITE_ONCE(csg->input->suspend_buf, (uint64_t)g->suspend_mcu_va);
	PW_WRITE_ONCE(csg->input->protm_suspend_buf, (uint64_t)g->protm_suspend_mcu_va);
	PW_WRITE_ONCE(csg->input->ack_irq_mask, ~0u);
	pw_fw_toggle_reqs(dev, csg, doorbell_req, doorbell_ack, queue_mask);
}

/* sched.c:1265 csg_slot_sync_state_locked(), restricted to what an unrotated slot sees. */
static void csg_slot_sync_state_locked(struct pw_sched *s, uint32_t csg_id)
{
	struct pw_device *dev = s->dev;
	struct pw_group *g = s->slots[csg_id].group;
	uint32_t csg_state;
	int new_state;

	if (!g)
		return;
	csg_state = PW_READ_ONCE(dev->fw.csg[csg_id].output->ack) & CSG_STATE_MASK;
	switch (csg_state) {
	case CSG_STATE_START:
	case CSG_STATE_RESUME:
		new_state = PW_GROUP_ACTIVE;
		break;
	case CSG_STATE_TERMINATE:
		new_state = PW_GROUP_TERMINATED;
		break;
	case CSG_STATE_SUSPEND:
		new_state = PW_GROUP_SUSPENDED;
		break;
	default:
		pw_err(dev, "Invalid state on CSG %u (state=%u)", csg_id, csg_state);
		new_state = PW_GROUP_UNKNOWN;
		break;
	}
	if (new_state == g->state)
		return;
	if (g->state == PW_GROUP_ACTIVE) {
		/* sched.c:1324-1332 cs_slot_reset_locked(): STOP every queue slot. */
		for (uint32_t i = 0; i < g->queue_count; i++)
			pw_fw_update_reqs(dev, &dev->fw.cs[csg_id][i], req, CS_STATE_STOP,
					  CS_STATE_MASK);
	}
	group_set_state(g, new_state);
}

/* Apply CSG req changes on one slot and wait for the FW (sched.c:2028 csgs_upd_ctx_apply). */
static int csg_slot_update_locked(struct pw_sched *s, uint32_t csg_id, uint32_t value,
				  uint32_t mask)
{
	struct pw_device *dev = s->dev;
	struct pw_fw_csg_iface *csg = &dev->fw.csg[csg_id];
	uint32_t acked = 0;	/* left unset by pw_fw_csg_wait_acks()'s early PW_EINVAL returns */
	int ret;

	pw_fw_update_reqs(dev, csg, req, value, mask);
	pw_fw_ring_csg_doorbells(dev, PW_BIT(csg_id));
	ret = pw_fw_csg_wait_acks(dev, csg_id, mask, &acked, PW_CSG_UPD_TIMEOUT_MS);
	if (acked & CSG_STATE_MASK)
		csg_slot_sync_state_locked(s, csg_id);
	if (ret)
		pw_err(dev, "CSG %u update request timedout (req 0x%08x ack 0x%08x)", csg_id,
		       PW_READ_ONCE(csg->input->req), PW_READ_ONCE(csg->output->ack));
	return ret;
}

static void process_csg_irq_locked(struct pw_sched *s, uint32_t csg_id, bool *need_work);

/*
 * sched.c:1910 group_bind_locked() + the tick's slot programming (sched.c:2440-2458). The VM is
 * activated first (panthor_vm_active()). PW_EBUSY: no free CSG slot or AS (retried later).
 */
static int group_bind_locked(struct pw_sched *s, struct pw_group *g)
{
	struct pw_device *dev = s->dev;
	struct pw_fw_csg_iface *csg;
	uint32_t csg_id;
	int ret;

	if (g->csg_id >= 0)
		return PW_OK;
	for (csg_id = 0; csg_id < s->csg_slot_count && s->slots[csg_id].group; csg_id++)
		;
	if (csg_id == s->csg_slot_count)
		return PW_EBUSY;

	ret = pw_uvm_activate(g->uvm);
	if (ret == PW_EINVAL) {
		/* Unusable VM: sched.c:2215-2220 flags every queue of such a group as faulty. */
		pw_warn(dev, "sched: group VM is unusable, failing the group");
		__atomic_store_n(&g->fatal_queues, PW_GENMASK(g->queue_count - 1, 0),
				 __ATOMIC_RELEASE);
		return ret;
	}
	if (ret)
		return ret;

	group_get(g);	/* the slot's reference */
	for (uint32_t i = 0; i < g->queue_count; i++)
		g->queues[i]->doorbell_id = csg_id + 1;
	pw_os_mutex_lock(s->events_lock);
	s->slots[csg_id].group = g;
	g->csg_id = (int)csg_id;
	pw_os_mutex_unlock(s->events_lock);

	csg = &dev->fw.csg[csg_id];
	csg_slot_prog_locked(s, csg_id, MAX_CSG_PRIO - csg_id);
	ret = csg_slot_update_locked(s, csg_id,
				     CSG_STATE_START |
				     ((PW_READ_ONCE(csg->output->ack) ^ CSG_ENDPOINT_CONFIG) &
				      CSG_ENDPOINT_CONFIG),
				     CSG_STATE_MASK | CSG_ENDPOINT_CONFIG);
	if (ret || g->state != PW_GROUP_ACTIVE) {
		/* panthor schedules a reset; we flag the group so it gets terminated. */
		if (g->state != PW_GROUP_ACTIVE)
			group_set_state(g, PW_GROUP_UNKNOWN);
		return ret ? ret : PW_EIO;
	}
	pw_dbg(dev, "sched: group bound to CSG %u (AS%d)", csg_id, pw_vm_as(pw_uvm_vm(g->uvm)));
	return PW_OK;
}

/* sched.c:1952 group_unbind_locked(): process what the slot still has pending, free it. */
static void group_unbind_locked(struct pw_sched *s, struct pw_group *g)
{
	bool need_work = false;

	if (g->csg_id < 0)
		return;
	pw_os_mutex_lock(s->events_lock);
	process_csg_irq_locked(s, (uint32_t)g->csg_id, &need_work);
	s->slots[g->csg_id].group = NULL;
	g->csg_id = -1;
	pw_os_mutex_unlock(s->events_lock);
	__atomic_store_n(&g->tiler_oom, 0, __ATOMIC_RELAXED);
	for (uint32_t i = 0; i < g->queue_count; i++)
		g->queues[i]->doorbell_id = 0;
	group_put(g);	/* the slot's reference; the caller holds another one */
}

/*
 * Stop a group that can no longer run: CSG TERMINATE + unbind (the tick's eviction path,
 * sched.c:2366-2378), then group_term_post_processing() (sched.c:2237): every in-flight job is
 * signalled with -EINVAL (fatal queue), -ETIMEDOUT (timed out group) or -ECANCELED, and the
 * syncobjs are bumped to the last seqno with status ~0 so GPU-side waiters don't hang. Jobs
 * that never reached the ring are cancelled with -ECANCELED (what queue_run_job() returns for
 * a group that can't run, sched.c:3317). sched->lock held.
 */
static void group_terminate_locked(struct pw_sched *s, struct pw_group *g)
{
	struct pw_device *dev = s->dev;

	if (g->term_done)
		return;
	if (g->csg_id >= 0) {
		uint32_t csg_id = (uint32_t)g->csg_id;

		if (csg_slot_update_locked(s, csg_id, CSG_STATE_TERMINATE, CSG_STATE_MASK))
			group_set_state(g, PW_GROUP_UNKNOWN);	/* panthor would reset the GPU */
		group_unbind_locked(s, g);
	}
	if (g->state != PW_GROUP_UNKNOWN)
		group_set_state(g, PW_GROUP_TERMINATED);

	for (uint32_t i = 0; i < g->queue_count; i++) {
		struct pw_queue *q = g->queues[i];
		struct pw_job *inflight, *pending, *job;
		int err;

		if (__atomic_load_n(&g->fatal_queues, __ATOMIC_ACQUIRE) & PW_BIT(i))
			err = PW_EINVAL;
		else if (__atomic_load_n(&g->timedout, __ATOMIC_ACQUIRE))
			err = PW_ETIMEDOUT;
		else
			err = PW_ECANCELED;

		pw_os_mutex_lock(q->lock);
		inflight = q->inflight;
		pending = q->pending;
		q->inflight = q->inflight_tail = NULL;
		q->pending = q->pending_tail = NULL;
		q->deadline_ns = 0;
		q->last_err = err;
		PW_WRITE_ONCE(g->syncobjs_kva[i].status, ~0u);
		PW_WRITE_ONCE(g->syncobjs_kva[i].seqno, q->seqno);
		pw_wmb();
		pw_os_mutex_unlock(q->lock);

		while ((job = inflight)) {
			inflight = job->next;
			/* A CS_FAULT error already recorded on the job wins (dma_fence_set_error()
			 * is sticky in panthor's eyes: the first error set is reported). */
			job_finish(job, job->err ? job->err : err);
		}
		job_cancel_list(pending, PW_ECANCELED);
	}
	g->term_done = true;
	pw_dbg(dev, "sched: group terminated (fatal 0x%x, timedout %u)",
	       __atomic_load_n(&g->fatal_queues, __ATOMIC_RELAXED),
	       __atomic_load_n(&g->timedout, __ATOMIC_RELAXED));
	/* A slot may have been released: let waiting groups bind. */
	pw_os_work_queue(s->work);
}

/* ------------------------------------------------------------------------------------------ */
/* ring buffer                                                                                */
/* ------------------------------------------------------------------------------------------ */

/* sched.c:3195 prepare_job_instrs() with profiling disabled. Returns the instruction count,
 * padded to a 64-byte cache line with NOPs (zeroes). */
static uint32_t prepare_job_instrs(struct pw_sched *s, struct pw_queue *q, struct pw_job *job,
				   uint64_t *buf)
{
	const struct pw_device *dev = s->dev;
	uint64_t addr_reg = dev->csif_info.cs_reg_count - dev->csif_info.unpreserved_cs_reg_count;
	uint64_t val_reg = addr_reg + 2;
	uint64_t sync_addr = q->group->syncobjs_va + q->idx * sizeof(struct pw_syncobj_64b);
	uint64_t waitall_mask = s->sb_slot_count ? PW_GENMASK64(s->sb_slot_count - 1, 0) : 0;
	const uint64_t imm48 = PW_GENMASK64(47, 0);
	uint32_t n = 0;

	/* MOV32 rX+2, cs.latest_flush */
	buf[n++] = (2ull << 56) | (val_reg << 48) | job->latest_flush;
	/* FLUSH_CACHE2.clean_inv_all.no_wait.signal(0) rX+2: the GPU skips the flush when one
	 * happened since latest_flush (flush reduction, sched.c:3204). */
	buf[n++] = (36ull << 56) | (0ull << 48) | (val_reg << 40) | (0ull << 16) | 0x233;
	/* MOV48 rX:rX+1, cs.start */
	buf[n++] = (1ull << 56) | (addr_reg << 48) | (job->stream_addr & imm48);
	/* MOV32 rX+2, cs.size */
	buf[n++] = (2ull << 56) | (val_reg << 48) | job->stream_size;
	/* WAIT(0) => waits for FLUSH_CACHE2 instruction */
	buf[n++] = (3ull << 56) | (1ull << 16);
	/* CALL rX:rX+1, rX+2 */
	buf[n++] = (32ull << 56) | (addr_reg << 40) | (val_reg << 32);
	/* MOV48 rX:rX+1, sync_addr */
	buf[n++] = (1ull << 56) | (addr_reg << 48) | (sync_addr & imm48);
	/* MOV48 rX+2, #1 */
	buf[n++] = (1ull << 56) | (val_reg << 48) | 1;
	/* WAIT(all) */
	buf[n++] = (3ull << 56) | (waitall_mask << 16);
	/* SYNC_ADD64.system_scope.propage_err.nowait rX:rX+1, rX+2 */
	buf[n++] = (51ull << 56) | (0ull << 48) | (addr_reg << 40) | (val_reg << 32) |
		   (0ull << 16) | 1;
	/* ERROR_BARRIER, so we can recover from faults at job boundaries. */
	buf[n++] = 47ull << 56;

	while (n % PW_NUM_INSTRS_PER_CACHE_LINE)
		buf[n++] = 0;
	return n;
}

/* sched.c:3125 copy_instrs_to_ringbuf() */
static void copy_instrs_to_ringbuf(struct pw_device *dev, struct pw_queue *q, uint64_t start,
				   const uint64_t *buf, uint32_t count)
{
	uint64_t size = q->ringbuf_size, off = start & (size - 1);
	uint64_t bytes = (uint64_t)count * sizeof(uint64_t);
	uint64_t written = PW_MIN(size - off, bytes);

	pw_memcpy(q->ring_kva + off, buf, (size_t)written);
	if (written < bytes)
		pw_memcpy(q->ring_kva, (const uint8_t *)buf + written, (size_t)(bytes - written));
	pw_os_mem_flush(dev->os, pw_bo_os_mem(q->ringbuf), 0, q->ringbuf_size);
}

/* Ring bytes still owned by the GPU: from the oldest in-flight stream job. queue->lock held. */
static uint64_t ring_used_locked(struct pw_queue *q)
{
	for (struct pw_job *j = q->inflight; j; j = j->next) {
		if (j->stream_size)
			return q->insert - j->ring_start;
	}
	return 0;
}

/*
 * Move ready jobs from the head of @q's pending list into the ring buffer, in order
 * (queue_run_job(), sched.c:3290-3388). sched->lock held.
 */
static void queue_run_locked(struct pw_sched *s, struct pw_queue *q)
{
	struct pw_device *dev = s->dev;
	struct pw_group *g = q->group;
	uint64_t buf[PW_MAX_INSTRS_PER_JOB];

	for (;;) {
		struct pw_job *job;
		bool arm_timer = false;
		uint32_t count;
		int ret;

		pw_os_mutex_lock(q->lock);
		job = q->pending;
		if (!job || !job_deps_ready(job)) {
			pw_os_mutex_unlock(q->lock);
			return;
		}
		if (!job->stream_size) {
			/* sched.c:3306: an empty stream completes with the queue's last job. */
			list_pop(&q->pending, &q->pending_tail);
			job->seqno = q->seqno;
			list_append(&q->inflight, &q->inflight_tail, job);
			pw_os_mutex_unlock(q->lock);
			queue_check_completion(q);
			continue;
		}
		if (!group_can_run(g)) {
			list_pop(&q->pending, &q->pending_tail);
			pw_os_mutex_unlock(q->lock);
			job_finish(job, PW_ECANCELED);
			continue;
		}
		pw_os_mutex_unlock(q->lock);

		/* The head job can't change under sched->lock: only we pop it. */
		ret = group_bind_locked(s, g);
		if (ret) {
			if (ret != PW_EBUSY)
				pw_os_work_queue(s->work);	/* let the work terminate it */
			return;
		}

		pw_os_mutex_lock(q->lock);
		count = prepare_job_instrs(s, q, job, buf);
		if (ring_used_locked(q) + count * sizeof(uint64_t) > q->ringbuf_size) {
			/* No room: a completion queues the work again. */
			pw_os_mutex_unlock(q->lock);
			return;
		}
		list_pop(&q->pending, &q->pending_tail);
		job->seqno = ++q->seqno;
		job->ring_start = q->insert;
		job->ring_end = q->insert + count * sizeof(uint64_t);
		copy_instrs_to_ringbuf(dev, q, job->ring_start, buf, count);
		if (!q->inflight) {
			q->deadline_ns = pw_os_time_ns(dev->os) +
					 (uint64_t)job_timeout_ms(s) * 1000000u;
			arm_timer = true;
		}
		list_append(&q->inflight, &q->inflight_tail, job);
		q->insert = job->ring_end;
		/* Make sure the ring buffer is updated before the INSERT register. */
		pw_wmb();
		PW_WRITE_ONCE(q->input->extract, PW_READ_ONCE(q->output->extract));
		PW_WRITE_ONCE(q->input->insert, q->insert);
		pw_os_mutex_unlock(q->lock);

		pw_fw_ring_doorbell(dev, q->doorbell_id);
		if (arm_timer)
			pw_os_work_queue_delayed(s->work, job_timeout_ms(s));
	}
}

static void group_run_locked(struct pw_sched *s, struct pw_group *g)
{
	for (uint32_t i = 0; i < g->queue_count; i++)
		queue_run_locked(s, g->queues[i]);
}

/* ------------------------------------------------------------------------------------------ */
/* events (job IRQ bottom half)                                                               */
/* ------------------------------------------------------------------------------------------ */

/* sched.c:1397 cs_slot_process_fatal_event_locked() */
static void cs_process_fatal_locked(struct pw_sched *s, struct pw_group *g, uint32_t csg_id,
				    uint32_t cs_id)
{
	struct pw_device *dev = s->dev;
	const struct pw_fw_cs_iface *cs = &dev->fw.cs[csg_id][cs_id];
	uint32_t fatal = PW_READ_ONCE(cs->output->fatal);
	uint64_t info = PW_READ_ONCE(cs->output->fatal_info);

	if (g)
		__atomic_or_fetch(&g->fatal_queues, PW_BIT(cs_id), __ATOMIC_ACQ_REL);
	pw_warn(dev, "CSG slot %u CS slot: %u CS_FATAL.EXCEPTION_TYPE: 0x%x (%s) "
		"CS_FATAL.EXCEPTION_DATA: 0x%x CS_FATAL_INFO.EXCEPTION_DATA: 0x%llx", csg_id, cs_id,
		CS_EXCEPTION_TYPE(fatal), pw_exception_name(CS_EXCEPTION_TYPE(fatal)),
		CS_EXCEPTION_DATA(fatal), (unsigned long long)info);
	/* CS_UNRECOVERABLE asks for a GPU reset in panthor; we have none (the group still gets
	 * terminated, the FW may be wedged). */
	if (CS_EXCEPTION_TYPE(fatal) == PW_EXC_CS_UNRECOVERABLE)
		pw_err(dev, "CS_UNRECOVERABLE: a GPU reset is needed (not implemented)");
}

/* sched.c:1443 cs_slot_process_fault_event_locked() */
static void cs_process_fault_locked(struct pw_sched *s, struct pw_group *g, uint32_t csg_id,
				    uint32_t cs_id)
{
	struct pw_device *dev = s->dev;
	const struct pw_fw_cs_iface *cs = &dev->fw.cs[csg_id][cs_id];
	uint32_t fault = PW_READ_ONCE(cs->output->fault);
	uint64_t info = PW_READ_ONCE(cs->output->fault_info);

	if (g && cs_id < g->queue_count) {
		struct pw_queue *q = g->queues[cs_id];
		uint64_t extract = PW_READ_ONCE(q->output->extract);

		pw_os_mutex_lock(q->lock);
		for (struct pw_job *j = q->inflight; j; j = j->next) {
			if (!j->stream_size || extract >= j->ring_end)
				continue;
			if (extract < j->ring_start)
				break;
			if (!j->err)
				j->err = PW_EINVAL;
		}
		pw_os_mutex_unlock(q->lock);
	}
	pw_warn(dev, "CSG slot %u CS slot: %u CS_FAULT.EXCEPTION_TYPE: 0x%x (%s) "
		"CS_FAULT.EXCEPTION_DATA: 0x%x CS_FAULT_INFO.EXCEPTION_DATA: 0x%llx", csg_id, cs_id,
		CS_EXCEPTION_TYPE(fault), pw_exception_name(CS_EXCEPTION_TYPE(fault)),
		CS_EXCEPTION_DATA(fault), (unsigned long long)info);
}

/* sched.c:1623 cs_slot_process_irq_locked(). Returns true if the CS doorbell must be rung. */
static bool cs_process_irq_locked(struct pw_sched *s, struct pw_group *g, uint32_t csg_id,
				  uint32_t cs_id, bool *need_work)
{
	struct pw_device *dev = s->dev;
	struct pw_fw_cs_iface *cs = &dev->fw.cs[csg_id][cs_id];
	uint32_t req, ack, events;

	if (!cs->input)
		return false;
	req = PW_READ_ONCE(cs->input->req);
	ack = PW_READ_ONCE(cs->output->ack);
	events = (req ^ ack) & CS_EVT_MASK;

	if (events & CS_FATAL) {
		cs_process_fatal_locked(s, g, csg_id, cs_id);
		*need_work = true;
	}
	if (events & CS_FAULT)
		cs_process_fault_locked(s, g, csg_id, cs_id);
	if ((events & CS_TILER_OOM) && g) {
		/* sched.c:1600: handled from the work (memory allocation). */
		__atomic_or_fetch(&g->tiler_oom, PW_BIT(cs_id), __ATOMIC_ACQ_REL);
		*need_work = true;
	}
	/* TILER_OOM is acked once the heap grew. */
	pw_fw_update_reqs(dev, cs, req, ack, CS_FATAL | CS_FAULT);
	return (events & (CS_FAULT | CS_TILER_OOM)) != 0;
}

/* sched.c:1788 sched_process_csg_irq_locked(). events_lock held. */
static void process_csg_irq_locked(struct pw_sched *s, uint32_t csg_id, bool *need_work)
{
	struct pw_device *dev = s->dev;
	struct pw_fw_csg_iface *csg;
	struct pw_group *g;
	uint32_t req, ack, cs_irq_req, cs_irq_ack, csg_events, cs_irqs, ring_mask = 0;

	if (csg_id >= s->csg_slot_count)
		return;
	csg = &dev->fw.csg[csg_id];
	if (!csg->input)
		return;
	g = s->slots[csg_id].group;

	pw_rmb();
	req = PW_READ_ONCE(csg->input->req);
	ack = PW_READ_ONCE(csg->output->ack);
	cs_irq_req = PW_READ_ONCE(csg->output->cs_irq_req);
	cs_irq_ack = PW_READ_ONCE(csg->input->cs_irq_ack);
	csg_events = (req ^ ack) & CSG_EVT_MASK;

	if (req != ack || cs_irq_req != cs_irq_ack) {
		/* Ack the CS IRQs first so a new one raised meanwhile is not lost (sched.c:1810). */
		PW_WRITE_ONCE(csg->input->cs_irq_ack, cs_irq_req);
		pw_fw_update_reqs(dev, csg, req, ack,
				  CSG_SYNC_UPDATE | CSG_IDLE | CSG_PROGRESS_TIMER_EVENT);

		if (csg_events & CSG_PROGRESS_TIMER_EVENT) {
			/* sched.c:1766 */
			pw_warn(dev, "CSG slot %u progress timeout", csg_id);
			if (g)
				__atomic_store_n(&g->timedout, 1, __ATOMIC_RELEASE);
			*need_work = true;
		}
		cs_irqs = cs_irq_req ^ cs_irq_ack;
		while (cs_irqs) {
			uint32_t cs_id = pw_ffs32(cs_irqs) - 1;

			if (cs_process_irq_locked(s, g, csg_id, cs_id, need_work))
				ring_mask |= PW_BIT(cs_id);
			cs_irqs &= ~PW_BIT(cs_id);
		}
		if (ring_mask)
			pw_fw_toggle_reqs(dev, csg, doorbell_req, doorbell_ack, ring_mask);
		pw_fw_ring_csg_doorbells(dev, PW_BIT(csg_id));
	}
	/* sched.c:1752 csg_slot_sync_update_locked(): checked on every CSG IRQ, not only on
	 * SYNC_UPDATE, so a coalesced event can't leave a fence behind. */
	if (g)
		group_check_completion(g);
}

void pw_sched_job_irq(struct pw_device *dev, uint32_t csg_mask)
{
	struct pw_sched *s = dev_sched(dev);
	bool need_work = false;

	if (!s)
		return;
	pw_os_mutex_lock(s->events_lock);
	while (csg_mask) {
		uint32_t csg_id = pw_ffs32(csg_mask) - 1;

		process_csg_irq_locked(s, csg_id, &need_work);
		csg_mask &= ~PW_BIT(csg_id);
	}
	pw_os_mutex_unlock(s->events_lock);
	if (need_work)
		pw_os_work_queue(s->work);
}

/* ------------------------------------------------------------------------------------------ */
/* work: ready jobs, termination, tiler OOM, timeouts                                         */
/* ------------------------------------------------------------------------------------------ */

/* sched.c:1495 group_process_tiler_oom(). sched->lock held. */
static void group_process_tiler_oom_locked(struct pw_sched *s, struct pw_group *g)
{
	struct pw_device *dev = s->dev;
	uint32_t oom = __atomic_exchange_n(&g->tiler_oom, 0, __ATOMIC_ACQ_REL);

	while (oom) {
		uint32_t cs_id = pw_ffs32(oom) - 1;
		uint32_t vt_start, vt_end, frag_end, csg_id;
		struct pw_fw_cs_iface *cs;
		uint64_t heap_address, new_chunk_va = 0;
		int ret;

		oom &= ~PW_BIT(cs_id);
		/* Scheduled out meanwhile: the FW re-raises the event when it runs again. */
		if (g->csg_id < 0)
			continue;
		csg_id = (uint32_t)g->csg_id;
		cs = &dev->fw.cs[csg_id][cs_id];
		heap_address = PW_READ_ONCE(cs->output->heap_address);
		vt_start = PW_READ_ONCE(cs->output->heap_vt_start);
		vt_end = PW_READ_ONCE(cs->output->heap_vt_end);
		frag_end = PW_READ_ONCE(cs->output->heap_frag_end);

		if (frag_end > vt_end || vt_end >= vt_start)
			ret = PW_EINVAL;
		else
			ret = pw_heap_grow(g->heaps, g->uvm, heap_address, vt_start - frag_end,
					   vt_end - frag_end, &new_chunk_va);
		/* ENOMEM: let the FW reclaim memory (wait for fragment jobs, or the exception
		 * handler) with heap_start == heap_end == 0. */
		if (ret && ret != PW_ENOMEM) {
			pw_warn(dev, "Failed to extend the tiler heap (%d)", ret);
			__atomic_or_fetch(&g->fatal_queues, PW_BIT(cs_id), __ATOMIC_ACQ_REL);
			continue;
		}
		PW_WRITE_ONCE(cs->input->heap_start, new_chunk_va);
		PW_WRITE_ONCE(cs->input->heap_end, new_chunk_va);
		pw_fw_update_reqs(dev, cs, req, PW_READ_ONCE(cs->output->ack), CS_TILER_OOM);
		pw_fw_toggle_reqs(dev, &dev->fw.csg[csg_id], doorbell_req, doorbell_ack,
				  PW_BIT(cs_id));
		pw_fw_ring_csg_doorbells(dev, PW_BIT(csg_id));
	}
}

static void sched_work_fn(void *ctx)
{
	struct pw_sched *s = ctx;
	struct pw_device *dev = s->dev;
	uint64_t now, next = UINT64_MAX;

	pw_os_mutex_lock(s->lock);
	for (struct pw_group *g = s->groups; g; g = g->next) {
		/* sched.c:2215 (tick_ctx_init): a bound group whose VM took an unhandled MMU
		 * fault gets every queue flagged faulty unless a CS fatal already said which.
		 * pw_mmu.c does not notify us; this runs on the next work (job timeout at the
		 * latest). */
		if (g->csg_id >= 0 && group_can_run(g) && pw_uvm_is_unusable(g->uvm)) {
			uint32_t all = PW_GENMASK(g->queue_count - 1, 0), zero = 0;

			pw_warn(dev, "sched: group VM faulted, failing the group");
			__atomic_compare_exchange_n(&g->fatal_queues, &zero, all, false,
						    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
		}
		if (__atomic_load_n(&g->tiler_oom, __ATOMIC_ACQUIRE) && group_can_run(g))
			group_process_tiler_oom_locked(s, g);
		if (!group_can_run(g)) {
			group_terminate_locked(s, g);
			continue;
		}
		group_run_locked(s, g);

		/* sched.c:3463 queue_timeout_work(): check for progress first. */
		now = pw_os_time_ns(dev->os);
		for (uint32_t i = 0; i < g->queue_count; i++) {
			struct pw_queue *q = g->queues[i];
			uint64_t deadline;

			queue_check_completion(q);
			pw_os_mutex_lock(q->lock);
			deadline = q->inflight ? q->deadline_ns : 0;
			pw_os_mutex_unlock(q->lock);
			if (!deadline)
				continue;
			if (now >= deadline) {
				/* sched.c:3390 queue_timedout_job() */
				pw_warn(dev, "job timeout: queue %u, seqno %llu", i,
					(unsigned long long)(q->inflight ? q->inflight->seqno : 0));
				__atomic_store_n(&g->timedout, 1, __ATOMIC_RELEASE);
			} else if (deadline < next) {
				next = deadline;
			}
		}
		if (!group_can_run(g))
			group_terminate_locked(s, g);
	}
	pw_os_mutex_unlock(s->lock);

	if (next != UINT64_MAX) {
		now = pw_os_time_ns(dev->os);
		pw_os_work_queue_delayed(s->work, next > now ?
					 (uint32_t)PW_DIV_ROUND_UP(next - now, 1000000u) : 0);
	}
}

/* ------------------------------------------------------------------------------------------ */
/* device level                                                                               */
/* ------------------------------------------------------------------------------------------ */

/* sched.c:4087 panthor_sched_init() */
int pw_sched_init(struct pw_device *dev)
{
	struct pw_sched *s;

	if (!pw_device_fw_booted(dev) || dev_sched(dev))
		return PW_OK;
	/* sched.c:4116: at least one AS besides the MCU's. */
	if (!(dev->gpu_info.as_present & PW_GENMASK(31, 1))) {
		pw_err(dev, "Not enough AS (expected at least 2)");
		return PW_EINVAL;
	}
	s = pw_os_zalloc(dev->os, sizeof(*s));
	if (!s)
		return PW_ENOMEM;
	s->dev = dev;
	s->lock = pw_os_mutex_create(dev->os);
	s->events_lock = pw_os_mutex_create(dev->os);
	s->work = pw_os_work_create(dev->os, sched_work_fn, s);
	if (!s->lock || !s->events_lock || !s->work) {
		pw_os_work_destroy(dev->os, s->work);
		pw_os_mutex_destroy(dev->os, s->events_lock);
		pw_os_mutex_destroy(dev->os, s->lock);
		pw_os_free(dev->os, s);
		return PW_ENOMEM;
	}
	/* pw_fw already clamped the CSG count to MAX_CSGS and MAX_CSG_PRIO + 1 (sched.c:4099). */
	s->csg_slot_count = dev->csif_info.csg_slot_count;
	s->cs_slot_count = dev->csif_info.cs_slot_count;
	s->sb_slot_count = dev->csif_info.scoreboard_slot_count;
	s->job_timeout_ms = PW_JOB_TIMEOUT_MS;
	__atomic_store_n(&dev->sched, s, __ATOMIC_RELEASE);
	pw_info(dev, "scheduler: %u CSG slots, %u CS per CSG, %u scoreboards", s->csg_slot_count,
		s->cs_slot_count, s->sb_slot_count);
	return PW_OK;
}

void pw_sched_fini(struct pw_device *dev)
{
	struct pw_sched *s = dev_sched(dev);

	if (!s)
		return;
	__atomic_store_n(&dev->sched, NULL, __ATOMIC_RELEASE);
	/* A job bottom half that already loaded the pointer finishes under bh_lock. */
	if (dev->irq[PW_IRQ_JOB].bh_lock) {
		pw_os_mutex_lock(dev->irq[PW_IRQ_JOB].bh_lock);
		pw_os_mutex_unlock(dev->irq[PW_IRQ_JOB].bh_lock);
	}
	if (s->groups || __atomic_load_n(&s->live_groups, __ATOMIC_ACQUIRE))
		pw_err(dev, "sched: %u groups still alive at teardown", s->live_groups);
	pw_os_work_destroy(dev->os, s->work);
	pw_os_mutex_destroy(dev->os, s->events_lock);
	pw_os_mutex_destroy(dev->os, s->lock);
	pw_os_free(dev->os, s);
}

void pw_sched_set_job_timeout(struct pw_device *dev, uint32_t timeout_ms)
{
	struct pw_sched *s = dev_sched(dev);

	if (s)
		__atomic_store_n(&s->job_timeout_ms, timeout_ms ? timeout_ms : 1, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------------------------------ */
/* per file                                                                                   */
/* ------------------------------------------------------------------------------------------ */

int pw_sched_file_init(struct pw_file *file)
{
	struct pw_sched_file *sf;
	int ret;

	sf = pw_os_zalloc(file->os, sizeof(*sf));
	if (!sf)
		return PW_ENOMEM;
	sf->lock = pw_os_mutex_create(file->os);
	if (!sf->lock) {
		pw_os_free(file->os, sf);
		return PW_ENOMEM;
	}
	ret = pw_heap_file_init(file, &sf->heaps);
	if (ret) {
		pw_os_mutex_destroy(file->os, sf->lock);
		pw_os_free(file->os, sf);
		return ret;
	}
	file->sched = sf;
	return PW_OK;
}

struct pw_heap_file *pw_sched_file_heaps(struct pw_file *file)
{
	return file->sched ? file->sched->heaps : NULL;
}

/* sched.c:3768 panthor_group_destroy(), after the handle was removed. Drops the caller's
 * (handle) reference. */
static void group_destroy(struct pw_sched *s, struct pw_group *g)
{
	pw_os_mutex_lock(s->lock);
	__atomic_store_n(&g->destroyed, true, __ATOMIC_RELEASE);
	group_terminate_locked(s, g);
	for (struct pw_group **link = &s->groups; *link; link = &(*link)->next) {
		if (*link == g) {
			*link = g->next;
			g->next = NULL;
			group_put(g);	/* the list's reference */
			break;
		}
	}
	pw_os_mutex_unlock(s->lock);
	group_put(g);
}

void pw_sched_file_fini(struct pw_file *file)
{
	struct pw_sched_file *sf = file->sched;
	struct pw_sched *s = dev_sched(file->dev);

	if (!sf)
		return;
	/* Every group of the file dies: flag them all first, so a job of one group that waits
	 * for another's fence is cancelled instead of being started by the work when that fence
	 * is signalled during the teardown. */
	if (s) {
		pw_os_mutex_lock(s->lock);
		pw_os_mutex_lock(sf->lock);
		for (uint32_t i = 0; i < PW_MAX_GROUPS_PER_POOL; i++) {
			if (sf->handles[i])
				__atomic_store_n(&sf->handles[i]->destroyed, true,
						 __ATOMIC_RELEASE);
		}
		pw_os_mutex_unlock(sf->lock);
		pw_os_mutex_unlock(s->lock);
	}
	/* sched.c:3866 panthor_group_pool_destroy() */
	for (uint32_t i = 0; i < PW_MAX_GROUPS_PER_POOL; i++) {
		struct pw_group *g;

		pw_os_mutex_lock(sf->lock);
		g = sf->handles[i];
		sf->handles[i] = NULL;
		pw_os_mutex_unlock(sf->lock);
		if (g)
			group_destroy(g->sched, g);
	}
	/* No flush of the device-wide work here: it re-arms itself while any queue of any file
	 * has a job in flight, so a flush would make this close wait for other files' jobs
	 * (unbounded under continuous load). Nothing needs it: the work and the job IRQ only
	 * reach groups through sched->groups and the CSG slots, under sched->lock/events_lock,
	 * and group_destroy() removed and unbound every group of this file under those locks;
	 * their in-flight and pending jobs were finished by group_terminate_locked(), so the
	 * groups were released synchronously and nothing refers to sf->heaps any more. */
	pw_heap_file_fini(sf->heaps);
	pw_os_mutex_destroy(file->os, sf->lock);
	pw_os_free(file->os, sf);
	file->sched = NULL;
}

static struct pw_group *group_from_handle(struct pw_file *file, uint32_t handle)
{
	struct pw_sched_file *sf = file->sched;
	struct pw_group *g = NULL;

	if (!sf || !handle || handle > PW_MAX_GROUPS_PER_POOL)
		return NULL;
	pw_os_mutex_lock(sf->lock);
	g = group_get(sf->handles[handle - 1]);
	pw_os_mutex_unlock(sf->lock);
	return g;
}

/* ------------------------------------------------------------------------------------------ */
/* GROUP_CREATE / DESTROY / GET_STATE                                                         */
/* ------------------------------------------------------------------------------------------ */

#define PW_OFFSETOFEND(type, member) (offsetof(type, member) + sizeof(((type *)0)->member))

/* Kernel BO with a write-combined kernel mapping, zeroed. */
static int kbo_create(struct pw_device *dev, uint64_t size, struct pw_bo **bo, void **kva)
{
	int ret = pw_bo_create_kernel(dev, size, PW_MEM_KMAP | PW_MEM_UNCACHED | PW_MEM_ZEROED, bo);

	if (ret)
		return ret;
	*kva = pw_os_mem_kva(pw_bo_os_mem(*bo));
	if (!*kva) {
		pw_bo_put(*bo);
		*bo = NULL;
		return PW_ENOMEM;
	}
	pw_memset(*kva, 0, (size_t)(*bo)->size);
	pw_os_mem_flush(dev->os, pw_bo_os_mem(*bo), 0, (size_t)(*bo)->size);
	return PW_OK;
}

/* sched.c:3474 group_create_queue() */
static int group_create_queue(struct pw_group *g, const struct drm_panthor_queue_create *args,
			      uint32_t idx, struct pw_queue **out)
{
	struct pw_device *dev = g->dev;
	struct pw_queue *q;
	void *kva;
	int ret;

	*out = NULL;
	if (args->pad[0] || args->pad[1] || args->pad[2])
		return PW_EINVAL;
	if (args->ringbuf_size < PW_SZ_4K || args->ringbuf_size > 0x10000u ||
	    (args->ringbuf_size & (args->ringbuf_size - 1)))
		return PW_EINVAL;
	if (args->priority > PW_MAX_QUEUE_PRIO)
		return PW_EINVAL;

	q = pw_os_zalloc(dev->os, sizeof(*q));
	if (!q)
		return PW_ENOMEM;
	q->group = g;
	q->idx = idx;
	q->priority = args->priority;
	q->ringbuf_size = args->ringbuf_size;
	q->lock = pw_os_mutex_create(dev->os);
	if (!q->lock) {
		ret = PW_ENOMEM;
		goto err;
	}

	/* Ring buffer: NO_MMAP kernel BO in the group's VM, NOEXEC | UNCACHED (sched.c:3517). */
	ret = kbo_create(dev, args->ringbuf_size, &q->ringbuf, &kva);
	if (ret)
		goto err;
	q->ring_kva = kva;
	ret = pw_uvm_map_kernel_bo(g->uvm, q->ringbuf, PW_VM_MAP_NOEXEC | PW_VM_MAP_UNCACHED,
				   &q->ringbuf_va);
	if (ret)
		goto err;

	/* Ring-buffer interfaces: 8K in the MCU VM, input at 0, output at 4K (fw.c:489). */
	ret = kbo_create(dev, PW_QUEUE_IFACE_SIZE, &q->iface_bo, &kva);
	if (ret)
		goto err;
	q->input = kva;
	q->output = (const void *)((uint8_t *)kva + PW_SZ_4K);
	ret = pw_fw_map_kernel_mem(dev, pw_bo_os_mem(q->iface_bo), PW_QUEUE_IFACE_SIZE,
				   PW_VM_MAP_NOEXEC | PW_VM_MAP_UNCACHED, &q->iface_mcu_va);
	if (ret)
		goto err;
	*out = q;
	return PW_OK;

err:
	queue_free(g, q);
	return ret;
}

/* drv.c:1224 panthor_ioctl_group_create() + sched.c:3626 panthor_group_create() */
int pw_ioctl_group_create(struct pw_file *file, void *arg)
{
	struct drm_panthor_group_create *args = arg;
	struct pw_device *dev = file->dev;
	struct pw_sched *s = dev_sched(dev);
	struct pw_sched_file *sf = file->sched;
	struct drm_panthor_queue_create *qargs = NULL;
	uint64_t shader_present = dev->gpu_info.shader_present;
	uint64_t tiler_present = dev->gpu_info.tiler_present;
	const struct pw_fw_csg_control_iface *csg0;
	struct pw_group *g = NULL;
	uint32_t size, handle;
	void *kva;
	int ret;

	if (!s || !sf)
		return PW_EOPNOTSUPP;
	/* drv.c:1231 */
	if (!args->queues.count || args->queues.count > MAX_CS_PER_CSG)
		return PW_EINVAL;
	ret = pw_copy_obj_array(file, &args->queues, sizeof(*qargs),
				PW_OFFSETOFEND(struct drm_panthor_queue_create, ringbuf_size),
				(void **)&qargs);
	if (ret)
		return ret;

	/* drv.c group_priority_permit(): HIGH and REALTIME need CAP_SYS_NICE/DRM master, which
	 * nobody has here (DEV_QUERY GROUP_PRIORITIES_INFO reports LOW|MEDIUM). */
	if (args->priority > PANTHOR_GROUP_PRIORITY_REALTIME) {
		ret = PW_EINVAL;
		goto out;
	}
	if (args->priority > PANTHOR_GROUP_PRIORITY_MEDIUM) {
		ret = PW_EACCES;
		goto out;
	}

	/* sched.c:3638-3657 */
	if (args->pad || args->priority >= PW_CSG_PRIORITY_COUNT ||
	    (args->compute_core_mask & ~shader_present) ||
	    (args->fragment_core_mask & ~shader_present) ||
	    (args->tiler_core_mask & ~tiler_present) ||
	    pw_hweight64(args->compute_core_mask) < args->max_compute_cores ||
	    pw_hweight64(args->fragment_core_mask) < args->max_fragment_cores ||
	    pw_hweight64(args->tiler_core_mask) < args->max_tiler_cores ||
	    args->queues.count > s->cs_slot_count) {
		ret = PW_EINVAL;
		goto out;
	}

	g = pw_os_zalloc(dev->os, sizeof(*g));
	if (!g) {
		ret = PW_ENOMEM;
		goto out;
	}
	g->refcount = 1;
	g->dev = dev;
	g->sched = s;
	g->heaps = sf->heaps;
	g->csg_id = -1;
	g->state = PW_GROUP_CREATED;
	g->max_compute_cores = args->max_compute_cores;
	g->compute_core_mask = args->compute_core_mask;
	g->max_fragment_cores = args->max_fragment_cores;
	g->fragment_core_mask = args->fragment_core_mask;
	g->max_tiler_cores = args->max_tiler_cores;
	g->tiler_core_mask = args->tiler_core_mask;
	g->priority = args->priority;
	__atomic_add_fetch(&s->live_groups, 1, __ATOMIC_RELAXED);

	g->uvm = pw_file_vm_get(file, args->vm_id);
	if (!g->uvm) {
		ret = PW_EINVAL;
		goto err_put;
	}

	/* Suspend buffers: MCU VM, NOEXEC (fw.c:530 panthor_fw_alloc_suspend_buf_mem()). The
	 * CPU never touches them. */
	csg0 = dev->fw.csg[0].control;
	size = PW_READ_ONCE(csg0->suspend_size);
	if (size) {
		ret = pw_bo_create_kernel(dev, size, PW_MEM_UNCACHED | PW_MEM_ZEROED,
					  &g->suspend_buf);
		if (!ret)
			ret = pw_fw_map_kernel_mem(dev, pw_bo_os_mem(g->suspend_buf),
						   g->suspend_buf->size, PW_VM_MAP_NOEXEC,
						   &g->suspend_mcu_va);
		if (ret)
			goto err_put;
	}
	size = PW_READ_ONCE(csg0->protm_suspend_size);
	if (size) {
		ret = pw_bo_create_kernel(dev, size, PW_MEM_UNCACHED | PW_MEM_ZEROED,
					  &g->protm_suspend_buf);
		if (!ret)
			ret = pw_fw_map_kernel_mem(dev, pw_bo_os_mem(g->protm_suspend_buf),
						   g->protm_suspend_buf->size, PW_VM_MAP_NOEXEC,
						   &g->protm_suspend_mcu_va);
		if (ret)
			goto err_put;
	}

	/* Per-queue panthor_syncobj_64b, group VM, NOEXEC | UNCACHED (sched.c:3700). */
	ret = kbo_create(dev, args->queues.count * sizeof(struct pw_syncobj_64b), &g->syncobjs,
			 &kva);
	if (ret)
		goto err_put;
	g->syncobjs_kva = kva;
	ret = pw_uvm_map_kernel_bo(g->uvm, g->syncobjs, PW_VM_MAP_NOEXEC | PW_VM_MAP_UNCACHED,
				   &g->syncobjs_va);
	if (ret)
		goto err_put;

	for (uint32_t i = 0; i < args->queues.count; i++) {
		ret = group_create_queue(g, &qargs[i], i, &g->queues[i]);
		if (ret)
			goto err_put;
		g->queue_count++;
	}

	/* Publish: the handle (xa_alloc limit 1..128, -EBUSY when full) and the device list.
	 * Lock order: sched->lock, then the file's handle lock. */
	pw_os_mutex_lock(s->lock);
	pw_os_mutex_lock(sf->lock);
	for (handle = 1; handle <= PW_MAX_GROUPS_PER_POOL && sf->handles[handle - 1]; handle++)
		;
	if (handle > PW_MAX_GROUPS_PER_POOL) {
		pw_os_mutex_unlock(sf->lock);
		pw_os_mutex_unlock(s->lock);
		ret = PW_EBUSY;
		goto err_put;
	}
	g->next = s->groups;
	s->groups = group_get(g);
	sf->handles[handle - 1] = g;	/* takes the creation reference */
	pw_os_mutex_unlock(sf->lock);
	pw_os_mutex_unlock(s->lock);

	args->group_handle = handle;
	ret = PW_OK;
	goto out;

err_put:
	group_put(g);
out:
	pw_os_free(file->os, qargs);
	return ret;
}

/* drv.c:1212 + sched.c:3768 */
int pw_ioctl_group_destroy(struct pw_file *file, void *arg)
{
	struct drm_panthor_group_destroy *args = arg;
	struct pw_sched_file *sf = file->sched;
	struct pw_group *g = NULL;

	if (args->pad)
		return PW_EINVAL;
	if (!sf)
		return PW_EINVAL;
	if (args->group_handle && args->group_handle <= PW_MAX_GROUPS_PER_POOL) {
		pw_os_mutex_lock(sf->lock);
		g = sf->handles[args->group_handle - 1];
		sf->handles[args->group_handle - 1] = NULL;
		pw_os_mutex_unlock(sf->lock);
	}
	if (!g)
		return PW_EINVAL;
	group_destroy(g->sched, g);
	return PW_OK;
}

/* sched.c:3820 panthor_group_get_state() */
int pw_ioctl_group_get_state(struct pw_file *file, void *arg)
{
	struct drm_panthor_group_get_state *args = arg;
	struct pw_group *g;

	if (args->pad)
		return PW_EINVAL;
	g = group_from_handle(file, args->group_handle);
	if (!g)
		return PW_EINVAL;
	args->state = 0;
	args->fatal_queues = 0;
	pw_os_mutex_lock(g->sched->lock);
	if (__atomic_load_n(&g->timedout, __ATOMIC_ACQUIRE))
		args->state |= DRM_PANTHOR_GROUP_STATE_TIMEDOUT;
	args->fatal_queues = __atomic_load_n(&g->fatal_queues, __ATOMIC_ACQUIRE);
	if (args->fatal_queues)
		args->state |= DRM_PANTHOR_GROUP_STATE_FATAL_FAULT;
	if (g->innocent)
		args->state |= DRM_PANTHOR_GROUP_STATE_INNOCENT;
	pw_os_mutex_unlock(g->sched->lock);
	group_put(g);
	return PW_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* GROUP_SUBMIT                                                                               */
/* ------------------------------------------------------------------------------------------ */

/* drv.c:240 struct panthor_sync_signal */
struct submit_signal {
	struct submit_signal *next;
	uint32_t handle;
	uint64_t point;
	struct pw_syncobj *so;
	struct pw_syncobj_point *pt;	/* timeline points only */
	struct pw_fence *fence;		/* fence to publish, may be NULL until a job signals it */
};

struct submit_job {
	struct pw_job *job;
	struct drm_panthor_sync_op *ops;
	uint32_t nops;
};

struct submit_ctx {
	struct pw_file *file;
	struct submit_signal *signals;
	struct submit_job *jobs;
	uint32_t njobs;
};

/* drv.c:330 panthor_check_sync_op() */
static int check_sync_op(const struct drm_panthor_sync_op *op)
{
	uint32_t type;

	if (op->flags & ~(DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_MASK | DRM_PANTHOR_SYNC_OP_SIGNAL))
		return PW_EINVAL;
	type = op->flags & DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_MASK;
	if (type != DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ &&
	    type != DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ)
		return PW_EINVAL;
	if (type == DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ && op->timeline_value)
		return PW_EINVAL;
	return PW_OK;
}

static struct submit_signal *ctx_find_signal(struct submit_ctx *ctx, uint32_t handle,
					     uint64_t point)
{
	for (struct submit_signal *sig = ctx->signals; sig; sig = sig->next) {
		if (sig->handle == handle && sig->point == point)
			return sig;
	}
	return NULL;
}

/* drv.c:382 panthor_submit_ctx_add_sync_signal() */
static int ctx_add_signal(struct submit_ctx *ctx, uint32_t handle, uint64_t point)
{
	struct pw_file *file = ctx->file;
	struct submit_signal *sig;
	struct pw_fence *cur = NULL;

	if (ctx_find_signal(ctx, handle, point))
		return PW_OK;
	sig = pw_os_zalloc(file->os, sizeof(*sig));
	if (!sig)
		return PW_ENOMEM;
	sig->handle = handle;
	sig->point = point;
	sig->next = ctx->signals;
	ctx->signals = sig;
	if (point) {
		sig->pt = pw_syncobj_point_alloc(file->dev);
		if (!sig->pt)
			return PW_ENOMEM;
	}
	sig->so = pw_syncobj_lookup(file, handle);
	if (!sig->so)
		return PW_EINVAL;
	/* A missing fence is fine: nothing attached to that point yet. */
	if (!pw_syncobj_find_fence(file, handle, point, 0, 0, &cur))
		sig->fence = cur;
	return PW_OK;
}

/* drv.c:1066 panthor_job_create(). *@gp caches the group: panthor looks the handle up for every
 * job, but a GROUP_DESTROY + GROUP_CREATE racing with this submit could then hand two different
 * groups to one batch, and only the first one's queues would be run below (reviewer fix). */
static int job_create(struct pw_file *file, struct pw_sched *s, uint32_t group_handle,
		      const struct drm_panthor_queue_submit *qs, struct pw_group **gp,
		      struct pw_job **out)
{
	struct pw_job *job;
	struct pw_group *g;

	*out = NULL;
	if (qs->pad)
		return PW_EINVAL;
	/* If stream_addr is zero, so stream_size should be. */
	if ((qs->stream_size == 0) != (qs->stream_addr == 0))
		return PW_EINVAL;
	/* 64-byte aligned stream address, 8-byte aligned (instruction) size. */
	if ((qs->stream_addr & 63) || (qs->stream_size & 7))
		return PW_EINVAL;
	/* bits 24:30 must be zero. */
	if (qs->latest_flush & PW_GENMASK(30, 24))
		return PW_EINVAL;

	if (!*gp)
		*gp = group_from_handle(file, group_handle);
	g = group_get(*gp);
	if (!g)
		return PW_EINVAL;
	if (!group_can_run(g) || qs->queue_index >= g->queue_count) {
		group_put(g);
		return PW_EINVAL;
	}
	job = pw_os_zalloc(file->os, sizeof(*job));
	if (!job) {
		group_put(g);
		return PW_ENOMEM;
	}
	job->sched = s;
	job->group = g;
	job->queue_idx = qs->queue_index;
	job->stream_addr = qs->stream_addr;
	job->stream_size = qs->stream_size;
	job->latest_flush = qs->latest_flush;
	*out = job;
	return PW_OK;
}

/* drv.c:597 panthor_submit_ctx_add_sync_deps_to_job() + arm + drv.c:503 update signals. */
static int ctx_deps_and_arm(struct submit_ctx *ctx, struct submit_job *sj)
{
	struct pw_file *file = ctx->file;
	struct pw_job *job = sj->job;
	int ret;

	if (sj->nops) {
		job->deps = pw_os_zalloc(file->os, sj->nops * sizeof(*job->deps));
		if (!job->deps)
			return PW_ENOMEM;
	}
	for (uint32_t i = 0; i < sj->nops; i++) {
		const struct drm_panthor_sync_op *op = &sj->ops[i];
		struct submit_signal *sig;
		struct pw_fence *f;

		if (op->flags & DRM_PANTHOR_SYNC_OP_SIGNAL)
			continue;
		ret = check_sync_op(op);
		if (ret)
			return ret;
		sig = ctx_find_signal(ctx, op->handle, op->timeline_value);
		if (sig) {
			if (!sig->fence)
				return PW_EINVAL;
			f = pw_fence_get(sig->fence);
		} else {
			ret = pw_syncobj_find_fence(file, op->handle, op->timeline_value, 0, 0, &f);
			if (ret)
				return ret;
		}
		job->deps[job->ndeps++] = f;
	}

	/* drm_sched_job_arm(): the job's fence now exists. */
	job->done = pw_fence_create(file->dev, 0);
	if (!job->done)
		return PW_ENOMEM;

	for (uint32_t i = 0; i < sj->nops; i++) {
		const struct drm_panthor_sync_op *op = &sj->ops[i];
		struct submit_signal *sig;

		if (!(op->flags & DRM_PANTHOR_SYNC_OP_SIGNAL))
			continue;
		sig = ctx_find_signal(ctx, op->handle, op->timeline_value);
		if (!sig)
			return PW_EINVAL;
		pw_fence_put(sig->fence);
		sig->fence = pw_fence_get(job->done);
	}
	return PW_OK;
}

static void ctx_cleanup(struct submit_ctx *ctx)
{
	struct pw_os *os = ctx->file->os;
	struct submit_signal *sig;

	while ((sig = ctx->signals)) {
		ctx->signals = sig->next;
		pw_syncobj_put(sig->so);
		pw_syncobj_point_free(sig->pt);
		pw_fence_put(sig->fence);
		pw_os_free(os, sig);
	}
	for (uint32_t i = 0; ctx->jobs && i < ctx->njobs; i++) {
		if (ctx->jobs[i].job)
			job_free(ctx->jobs[i].job);	/* never published */
		pw_os_free(os, ctx->jobs[i].ops);
	}
	pw_os_free(os, ctx->jobs);
}

/* drv.c:1106 panthor_ioctl_group_submit() */
int pw_ioctl_group_submit(struct pw_file *file, void *arg)
{
	struct drm_panthor_group_submit *args = arg;
	struct pw_sched *s = dev_sched(file->dev);
	struct drm_panthor_queue_submit *qs = NULL;
	struct submit_ctx ctx = { .file = file };
	struct pw_group *g = NULL;
	int ret;

	if (args->pad)
		return PW_EINVAL;
	if (!s || !file->sched)
		return PW_EOPNOTSUPP;
	ret = pw_copy_obj_array(file, &args->queue_submits, sizeof(*qs),
				PW_OFFSETOFEND(struct drm_panthor_queue_submit, syncs),
				(void **)&qs);
	if (ret)
		return ret;
	if (!args->queue_submits.count)
		goto out;

	ctx.njobs = args->queue_submits.count;
	ctx.jobs = pw_os_zalloc(file->os, ctx.njobs * sizeof(*ctx.jobs));
	if (!ctx.jobs) {
		ret = PW_ENOMEM;
		goto out;
	}

	/* Create jobs and attach sync operations. */
	for (uint32_t i = 0; i < ctx.njobs; i++) {
		ret = job_create(file, s, args->group_handle, &qs[i], &g, &ctx.jobs[i].job);
		if (ret)
			goto out;
		ret = pw_copy_obj_array(file, &qs[i].syncs, sizeof(struct drm_panthor_sync_op),
					PW_OFFSETOFEND(struct drm_panthor_sync_op, timeline_value),
					(void **)&ctx.jobs[i].ops);
		if (ret)
			goto out;
		ctx.jobs[i].nops = qs[i].syncs.count;
	}

	/* Collect the signal operations of every job (drv.c:543). */
	for (uint32_t i = 0; i < ctx.njobs; i++) {
		for (uint32_t k = 0; k < ctx.jobs[i].nops; k++) {
			const struct drm_panthor_sync_op *op = &ctx.jobs[i].ops[k];

			if (!(op->flags & DRM_PANTHOR_SYNC_OP_SIGNAL))
				continue;
			ret = check_sync_op(op);
			if (!ret)
				ret = ctx_add_signal(&ctx, op->handle, op->timeline_value);
			if (ret)
				goto out;
		}
	}

	/* Dependencies, job fences, signal fences, job by job so later jobs of the batch can
	 * depend on earlier ones (drv.c:664). */
	for (uint32_t i = 0; i < ctx.njobs; i++) {
		ret = ctx_deps_and_arm(&ctx, &ctx.jobs[i]);
		if (ret)
			goto out;
	}

	/* Nothing can fail after this point (drv.c:1194): queue the jobs, publish the fences. */
	for (uint32_t i = 0; i < ctx.njobs; i++) {
		struct pw_job *job = ctx.jobs[i].job;
		struct pw_queue *q = job->group->queues[job->queue_idx];

		ctx.jobs[i].job = NULL;
		pw_os_mutex_lock(q->lock);
		list_append(&q->pending, &q->pending_tail, job);
		pw_os_mutex_unlock(q->lock);
	}
	for (struct submit_signal *sig = ctx.signals; sig; sig = sig->next) {
		if (sig->pt) {
			pw_syncobj_add_point(sig->so, sig->pt, sig->fence, sig->point);
			sig->pt = NULL;
		} else {
			pw_syncobj_replace_fence(sig->so, sig->fence);
		}
	}

	/* Run what is ready now; the rest goes through fence callbacks and the work. */
	pw_os_mutex_lock(s->lock);
	if (group_can_run(g)) {
		group_run_locked(s, g);
	} else {
		/* The group died between job_create() and here: what queue_run_job() does for
		 * such a group (sched.c:3317). */
		for (uint32_t i = 0; i < g->queue_count; i++)
			queue_cancel_pending(g->queues[i], PW_ECANCELED);
	}
	pw_os_mutex_unlock(s->lock);
	ret = PW_OK;

out:
	group_put(g);
	ctx_cleanup(&ctx);
	pw_os_free(file->os, qs);
	return ret;
}
