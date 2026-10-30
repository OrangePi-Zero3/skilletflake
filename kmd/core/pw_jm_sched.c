// SPDX-License-Identifier: MIT
/*
 * pw_jm_sched.c - job-manager scheduling (pw_jm_sched.h): SUBMIT, JM contexts, per-slot
 * queues, dependencies, completion, timeouts and the reset policy. Owner: jm-sched.
 * Design: docs/status/jm-arch.md (tasks C1-C8).
 *
 * Behaviour reference: Linux drivers/gpu/drm/panfrost v7.3 (panfrost_drv.c:
 * panfrost_ioctl_submit, panfrost_job.c) and the drm_sched behaviour it relies on.
 * GPL-2.0-only: read for behaviour and errnos only, no code or comments were copied.
 * Citations are file:function. drm_sched is replaced by the small model below.
 *
 * Objects:
 *   struct pw_jm_ctx     a JM context (drm_panfrost_jm_ctx_create): priority, one entity per
 *                        slot, refcounted (file table + every job).
 *   struct jm_entity     the FIFO of one context's jobs for one slot (drm_sched_entity). Only
 *                        its head job is looked at; jobs of one entity reach the GPU in order.
 *                        Linked on its slot's run queue for its priority while non-empty.
 *   struct pw_jm_job     one SUBMIT: one job chain for one slot, its dependencies, BO / VM /
 *                        context references and its done fence.
 *   struct jm_slot       per job slot: the hardware queue (current + NEXT, pfdev->jobs[js]),
 *                        a requeue list (jobs to put back on the GPU first, after a reset or a
 *                        soft stop) and the run queues (drm_sched run queues, one per priority).
 *
 * Job states: QUEUED (in its entity) -> [REQUEUED] <-> HW -> DONE. A QUEUED job becomes
 * eligible when its dependencies signalled (callbacks queue the work item, the ioctl never
 * waits) and is then submitted when its slot has room and an AS could be bound. DONE jobs have
 * their fence signalled outside every scheduler lock, then their references are dropped in the
 * work item (the last BO/VM reference can unmap memory, which must not happen in the job IRQ
 * bottom half).
 *
 * Locking (outermost first):
 *   sf->lock        a file's context table; never held with another lock of this module.
 *   pw_jm_resv_lock SUBMIT's "collect BO fences + queue + publish" section (pw_jm_mem.h).
 *   job bh_lock     dev->irq[PW_IRQ_JOB].bh_lock (pw_irq.c holds it around the job bottom
 *                   half). Every other path that can program a slot takes it before s->lock
 *                   (jm_lock): the bottom half cancels NEXT, clears JOB_INT and samples
 *                   JS_STATE before it calls pw_jm_sched_job_irq(), and a START issued in
 *                   between would make that event wrong (a job submitted into a slot the
 *                   stale sample calls idle would be taken as done). Linux holds job_lock
 *                   across panfrost_jm_handle_irq and around JS_COMMAND_NEXT = START.
 *   s->lock         all job, entity and slot state. Taken in ioctls, the job IRQ bottom half,
 *                   the work item and the watchdog. Under it: pw_jm_as_get/put (vm op_lock ->
 *                   slots_lock), pw_fence_add_callback (fence lock), the pw_jm_slot_* register
 *                   helpers, pw_os_work_queue. Never held across pw_jm_hw_reset() (which waits
 *                   for the job bottom half, and that one takes s->lock), never across a
 *                   pw_fence_signal() or a BO/VM put.
 *   s->clean_lock   leaf: the list of finished jobs waiting for the work item.
 */

#include "pw_jm_mem.h"
#include "pw_jm_sched.h"
#include "pw_sync.h"

#define JM_NUM_PRIO		3	/* PANFROST_JM_CTX_PRIORITY_LOW/MEDIUM/HIGH */
#define JM_MAX_DEPTH		2	/* current + NEXT (pfdev->jobs[slot][2]) */
#define JM_NUM_CTX		(PW_JM_MAX_CTX_PER_FILE + 1)
#define JM_FILE_FINI_POLL_MS	100
#define JM_NEXT_RETRY_MS	1	/* hw_submit() found NEXT busy on an idle slot */

enum jm_job_state {
	JM_JOB_NEW = 0,		/* SUBMIT is still setting it up */
	JM_JOB_QUEUED,		/* in its entity */
	JM_JOB_PICKED,		/* taken from its entity, being submitted (transient, s->lock) */
	JM_JOB_REQUEUED,	/* in a slot's requeue list: dependencies met, off the GPU */
	JM_JOB_HW,		/* in a slot's hardware queue */
	JM_JOB_DONE,		/* fence about to be / already signalled */
};

enum { JM_CB_NONE = 0, JM_CB_ARMED, JM_CB_FIRED };

struct pw_jm_job;
struct pw_jm_ctx;

struct jm_jlist {
	struct pw_jm_job *head, *tail;
};

struct jm_entity {
	struct pw_jm_ctx *ctx;
	uint32_t slot;
	struct jm_jlist q;
	struct jm_entity *rq_prev, *rq_next;
	bool on_rq;
};

struct jm_rq {
	struct jm_entity *head, *tail;
};

struct pw_jm_ctx {
	uint32_t refcount;		/* __atomic: file table + jobs + temporary lookups */
	struct pw_jm_sched *s;
	uint32_t prio;			/* PANFROST_JM_CTX_PRIORITY_* */
	uint32_t handle;
	uint64_t owner;			/* id of the file that created it (file_busy_locked) */
	/* under s->lock */
	bool destroyed;
	uint32_t hw_jobs;		/* jobs REQUEUED or HW (debugging) */
	struct pw_os_event *idle_ev;	/* signalled when one of them leaves (file close) */
	struct jm_entity ent[PW_JM_NUM_SLOTS];
};

struct pw_jm_job {
	struct pw_jm_sched *s;
	struct pw_jm_job *next;		/* entity / requeue / done / clean list */
	struct pw_jm_ctx *ctx;		/* reference */
	struct pw_jm_vm *jvm;		/* reference */
	struct pw_bo **bos;		/* references */
	uint32_t nbos;
	struct pw_fence **deps;		/* references; in_syncs then BO fences */
	uint32_t ndeps;
	uint32_t next_dep;		/* deps[< next_dep] are signalled */
	uint32_t cb_dep;		/* the callback is on deps[cb_dep] while ARMED */
	struct pw_fence_cb cb;
	uint32_t cb_state;		/* __atomic */
	struct pw_fence *done;		/* reference */
	uint64_t jc;			/* chain head; the resume point after a stop */
	uint32_t reqs;
	uint32_t slot;
	uint32_t flush_id;
	uint32_t timeout_ms;
	bool cc;			/* holds the cycle counter */
	/* under s->lock */
	enum jm_job_state state;
	int as;				/* bound AS while HW, else -1 */
	bool chain_flag;
	bool killed;			/* its context was destroyed while it was on the GPU */
	bool guilty;			/* timed out */
	uint64_t deadline_ns;		/* while the current job of its slot, else 0 */
	int error;
};

struct jm_slot {
	struct pw_jm_job *hw[JM_MAX_DEPTH];	/* [0] current, [1] in NEXT */
	uint32_t nhw;
	struct jm_jlist requeue;
	struct jm_rq rq[JM_NUM_PRIO];
	uint32_t seq;			/* JOB_CHAIN_FLAG parity (panfrost_get_job_chain_flag) */
	bool next_stuck;		/* NEXT read busy on an idle slot (logged once) */
};

struct pw_jm_sched {
	struct pw_device *dev;
	struct pw_os_mutex *bh_lock;	/* the job line's, borrowed (see "Locking") */
	struct pw_os_mutex *lock;
	struct pw_os_mutex *clean_lock;
	struct pw_os_work *work;	/* dependencies, feeding, freeing finished jobs */
	struct pw_os_work *wd_work;	/* timeout watchdog + reset */
	struct jm_jlist clean;		/* under clean_lock */
	uint32_t timeout_ms;		/* __atomic: for jobs submitted from now on */
	uint64_t seqno;			/* __atomic: fence seqnos */
	uint64_t next_owner;		/* __atomic: file ids for pw_jm_ctx.owner */
	uint32_t depth;
	bool fini_done;
	/* under s->lock */
	struct jm_slot slot[PW_JM_NUM_SLOTS];
	bool resetting;			/* stop feeding: a reset is in progress */
	bool stopping;			/* pw_jm_sched_fini() */
	bool dead;			/* a reset failed: the GPU is unusable */
	bool wd_armed;
	uint64_t wd_due_ns;
	struct pw_jm_sched_stats st;	/* jobs_alive is __atomic */
};

struct pw_jm_sched_file {
	struct pw_os_mutex *lock;
	struct pw_jm_ctx *ctx[JM_NUM_CTX];	/* handle -> context (a reference) */
	struct pw_os_event *idle_ev;
	uint64_t owner;			/* this file's id, never reused */
};

#define job_of_cb(c) ((struct pw_jm_job *)((char *)(c) - offsetof(struct pw_jm_job, cb)))

/* ------------------------------------------------------------------------------------------ */
/* small helpers                                                                              */
/* ------------------------------------------------------------------------------------------ */

static void jl_push(struct jm_jlist *l, struct pw_jm_job *j)
{
	j->next = NULL;
	if (l->tail)
		l->tail->next = j;
	else
		l->head = j;
	l->tail = j;
}

static struct pw_jm_job *jl_pop(struct jm_jlist *l)
{
	struct pw_jm_job *j = l->head;

	if (j) {
		l->head = j->next;
		if (!l->head)
			l->tail = NULL;
		j->next = NULL;
	}
	return j;
}

/* @front goes before the jobs already in @l, keeping its own order. */
static void jl_prepend(struct jm_jlist *l, struct jm_jlist *front)
{
	if (!front->head)
		return;
	front->tail->next = l->head;
	if (!l->tail)
		l->tail = front->tail;
	l->head = front->head;
	front->head = front->tail = NULL;
}

static void rq_add(struct jm_rq *rq, struct jm_entity *e)
{
	e->rq_next = NULL;
	e->rq_prev = rq->tail;
	if (rq->tail)
		rq->tail->rq_next = e;
	else
		rq->head = e;
	rq->tail = e;
	e->on_rq = true;
}

static void rq_del(struct jm_rq *rq, struct jm_entity *e)
{
	if (e->rq_prev)
		e->rq_prev->rq_next = e->rq_next;
	else
		rq->head = e->rq_next;
	if (e->rq_next)
		e->rq_next->rq_prev = e->rq_prev;
	else
		rq->tail = e->rq_prev;
	e->rq_prev = e->rq_next = NULL;
	e->on_rq = false;
}

static struct jm_rq *ent_rq(struct pw_jm_sched *s, struct jm_entity *e)
{
	return &s->slot[e->slot].rq[e->ctx->prio];
}

static uint64_t jm_now(struct pw_jm_sched *s)
{
	return pw_os_time_ns(s->dev->os);
}

/* s->lock for paths that may program a slot, outside the job bottom half (which already holds
 * bh_lock and takes s->lock alone). Never held across pw_jm_hw_reset_collect(). */
static void jm_lock(struct pw_jm_sched *s)
{
	if (s->bh_lock)
		pw_os_mutex_lock(s->bh_lock);
	pw_os_mutex_lock(s->lock);
}

static void jm_unlock(struct pw_jm_sched *s)
{
	pw_os_mutex_unlock(s->lock);
	if (s->bh_lock)
		pw_os_mutex_unlock(s->bh_lock);
}

/* ------------------------------------------------------------------------------------------ */
/* contexts                                                                                   */
/* ------------------------------------------------------------------------------------------ */

static struct pw_jm_ctx *ctx_get(struct pw_jm_ctx *ctx)
{
	if (ctx)
		__atomic_add_fetch(&ctx->refcount, 1, __ATOMIC_RELAXED);
	return ctx;
}

/* Every job holds a reference, so the last put finds all entities empty and unlinked. */
static void ctx_put(struct pw_jm_ctx *ctx)
{
	if (!ctx || __atomic_sub_fetch(&ctx->refcount, 1, __ATOMIC_ACQ_REL))
		return;
	pw_os_free(ctx->s->dev->os, ctx);
}

static struct pw_jm_ctx *ctx_alloc(struct pw_jm_sched *s, uint32_t prio, uint64_t owner)
{
	struct pw_jm_ctx *ctx = pw_os_zalloc(s->dev->os, sizeof(*ctx));

	if (!ctx)
		return NULL;
	ctx->refcount = 1;
	ctx->s = s;
	ctx->prio = prio;
	ctx->owner = owner;
	for (uint32_t js = 0; js < PW_JM_NUM_SLOTS; js++) {
		ctx->ent[js].ctx = ctx;
		ctx->ent[js].slot = js;
	}
	return ctx;
}

/* ------------------------------------------------------------------------------------------ */
/* job lifetime                                                                               */
/* ------------------------------------------------------------------------------------------ */

/* Passive level, no scheduler lock: drops BO/VM references (may unmap). */
static void job_free(struct pw_jm_job *job)
{
	struct pw_jm_sched *s = job->s;
	struct pw_device *dev = s->dev;

	for (uint32_t i = 0; i < job->ndeps; i++)
		pw_fence_put(job->deps[i]);
	pw_os_free(dev->os, job->deps);
	pw_jm_bos_put(dev, job->bos, job->nbos);
	pw_jm_vm_put(job->jvm);
	if (job->cc)
		pw_jm_cycle_counter_put(dev);
	pw_fence_put(job->done);
	ctx_put(job->ctx);
	pw_os_free(dev->os, job);
	__atomic_sub_fetch(&s->st.jobs_alive, 1, __ATOMIC_RELAXED);
}

/* Fence callback (signaller's context, no fence lock held): only wakes the work item. The
 * FIRED store is the last access to @job: a canceller may free it as soon as it sees it. */
static void jm_dep_cb(struct pw_fence *f, struct pw_fence_cb *cb)
{
	struct pw_jm_job *job = job_of_cb(cb);
	struct pw_os_work *w = job->s->work;

	(void)f;
	__atomic_store_n(&job->cb_state, JM_CB_FIRED, __ATOMIC_RELEASE);
	pw_os_work_queue(w);
}

/* Make sure the dependency callback is neither registered nor running. */
static void job_disarm(struct pw_jm_job *job)
{
	uint32_t st = __atomic_load_n(&job->cb_state, __ATOMIC_ACQUIRE);

	if (st == JM_CB_ARMED && !pw_fence_remove_callback(job->deps[job->cb_dep], &job->cb)) {
		/* It ran or is running; its window is a few instructions. */
		while (__atomic_load_n(&job->cb_state, __ATOMIC_ACQUIRE) != JM_CB_FIRED)
			pw_os_udelay(job->s->dev->os, 1);
	}
	__atomic_store_n(&job->cb_state, JM_CB_NONE, __ATOMIC_RELAXED);
}

static bool state_counts(enum jm_job_state st)
{
	return st == JM_JOB_REQUEUED || st == JM_JOB_HW;
}

/* s->lock held. Keeps ctx->hw_jobs (the "may still touch the GPU" count) in step. */
static void job_set_state(struct pw_jm_job *job, enum jm_job_state st)
{
	struct pw_jm_ctx *ctx = job->ctx;
	bool was = state_counts(job->state), is = state_counts(st);

	job->state = st;
	if (is && !was) {
		ctx->hw_jobs++;
	} else if (was && !is) {
		ctx->hw_jobs--;
		if (ctx->idle_ev)
			pw_os_event_signal(ctx->idle_ev);	/* file close re-checks */
	}
}

/* Drop the AS user reference of a job that left the GPU (panfrost_job.c:
 * panfrost_jm_handle_done / panfrost_job_handle_err: panfrost_mmu_as_put). s->lock held. */
static void job_as_put(struct pw_jm_sched *s, struct pw_jm_job *job)
{
	if (job->as >= 0) {
		pw_jm_as_put(s->dev, job->jvm);
		job->as = -1;
	}
	job->deadline_ns = 0;
}

/* Finish @job with @err: off every queue, AS released, on @done for jm_finish(). s->lock held. */
static void job_complete(struct pw_jm_sched *s, struct pw_jm_job *job, int err,
			 struct jm_jlist *done)
{
	job_as_put(s, job);
	if (__atomic_load_n(&job->cb_state, __ATOMIC_ACQUIRE) != JM_CB_NONE)
		job_disarm(job);
	job->error = err;
	job_set_state(job, JM_JOB_DONE);
	jl_push(done, job);
}

/* No lock held: signal the fences of @done in order, then hand the jobs to the work item. */
static void jm_finish(struct pw_jm_sched *s, struct jm_jlist *done)
{
	struct pw_jm_job *job;
	uint64_t n = 0;

	if (!done->head)
		return;
	for (job = done->head; job; job = job->next) {
		pw_fence_signal(job->done, job->error);
		n++;
	}
	__atomic_add_fetch(&s->st.completed, n, __ATOMIC_RELAXED);
	pw_os_mutex_lock(s->clean_lock);
	if (s->clean.tail)
		s->clean.tail->next = done->head;
	else
		s->clean.head = done->head;
	s->clean.tail = done->tail;
	pw_os_mutex_unlock(s->clean_lock);
	done->head = done->tail = NULL;
	pw_os_work_queue(s->work);
}

/* ------------------------------------------------------------------------------------------ */
/* dependencies                                                                               */
/* ------------------------------------------------------------------------------------------ */

/* @f is the fence of a job already in @sl's hardware queue: the slot runs it first. */
static bool fence_on_hw(const struct jm_slot *sl, const struct pw_fence *f)
{
	for (uint32_t i = 0; i < sl->nhw; i++) {
		if (sl->hw[i]->done == f)
			return true;
	}
	return false;
}

/* True when every dependency of @job signalled (consumed ones are skipped next time). */
static bool job_deps_signalled(struct pw_jm_job *job)
{
	while (job->next_dep < job->ndeps && pw_fence_is_signalled(job->deps[job->next_dep]))
		job->next_dep++;
	return job->next_dep == job->ndeps;
}

/*
 * True when every dependency of @job is met. Otherwise a callback is armed on the first one
 * that is not (drm_sched_entity_add_dependency_cb). With @sched_ok, a dependency on a job that
 * is in the hardware queue of @job's slot counts as met: drm_sched only waits for the
 * "scheduled" fence of a dependency from the same scheduler (the slot executes in order). Such
 * a dependency is not consumed: a job that ends up not being run (destroyed context) must
 * still wait for its signal. s->lock held.
 */
static bool job_deps_ready(struct pw_jm_sched *s, struct pw_jm_job *job, bool sched_ok)
{
	const struct jm_slot *sl = &s->slot[job->slot];

	if (__atomic_load_n(&job->cb_state, __ATOMIC_ACQUIRE) == JM_CB_ARMED) {
		struct pw_fence *f = job->deps[job->cb_dep];

		if (!pw_fence_is_signalled(f) && !(sched_ok && fence_on_hw(sl, f)))
			return false;
		job_disarm(job);
	} else {
		__atomic_store_n(&job->cb_state, JM_CB_NONE, __ATOMIC_RELAXED);
	}
	job_deps_signalled(job);
	for (uint32_t i = job->next_dep; i < job->ndeps; i++) {
		struct pw_fence *f = job->deps[i];

		if (pw_fence_is_signalled(f) || (sched_ok && fence_on_hw(sl, f)))
			continue;
		job->cb_dep = i;
		__atomic_store_n(&job->cb_state, JM_CB_ARMED, __ATOMIC_RELEASE);
		if (pw_fence_add_callback(f, &job->cb, jm_dep_cb) == PW_ENOENT) {
			__atomic_store_n(&job->cb_state, JM_CB_NONE, __ATOMIC_RELAXED);
			continue;
		}
		return false;
	}
	return true;
}

/* ------------------------------------------------------------------------------------------ */
/* feeding the slots                                                                          */
/* ------------------------------------------------------------------------------------------ */

static struct pw_jm_job *ent_pop(struct pw_jm_sched *s, struct jm_entity *e)
{
	struct pw_jm_job *job = jl_pop(&e->q);

	if (!e->q.head && e->on_rq)
		rq_del(ent_rq(s, e), e);
	return job;
}

/*
 * Entity of a destroyed context: its jobs never reach the GPU; each one fails with -ECANCELED
 * once all its dependencies signalled (panfrost_job.c:panfrost_job_run returns -ECANCELED for a
 * destroyed context when drm_sched gets to it, i.e. after its dependencies), so a BO's fence
 * never signals before the fences it replaced. s->lock held.
 */
static void ent_reap(struct pw_jm_sched *s, struct jm_entity *e, struct jm_jlist *done)
{
	struct pw_jm_job *job;

	while ((job = e->q.head) && job_deps_ready(s, job, false)) {
		ent_pop(s, e);
		job_complete(s, job, PW_ECANCELED, done);
	}
}

static void slot_reap(struct pw_jm_sched *s, uint32_t js, struct jm_jlist *done)
{
	struct jm_slot *sl = &s->slot[js];

	for (uint32_t p = 0; p < JM_NUM_PRIO; p++) {
		struct jm_entity *e, *n;

		for (e = sl->rq[p].head; e; e = n) {
			n = e->rq_next;
			if (e->ctx->destroyed)
				ent_reap(s, e, done);
		}
	}
}

/* The next ready job for @js: highest priority first, round robin between the entities of one
 * priority (drm_sched_rq_select_entity_rr), FIFO within an entity. s->lock held. */
static struct pw_jm_job *slot_pick(struct pw_jm_sched *s, uint32_t js)
{
	struct jm_slot *sl = &s->slot[js];

	for (uint32_t p = JM_NUM_PRIO; p-- > 0;) {
		struct jm_entity *e, *n;

		for (e = sl->rq[p].head; e; e = n) {
			struct pw_jm_job *job = e->q.head;

			n = e->rq_next;
			if (!job || e->ctx->destroyed || !job_deps_ready(s, job, true))
				continue;
			ent_pop(s, e);
			if (e->q.head) {	/* round robin: to the back of its run queue */
				rq_del(&sl->rq[p], e);
				rq_add(&sl->rq[p], e);
			}
			job->state = JM_JOB_PICKED;
			return job;
		}
	}
	return NULL;
}

static void wd_arm(struct pw_jm_sched *s, uint64_t due_ns)
{
	uint64_t now = jm_now(s);
	uint32_t ms;

	if (s->stopping || !s->wd_work)
		return;
	if (s->wd_armed) {
		/* A sooner deadline than the armed one (shorter timeout set for tests): ask for an
		 * early run, which re-arms for the earliest deadline (OS layers that ignore a queue
		 * on a pending delayed item only make detection later). */
		if (due_ns + 1000000u < s->wd_due_ns)
			pw_os_work_queue(s->wd_work);
		return;
	}
	ms = due_ns > now ? (uint32_t)PW_MIN((due_ns - now + 999999u) / 1000000u, 0xfffffffeull)
			  : 0u;
	s->wd_armed = true;
	s->wd_due_ns = due_ns;
	pw_os_work_queue_delayed(s->wd_work, ms + 1);
}

/* The current job of @js has just started: its timeout runs from now (drm_sched restarts the
 * timeout for the next pending job when one completes). s->lock held. */
static void slot_head_started(struct pw_jm_sched *s, uint32_t js)
{
	struct jm_slot *sl = &s->slot[js];
	struct pw_jm_job *job = sl->nhw ? sl->hw[0] : NULL;

	if (!job || job->deadline_ns)
		return;
	job->deadline_ns = jm_now(s) + (uint64_t)job->timeout_ms * 1000000u;
	wd_arm(s, job->deadline_ns);
}

/* Remove the current job of @js from the hardware queue. s->lock held. */
static struct pw_jm_job *hw_pop(struct jm_slot *sl)
{
	struct pw_jm_job *job;

	if (!sl->nhw)
		return NULL;
	job = sl->hw[0];
	for (uint32_t i = 1; i < sl->nhw; i++)
		sl->hw[i - 1] = sl->hw[i];
	sl->hw[--sl->nhw] = NULL;
	return job;
}

/*
 * Put @job on @js (panfrost_job.c:panfrost_job_hw_submit): bind its VM to an AS, pick the
 * JOB_CHAIN_FLAG so the job in NEXT never carries the current one's, then program NEXT.
 * PW_EBUSY when no AS is free (Linux WARNs; the job waits here instead), other errors fail it.
 */
static int hw_submit(struct pw_jm_sched *s, uint32_t js, struct pw_jm_job *job)
{
	struct pw_device *dev = s->dev;
	struct jm_slot *sl = &s->slot[js];
	bool flag;
	int as;

	/* Our bookkeeping says NEXT is empty. Right after the previous START the JM may not have
	 * moved that chain to the current slot yet, or it holds it back behind an error that the
	 * bottom half has not cleared: wait a little, else retry on the next event. */
	for (uint32_t i = 0; !pw_jm_slot_next_free(dev, js); i++) {
		if (i == 20) {
			if (!sl->nhw && !sl->next_stuck) {
				pw_err(dev, "JS%u: NEXT registers busy on an idle slot", js);
				sl->next_stuck = true;
			}
			return PW_EAGAIN;
		}
		pw_os_udelay(dev->os, 1);
	}
	sl->next_stuck = false;
	as = pw_jm_as_get(dev, job->jvm);
	if (as < 0)
		return as;
	job->as = as;
	flag = (++sl->seq & 1u) != 0;
	if (sl->nhw && sl->hw[0]->chain_flag == flag)
		flag = !flag;
	job->chain_flag = flag;
	job->deadline_ns = 0;
	sl->hw[sl->nhw++] = job;
	job_set_state(job, JM_JOB_HW);
	s->st.hw_submits++;
	pw_jm_slot_submit(dev, js, job->jc, (uint32_t)as, job->flush_id, flag);
	slot_head_started(s, js);
	return PW_OK;
}

/* Fill @js's hardware queue: requeued jobs first, then ready entity jobs. s->lock held. */
static void slot_feed(struct pw_jm_sched *s, uint32_t js, struct jm_jlist *done)
{
	struct jm_slot *sl = &s->slot[js];

	if (s->resetting || s->stopping)
		return;
	slot_reap(s, js, done);
	while (sl->nhw < s->depth || s->dead) {
		struct pw_jm_job *job = jl_pop(&sl->requeue);
		int ret;

		if (!job)
			job = slot_pick(s, js);
		if (!job)
			break;
		if (job->ctx->destroyed) {
			/* Only requeued jobs get here. One whose dependency is still on this slot
			 * (scheduled-fence shortcut) is cancelled after it: that job's event
			 * feeds the slot again. */
			if (!job_deps_signalled(job)) {
				job->next = sl->requeue.head;
				sl->requeue.head = job;
				if (!sl->requeue.tail)
					sl->requeue.tail = job;
				break;
			}
			job_complete(s, job, PW_ECANCELED, done);
			continue;
		}
		if (s->dead) {
			job_complete(s, job, PW_EIO, done);
			continue;
		}
		ret = hw_submit(s, js, job);
		if (ret == PW_EBUSY || ret == PW_EAGAIN) {
			/* Every AS has running jobs (EBUSY), or NEXT is not free yet: retried when
			 * a job of the GPU completes. Its dependencies stay met. */
			job_set_state(job, JM_JOB_REQUEUED);
			job->next = sl->requeue.head;
			sl->requeue.head = job;
			if (!sl->requeue.tail)
				sl->requeue.tail = job;
			if (ret == PW_EBUSY)
				s->st.as_busy++;
			/* (review) With nothing of this slot on the GPU no job event will come to
			 * retry it (and no watchdog covers a requeued job): poll instead. */
			if (ret == PW_EAGAIN && !sl->nhw)
				pw_os_work_queue_delayed(s->work, JM_NEXT_RETRY_MS);
			break;
		}
		if (ret) {
			pw_err(s->dev, "JS%u: cannot bind an address space (%d)", js, ret);
			job_complete(s, job, ret, done);
		}
	}
}

/* Both slots: an AS released by one can unblock the other. s->lock held. */
static void feed_all(struct pw_jm_sched *s, struct jm_jlist *done)
{
	for (uint32_t js = 0; js < PW_JM_NUM_SLOTS; js++)
		slot_feed(s, js, done);
}

/* ------------------------------------------------------------------------------------------ */
/* completion (job IRQ bottom half)                                                           */
/* ------------------------------------------------------------------------------------------ */

/* Back to the requeue list (dependencies stay met), AS released. s->lock held. */
static void job_to_back(struct pw_jm_sched *s, struct pw_jm_job *job, struct jm_jlist *back)
{
	job_as_put(s, job);
	job_set_state(job, JM_JOB_REQUEUED);
	jl_push(back, job);
}

/*
 * One slot's part of an event (panfrost_job.c:panfrost_jm_handle_irq + panfrost_job_handle_err +
 * panfrost_jm_handle_done), outside a reset. Done happened before failed (pw_jm_hw.h). A
 * failed slot had its NEXT cancelled by the handler: that job is restarted, or cancelled if its
 * context was destroyed meanwhile. s->lock held.
 */
static void slot_event(struct pw_jm_sched *s, uint32_t js, const struct pw_jm_irq_event *ev,
		       struct jm_jlist *done)
{
	struct pw_device *dev = s->dev;
	struct jm_slot *sl = &s->slot[js];
	struct jm_jlist back = { 0 };
	bool d = (ev->done & PW_BIT(js)) != 0, f = (ev->failed & PW_BIT(js)) != 0;
	struct pw_jm_job *job;

	if (!d && !f)
		return;
	if (d) {
		job = hw_pop(sl);
		if (job)
			job_complete(s, job, 0, done);
		else
			pw_warn(dev, "JS%u: done event without a job", js);
	}
	if (f) {
		uint32_t st = ev->status[js];

		job = hw_pop(sl);
		if (!job) {
			pw_warn(dev, "JS%u: failed event (0x%x) without a job", js, st);
		} else if (st == PW_JM_EXC_STOPPED) {
			/* Soft-stopped (we never issue one outside a reset): resume from the tail,
			 * before the job that waited in NEXT. */
			if (ev->tail[js])
				job->jc = ev->tail[js];
			job_to_back(s, job, &back);
		} else if (st == PW_EXC_TERMINATED) {
			job_complete(s, job, PW_ECANCELED, done);	/* hard-stopped */
		} else if (st > PW_EXC_MAX_NON_FAULT) {
			/* A fault (already logged); the slot stays usable. Linux resets only for
			 * JOB_BUS_FAULT with HW_ISSUE_TTRX_3076 (panfrost_device.c:
			 * panfrost_exception_needs_reset), a G57 erratum: no Bifrost part has it. */
			job_complete(s, job, PW_EINVAL, done);
		} else {
			job_complete(s, job, 0, done);
		}
	} else if (d && sl->nhw && !(ev->js_state & JOB_JS_STATE_SLOT_MASK(js))) {
		/* The JM starts NEXT without an ack: both chains completed under one done bit. A
		 * job whose context was destroyed may have been cancelled in NEXT instead: then it
		 * never ran. */
		job = hw_pop(sl);
		job_complete(s, job, job->killed ? PW_ECANCELED : 0, done);
	}

	if (f && sl->nhw) {
		job = sl->hw[0];
		if (job->killed) {
			hw_pop(sl);
			job_complete(s, job, PW_ECANCELED, done);
		} else if (back.head || s->resetting) {
			/* Behind the stopped job, or a reset is about to start: it is off the GPU
			 * (the handler cancelled NEXT), so it waits in the requeue list. */
			hw_pop(sl);
			job_to_back(s, job, &back);
		} else {
			job->deadline_ns = 0;
			s->st.hw_submits++;
			pw_jm_slot_submit(dev, js, job->jc, (uint32_t)job->as, job->flush_id,
					  job->chain_flag);
		}
	}
	jl_prepend(&sl->requeue, &back);
	slot_head_started(s, js);
}

void pw_jm_sched_job_irq(struct pw_device *dev, const struct pw_jm_irq_event *ev)
{
	struct pw_jm_sched *s = dev->jm_sched;
	struct jm_jlist done = { 0 };

	if (!s || !ev)
		return;
	pw_os_mutex_lock(s->lock);
	for (uint32_t js = 0; js < PW_JM_NUM_SLOTS; js++)
		slot_event(s, js, ev, &done);
	feed_all(s, &done);
	pw_os_mutex_unlock(s->lock);
	jm_finish(s, &done);
}

/* ------------------------------------------------------------------------------------------ */
/* timeouts and reset                                                                         */
/* ------------------------------------------------------------------------------------------ */

/*
 * After pw_jm_hw_reset_collect() (panfrost_job.c:panfrost_reset): apply the events of the
 * stopped slots, then decide for every job left in the hardware queue. Done chains complete
 * (Linux likewise counts an ambiguous done-with-NEXT as two completions, see pw_jm_hw.h). The
 * guilty job, and the later jobs of its context in the same slot, fail with -ECANCELED
 * (drm_sched_increase_karma + drm_sched_resubmit_jobs: karma above hang_limit 0 marks the
 * entity guilty); so do jobs of destroyed contexts; a faulted chain fails with -EINVAL. The
 * innocent ones go back to the front of the requeue list in their order and resume from
 * JS_TAIL if the stop reported one (Linux soft-stops and resumes STOPPED chains from the tail;
 * the reset here hard-stops, see docs/status/jm-sched.md). s->lock held.
 */
static void slot_after_reset(struct pw_jm_sched *s, uint32_t js, const struct pw_jm_irq_event *ev,
			     struct jm_jlist *done)
{
	struct jm_slot *sl = &s->slot[js];
	struct jm_jlist back = { 0 };
	struct pw_jm_ctx *guilty_ctx = NULL;
	bool d = (ev->done & PW_BIT(js)) != 0, f = (ev->failed & PW_BIT(js)) != 0;
	struct pw_jm_job *job;
	bool first = true;

	if (d && (job = hw_pop(sl)))
		job_complete(s, job, 0, done);
	if (d && !f && sl->nhw && !(ev->js_state & JOB_JS_STATE_SLOT_MASK(js)) &&
	    (job = hw_pop(sl)))
		job_complete(s, job, job->killed ? PW_ECANCELED : 0, done);

	while ((job = hw_pop(sl))) {
		uint32_t st = (f && first) ? ev->status[js] : 0;
		uint64_t tail = (f && first) ? ev->tail[js] : 0;
		bool cancel;

		first = false;
		if (job->guilty)
			guilty_ctx = job->ctx;
		cancel = job->killed || job->guilty || job->ctx->destroyed ||
			 job->ctx == guilty_ctx;
		if (st > PW_EXC_MAX_NON_FAULT) {
			job_complete(s, job, PW_EINVAL, done);
		} else if (st && st != PW_JM_EXC_STOPPED && st != PW_EXC_TERMINATED) {
			job_complete(s, job, 0, done);
		} else if (cancel) {
			job_complete(s, job, PW_ECANCELED, done);
		} else {
			if (tail)
				job->jc = tail;
			job_to_back(s, job, &back);
		}
	}
	jl_prepend(&sl->requeue, &back);
}

static void jm_reset(struct pw_jm_sched *s)
{
	struct pw_device *dev = s->dev;
	struct pw_jm_irq_event ev;
	struct jm_jlist done = { 0 };
	int ret;

	pw_os_mutex_lock(s->lock);
	if (s->stopping || s->dead) {
		pw_os_mutex_unlock(s->lock);
		return;
	}
	s->resetting = true;	/* nothing is fed or restarted until the end */
	pw_os_mutex_unlock(s->lock);

	/* Without s->lock: the reset waits for a running job bottom half, which takes it. */
	ret = pw_jm_hw_reset_collect(dev, &ev);

	jm_lock(s);
	s->st.resets++;
	for (uint32_t js = 0; js < PW_JM_NUM_SLOTS; js++)
		slot_after_reset(s, js, &ev, &done);
	if (ret) {
		pw_err(dev, "GPU reset failed (%d): failing every queued job", ret);
		s->dead = true;
	}
	s->resetting = false;
	feed_all(s, &done);
	jm_unlock(s);
	jm_finish(s, &done);
}

/* Current jobs past their deadline; *@next_due = the earliest deadline still ahead (0 if none).
 * With @mark, flag them guilty and log them (panfrost_job.c:panfrost_job_timedout). */
static bool scan_deadlines(struct pw_jm_sched *s, bool mark, uint64_t *next_due)
{
	uint64_t now = jm_now(s);
	bool expired = false;

	*next_due = 0;
	for (uint32_t js = 0; js < PW_JM_NUM_SLOTS; js++) {
		struct jm_slot *sl = &s->slot[js];
		struct pw_jm_job *job = sl->nhw ? sl->hw[0] : NULL;

		if (!job || !job->deadline_ns)
			continue;
		if (now >= job->deadline_ns) {
			expired = true;
			if (mark && !job->guilty) {
				job->guilty = true;
				s->st.timeouts++;
				pw_err(s->dev, "gpu sched timeout, js=%u, status=0x%x, head=0x%llx, "
				       "tail=0x%llx, jc=0x%llx", js, pw_jm_slot_status(s->dev, js),
				       (unsigned long long)pw_jm_slot_head(s->dev, js),
				       (unsigned long long)pw_jm_slot_tail(s->dev, js),
				       (unsigned long long)job->jc);
			}
		} else if (!*next_due || job->deadline_ns < *next_due) {
			*next_due = job->deadline_ns;
		}
	}
	return expired;
}

static void jm_wd_work(void *arg)
{
	struct pw_jm_sched *s = arg;
	struct pw_device *dev = s->dev;
	struct pw_irq *irq = &dev->irq[PW_IRQ_JOB];
	uint64_t next_due;
	bool expired;

	pw_os_mutex_lock(s->lock);
	s->wd_armed = false;
	if (s->stopping || s->dead) {
		pw_os_mutex_unlock(s->lock);
		return;
	}
	expired = scan_deadlines(s, false, &next_due);
	if (!expired) {
		if (next_due)
			wd_arm(s, next_due);
		pw_os_mutex_unlock(s->lock);
		return;
	}
	pw_os_mutex_unlock(s->lock);

	/* panfrost_job.c:panfrost_job_timedout: a slow bottom half is not a hang. Wait for one
	 * that is running (synchronize_irq), then look again. */
	if (irq->bh_lock) {
		pw_os_mutex_lock(irq->bh_lock);
		pw_os_mutex_unlock(irq->bh_lock);
	}
	pw_os_mutex_lock(s->lock);
	expired = scan_deadlines(s, true, &next_due);
	if (!expired) {
		pw_warn(dev, "unexpectedly high interrupt latency");
		if (next_due)
			wd_arm(s, next_due);
		pw_os_mutex_unlock(s->lock);
		return;
	}
	pw_os_mutex_unlock(s->lock);
	jm_reset(s);
}

/* ------------------------------------------------------------------------------------------ */
/* work item: free finished jobs, resolve dependencies, feed                                  */
/* ------------------------------------------------------------------------------------------ */

static void jm_work(void *arg)
{
	struct pw_jm_sched *s = arg;
	struct jm_jlist done = { 0 };
	struct pw_jm_job *job, *next;

	pw_os_mutex_lock(s->clean_lock);
	job = s->clean.head;
	s->clean.head = s->clean.tail = NULL;
	pw_os_mutex_unlock(s->clean_lock);
	for (; job; job = next) {
		next = job->next;
		job_free(job);
	}

	jm_lock(s);
	feed_all(s, &done);
	jm_unlock(s);
	jm_finish(s, &done);
}

/* ------------------------------------------------------------------------------------------ */
/* context destruction                                                                        */
/* ------------------------------------------------------------------------------------------ */

/*
 * panfrost_job.c:panfrost_jm_ctx_destroy: the context's jobs never start again. A job in NEXT
 * is cancelled (and hard-stopped with its own JOB_CHAIN_FLAG in case it was just promoted), the
 * current one hard-stopped with its flag, so another context's chain in the same slot keeps
 * running; their events complete them with -ECANCELED. Requeued and queued jobs never start
 * again; they are cancelled once their dependencies signalled (here, slot_feed, ent_reap).
 * s->lock held.
 */
static void ctx_kill_locked(struct pw_jm_sched *s, struct pw_jm_ctx *ctx, struct jm_jlist *done)
{
	struct pw_device *dev = s->dev;

	if (ctx->destroyed)
		return;
	ctx->destroyed = true;
	for (uint32_t js = 0; js < PW_JM_NUM_SLOTS; js++) {
		struct jm_slot *sl = &s->slot[js];
		struct jm_jlist keep = { 0 };
		struct pw_jm_job *job;

		while ((job = jl_pop(&sl->requeue))) {
			if (job->ctx == ctx && job_deps_signalled(job))
				job_complete(s, job, PW_ECANCELED, done);
			else
				jl_push(&keep, job);	/* slot_feed() cancels it after its deps */
		}
		sl->requeue = keep;

		for (uint32_t i = sl->nhw; i-- > 0;) {
			job = sl->hw[i];
			if (job->ctx != ctx)
				continue;
			job->killed = true;
			if (s->resetting)
				continue;	/* the reset fails it */
			if (i == 1)
				pw_jm_slot_cancel_next(dev, js);
			pw_jm_slot_stop(dev, js, true, job->chain_flag);
		}
	}
}

/* ------------------------------------------------------------------------------------------ */
/* device lifetime                                                                            */
/* ------------------------------------------------------------------------------------------ */

int pw_jm_sched_init(struct pw_device *dev)
{
	struct pw_jm_sched *s;

	if (dev->jm_sched)
		return PW_OK;
	s = pw_os_zalloc(dev->os, sizeof(*s));
	if (!s)
		return PW_ENOMEM;
	dev->jm_sched = s;	/* published first: release frees a partial init */
	s->dev = dev;
	s->timeout_ms = PW_JM_JOB_TIMEOUT_MS;
	s->depth = pw_jm_slot_depth(dev);
	if (!s->depth || s->depth > JM_MAX_DEPTH)
		s->depth = 1;
	s->bh_lock = dev->irq[PW_IRQ_JOB].bh_lock;	/* created by pw_jm_hw_init */
	s->lock = pw_os_mutex_create(dev->os);
	s->clean_lock = pw_os_mutex_create(dev->os);
	if (!s->lock || !s->clean_lock)
		return PW_ENOMEM;
	s->work = pw_os_work_create(dev->os, jm_work, s);
	s->wd_work = pw_os_work_create(dev->os, jm_wd_work, s);
	if (!s->work || !s->wd_work)
		return PW_ENOMEM;
	pw_dbg(dev, "JM scheduler: %u job(s) per slot, %u ms timeout", s->depth, s->timeout_ms);
	return PW_OK;
}

/* Fail everything still known (all files are closed; the OS layer has stopped the job line, so
 * no event will complete the hardware jobs). */
static void cancel_all_locked(struct pw_jm_sched *s, struct jm_jlist *done)
{
	for (uint32_t js = 0; js < PW_JM_NUM_SLOTS; js++) {
		struct jm_slot *sl = &s->slot[js];
		struct pw_jm_job *job;

		while ((job = hw_pop(sl)))
			job_complete(s, job, PW_ECANCELED, done);
		while ((job = jl_pop(&sl->requeue)))
			job_complete(s, job, PW_ECANCELED, done);
		for (uint32_t p = 0; p < JM_NUM_PRIO; p++) {
			struct jm_entity *e;

			while ((e = sl->rq[p].head)) {
				job = ent_pop(s, e);
				if (job)
					job_complete(s, job, PW_ECANCELED, done);
				else if (e->on_rq)
					rq_del(&sl->rq[p], e);
			}
		}
	}
}

void pw_jm_sched_fini(struct pw_device *dev)
{
	struct pw_jm_sched *s = dev->jm_sched;
	struct jm_jlist done = { 0 };
	uint32_t alive;

	if (!s || s->fini_done || !s->lock)
		return;
	s->fini_done = true;
	pw_os_mutex_lock(s->lock);
	s->stopping = true;	/* no feeding, no watchdog re-arm */
	pw_os_mutex_unlock(s->lock);
	if (s->wd_work) {
		pw_os_work_destroy(dev->os, s->wd_work);	/* cancels a pending timeout */
		s->wd_work = NULL;
	}
	if (!s->work || !s->clean_lock)
		return;
	pw_os_mutex_lock(s->lock);
	cancel_all_locked(s, &done);
	pw_os_mutex_unlock(s->lock);
	if (done.head)
		pw_warn(dev, "JM scheduler stopped with jobs left: cancelled");
	jm_finish(s, &done);
	pw_os_work_flush(s->work);	/* frees them: the MMU is still up */
	alive = __atomic_load_n(&s->st.jobs_alive, __ATOMIC_RELAXED);
	if (alive)
		pw_err(dev, "JM scheduler: %u job(s) still alive at fini", alive);
}

void pw_jm_sched_release(struct pw_device *dev)
{
	struct pw_jm_sched *s = dev->jm_sched;

	if (!s)
		return;
	if (s->wd_work)
		pw_os_work_destroy(dev->os, s->wd_work);
	if (s->work)
		pw_os_work_destroy(dev->os, s->work);
	if (s->clean_lock)
		pw_os_mutex_destroy(dev->os, s->clean_lock);
	if (s->lock)
		pw_os_mutex_destroy(dev->os, s->lock);
	pw_os_free(dev->os, s);
	dev->jm_sched = NULL;
}

void pw_jm_sched_set_job_timeout(struct pw_device *dev, uint32_t timeout_ms)
{
	struct pw_jm_sched *s = dev ? dev->jm_sched : NULL;

	if (!s)
		return;
	__atomic_store_n(&s->timeout_ms, timeout_ms ? timeout_ms : 1u, __ATOMIC_RELAXED);
}

void pw_jm_sched_get_stats(struct pw_device *dev, struct pw_jm_sched_stats *st)
{
	struct pw_jm_sched *s = dev ? dev->jm_sched : NULL;

	pw_memset(st, 0, sizeof(*st));
	if (!s || !s->lock)
		return;
	pw_os_mutex_lock(s->lock);
	*st = s->st;
	pw_os_mutex_unlock(s->lock);
	st->submitted = __atomic_load_n(&s->st.submitted, __ATOMIC_RELAXED);
	st->completed = __atomic_load_n(&s->st.completed, __ATOMIC_RELAXED);
	st->jobs_alive = __atomic_load_n(&s->st.jobs_alive, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------------------------------ */
/* per file                                                                                   */
/* ------------------------------------------------------------------------------------------ */

int pw_jm_sched_file_init(struct pw_file *file)
{
	struct pw_jm_sched *s = file->dev->jm_sched;
	struct pw_jm_sched_file *sf;

	if (!s)
		return PW_EINVAL;
	sf = pw_os_zalloc(file->os, sizeof(*sf));
	if (!sf)
		return PW_ENOMEM;
	sf->lock = pw_os_mutex_create(file->os);
	sf->idle_ev = pw_os_event_create(file->os);
	sf->owner = __atomic_add_fetch(&s->next_owner, 1, __ATOMIC_RELAXED);
	/* panfrost_job.c:panfrost_jm_open: the default context, handle 0, MEDIUM priority. */
	sf->ctx[0] = ctx_alloc(s, PANFROST_JM_CTX_PRIORITY_MEDIUM, sf->owner);
	if (!sf->lock || !sf->idle_ev || !sf->ctx[0]) {
		if (sf->ctx[0])
			ctx_put(sf->ctx[0]);
		if (sf->idle_ev)
			pw_os_event_destroy(file->os, sf->idle_ev);
		if (sf->lock)
			pw_os_mutex_destroy(file->os, sf->lock);
		pw_os_free(file->os, sf);
		return PW_ENOMEM;
	}
	file->jm_sched = sf;
	return PW_OK;
}

/*
 * Whether a killed job of the closing file may still be executing: it is the current job of a
 * slot (hard-stopped, its event is due). A killed job that waited in NEXT behind another
 * context's chain was cancelled there and cannot start (if the JM had promoted it just before
 * the cancel, the hard stop aimed at its JOB_CHAIN_FLAG ends it); it completes with that
 * slot's next event, which may be much later, so it is not waited for. Requeued and queued
 * jobs of a destroyed context never start. Matched by the file id, so a context the file
 * destroyed earlier with JM_CTX_DESTROY counts too (review fix: it was missed). s->lock held.
 */
static bool file_busy_locked(struct pw_jm_sched *s, uint64_t owner)
{
	for (uint32_t js = 0; js < PW_JM_NUM_SLOTS; js++) {
		const struct jm_slot *sl = &s->slot[js];

		if (sl->nhw && sl->hw[0]->ctx->owner == owner)
			return true;
	}
	return false;
}

/* panfrost_job.c:panfrost_jm_close: destroy every context, then wait (without holding any lock)
 * until none of their jobs can still execute (file_busy_locked). Jobs still waiting for
 * dependencies stay queued and are cancelled once those signal; they no longer refer to the
 * file. Linux does not wait at all (jobs hold their references); the wait is this core's
 * contract (pw_jm_sched.h). */
void pw_jm_sched_file_fini(struct pw_file *file)
{
	struct pw_jm_sched_file *sf = file->jm_sched;
	struct pw_jm_sched *s = file->dev->jm_sched;
	struct pw_jm_ctx *ctxs[JM_NUM_CTX];
	struct jm_jlist done = { 0 };

	if (!sf)
		return;
	file->jm_sched = NULL;
	pw_os_mutex_lock(sf->lock);
	for (uint32_t i = 0; i < JM_NUM_CTX; i++) {
		ctxs[i] = sf->ctx[i];
		sf->ctx[i] = NULL;
	}
	pw_os_mutex_unlock(sf->lock);

	jm_lock(s);
	for (uint32_t i = 0; i < JM_NUM_CTX; i++) {
		if (!ctxs[i])
			continue;
		ctxs[i]->idle_ev = sf->idle_ev;
		ctx_kill_locked(s, ctxs[i], &done);
	}
	feed_all(s, &done);
	jm_unlock(s);
	jm_finish(s, &done);

	for (;;) {
		bool busy;

		pw_os_mutex_lock(s->lock);
		busy = file_busy_locked(s, sf->owner);
		if (!busy) {
			for (uint32_t i = 0; i < JM_NUM_CTX; i++) {
				if (ctxs[i])
					ctxs[i]->idle_ev = NULL;
			}
		} else {
			pw_os_event_reset(sf->idle_ev);
		}
		pw_os_mutex_unlock(s->lock);
		if (!busy)
			break;
		/* The hard stops complete them; a chain that ignores them times out and the reset
		 * fails it. The poll interval bounds a missed wake-up, and is the only wake-up for
		 * a context destroyed before the close (its idle_ev is not set). */
		pw_os_event_wait(sf->idle_ev, JM_FILE_FINI_POLL_MS);
	}

	for (uint32_t i = 0; i < JM_NUM_CTX; i++)
		ctx_put(ctxs[i]);
	pw_os_event_destroy(file->os, sf->idle_ev);
	pw_os_mutex_destroy(file->os, sf->lock);
	pw_os_free(file->os, sf);
}

/* A reference on context @handle of @file, or NULL. */
static struct pw_jm_ctx *file_ctx_get(struct pw_file *file, uint32_t handle)
{
	struct pw_jm_sched_file *sf = file->jm_sched;
	struct pw_jm_ctx *ctx = NULL;

	if (!sf || handle >= JM_NUM_CTX)
		return NULL;
	pw_os_mutex_lock(sf->lock);
	ctx = ctx_get(sf->ctx[handle]);
	pw_os_mutex_unlock(sf->lock);
	return ctx;
}

/* ------------------------------------------------------------------------------------------ */
/* ioctls                                                                                     */
/* ------------------------------------------------------------------------------------------ */

/* panfrost_drv.c:panfrost_copy_in_sync -> drm_sched_job_add_syncobj_dependency ->
 * drm_syncobj_find_fence: no syncobj -> ENOENT, no fence -> EINVAL. Already signalled fences
 * are dropped. */
static int submit_in_syncs(struct pw_file *file, const struct drm_panfrost_submit *args,
			   struct pw_fence **deps, uint32_t *ndeps)
{
	uint32_t *h;
	int ret = PW_OK;

	if (!args->in_sync_count)
		return PW_OK;
	h = pw_os_zalloc(file->os, (size_t)args->in_sync_count * sizeof(*h));
	if (!h)
		return PW_ENOMEM;
	ret = pw_os_copy_from_user(file->os, h, args->in_syncs,
				   (size_t)args->in_sync_count * sizeof(*h));
	for (uint32_t i = 0; !ret && i < args->in_sync_count; i++) {
		struct pw_syncobj *so = pw_syncobj_lookup(file, h[i]);
		struct pw_fence *f = NULL;

		if (!so) {
			ret = PW_ENOENT;
			break;
		}
		pw_syncobj_put(so);
		ret = pw_syncobj_find_fence(file, h[i], 0, 0, 0, &f);
		if (ret)
			break;
		if (pw_fence_is_signalled(f))
			pw_fence_put(f);
		else
			deps[(*ndeps)++] = f;
	}
	pw_os_free(file->os, h);
	return ret;
}

int pw_jm_ioctl_submit(struct pw_file *file, void *arg)
{
	struct drm_panfrost_submit *args = arg;
	struct pw_device *dev = file->dev;
	struct pw_jm_sched *s = dev->jm_sched;
	struct pw_syncobj *out = NULL;
	struct pw_jm_ctx *ctx = NULL;
	struct pw_jm_job *job = NULL;
	struct pw_fence *fence;
	struct jm_jlist done = { 0 };
	struct jm_entity *e;
	uint64_t ndeps_max;
	int ret;

	/* panfrost_drv.c:panfrost_ioctl_submit, in its order. */
	if (args->pad)
		return PW_EINVAL;
	if (!args->jc)
		return PW_EINVAL;
	if (args->requirements & ~(uint32_t)(PANFROST_JD_REQ_FS | PANFROST_JD_REQ_CYCLE_COUNT))
		return PW_EINVAL;
	if (!s || !file->jm_sched)
		return PW_EINVAL;
	if (args->out_sync) {
		out = pw_syncobj_lookup(file, args->out_sync);
		if (!out)
			return PW_ENODEV;
	}
	ctx = file_ctx_get(file, args->jm_ctx_handle);
	if (!ctx) {
		ret = PW_EINVAL;
		goto out_put;
	}
	if (args->in_sync_count > PW_JM_MAX_IN_SYNCS) {
		ret = PW_ENOMEM;
		goto out_put;
	}

	job = pw_os_zalloc(file->os, sizeof(*job));
	if (!job) {
		ret = PW_ENOMEM;
		goto out_put;
	}
	__atomic_add_fetch(&s->st.jobs_alive, 1, __ATOMIC_RELAXED);
	job->s = s;
	job->ctx = ctx;		/* the lookup reference becomes the job's */
	ctx = NULL;
	job->jc = args->jc;
	job->reqs = args->requirements;
	job->as = -1;
	job->slot = (args->requirements & PANFROST_JD_REQ_FS) ? 0u : 1u;	/* panfrost_job_get_slot */
	/* Too many BOs fail in pw_jm_bos_lookup() (ENOMEM), after the in_syncs, as in Linux. */
	ndeps_max = (uint64_t)args->in_sync_count +
		    PW_MIN(args->bo_handle_count, PW_JM_MAX_JOB_BOS);
	if (ndeps_max) {
		job->deps = pw_os_zalloc(file->os, (size_t)ndeps_max * sizeof(*job->deps));
		if (!job->deps) {
			ret = PW_ENOMEM;
			goto out_free;
		}
	}
	ret = submit_in_syncs(file, args, job->deps, &job->ndeps);
	if (ret)
		goto out_free;
	ret = pw_jm_bos_lookup(file, args->bo_handles, args->bo_handle_count, &job->bos);
	if (ret)
		goto out_free;
	job->nbos = args->bo_handle_count;
	job->done = pw_fence_create(dev, __atomic_add_fetch(&s->seqno, 1, __ATOMIC_RELAXED));
	if (!job->done) {
		ret = PW_ENOMEM;
		goto out_free;
	}

	/* No failure from here on. */
	fence = pw_fence_get(job->done);
	job->jvm = pw_jm_vm_get(pw_jm_file_vm(file));
	job->flush_id = pw_jm_latest_flush_id(dev);
	job->timeout_ms = __atomic_load_n(&s->timeout_ms, __ATOMIC_RELAXED);
	if (job->reqs & PANFROST_JD_REQ_CYCLE_COUNT) {
		pw_jm_cycle_counter_get(dev);	/* held while queued (pw_jm_hw.h) */
		job->cc = true;
	}

	/*
	 * panfrost_job.c:panfrost_job_push: under the reservation locks, every BO becomes an
	 * implicit dependency in write mode (panfrost_acquire_object_fences), the job is queued
	 * and its fence becomes the BO's (panfrost_attach_object_fences, DMA_RESV_USAGE_WRITE).
	 */
	pw_jm_resv_lock(dev);
	for (uint32_t i = 0; i < job->nbos; i++) {
		struct pw_fence *f = pw_jm_bo_fence_get(job->bos[i]);

		if (f && !pw_fence_is_signalled(f))
			job->deps[job->ndeps++] = f;
		else
			pw_fence_put(f);
	}
	for (uint32_t i = 0; i < job->nbos; i++)
		pw_jm_bo_fence_set(job->bos[i], job->done);

	jm_lock(s);
	e = &job->ctx->ent[job->slot];
	job->state = JM_JOB_QUEUED;
	jl_push(&e->q, job);
	if (!e->on_rq)
		rq_add(ent_rq(s, e), e);
	s->st.submitted++;
	slot_feed(s, job->slot, &done);
	jm_unlock(s);
	pw_jm_resv_unlock(dev);
	jm_finish(s, &done);

	/* panfrost_ioctl_submit: drm_syncobj_replace_fence(sync_out, render_done_fence). The job
	 * may be done and freed by now, hence our own fence reference. */
	if (out) {
		pw_syncobj_replace_fence(out, fence);
		pw_syncobj_put(out);
	}
	pw_fence_put(fence);
	return PW_OK;

out_free:
	for (uint32_t i = 0; i < job->ndeps; i++)
		pw_fence_put(job->deps[i]);
	pw_os_free(file->os, job->deps);
	pw_jm_bos_put(dev, job->bos, job->nbos);
	pw_fence_put(job->done);
	ctx_put(job->ctx);
	pw_os_free(file->os, job);
	__atomic_sub_fetch(&s->st.jobs_alive, 1, __ATOMIC_RELAXED);
out_put:
	ctx_put(ctx);
	pw_syncobj_put(out);
	return ret;
}

/* panfrost_job.c:panfrost_jm_ctx_create (jm_ctx_prio_to_drm_sched_prio, xa_alloc in [0, 64]). */
int pw_jm_ioctl_jm_ctx_create(struct pw_file *file, void *arg)
{
	struct drm_panfrost_jm_ctx_create *args = arg;
	struct pw_jm_sched_file *sf = file->jm_sched;
	struct pw_jm_sched *s = file->dev->jm_sched;
	struct pw_jm_ctx *ctx;
	int ret = PW_EBUSY;

	if (!sf || !s)
		return PW_EINVAL;
	switch (args->priority) {
	case PANFROST_JM_CTX_PRIORITY_LOW:
	case PANFROST_JM_CTX_PRIORITY_MEDIUM:
		break;
	case PANFROST_JM_CTX_PRIORITY_HIGH:
		return PW_EACCES;	/* no CAP_SYS_NICE / DRM master equivalent (pw_jm_hw.h) */
	default:
		return PW_EINVAL;
	}
	ctx = ctx_alloc(s, args->priority, sf->owner);
	if (!ctx)
		return PW_ENOMEM;
	pw_os_mutex_lock(sf->lock);
	for (uint32_t h = 0; h < JM_NUM_CTX; h++) {
		if (!sf->ctx[h]) {
			sf->ctx[h] = ctx;
			ctx->handle = h;
			args->handle = h;
			ret = PW_OK;
			break;
		}
	}
	pw_os_mutex_unlock(sf->lock);
	if (ret)
		ctx_put(ctx);
	return ret;
}

/* panfrost_drv.c:panfrost_ioctl_jm_ctx_destroy + panfrost_job.c:panfrost_jm_ctx_destroy. Does
 * not wait for the GPU. */
int pw_jm_ioctl_jm_ctx_destroy(struct pw_file *file, void *arg)
{
	struct drm_panfrost_jm_ctx_destroy *args = arg;
	struct pw_jm_sched_file *sf = file->jm_sched;
	struct pw_jm_sched *s = file->dev->jm_sched;
	struct pw_jm_ctx *ctx = NULL;
	struct jm_jlist done = { 0 };

	if (args->pad)
		return PW_EINVAL;
	if (!args->handle)
		return PW_EINVAL;	/* the default context lives as long as the file */
	if (!sf || !s || args->handle >= JM_NUM_CTX)
		return PW_EINVAL;
	pw_os_mutex_lock(sf->lock);
	ctx = sf->ctx[args->handle];
	sf->ctx[args->handle] = NULL;
	pw_os_mutex_unlock(sf->lock);
	if (!ctx)
		return PW_EINVAL;

	jm_lock(s);
	ctx_kill_locked(s, ctx, &done);
	feed_all(s, &done);
	jm_unlock(s);
	jm_finish(s, &done);
	ctx_put(ctx);
	return PW_OK;
}
