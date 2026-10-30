// SPDX-License-Identifier: MIT
/*
 * pw_sync.c - kernel fences (pw_fence.h) and DRM syncobjs, binary and timeline (pw_sync.h).
 *
 * The user-visible semantics follow Linux drivers/gpu/drm/drm_syncobj.c and
 * drivers/dma-buf/dma-fence-chain.c (v7.2). Those files are GPL-2.0 and were read for their
 * semantics only; this is an independent implementation with a different data structure:
 *
 *  - A fence is a refcounted one-shot completion with its own mutex (absent for fences created
 *    signalled), a callback list and a lazily created event for pw_fence_wait().
 *  - A timeline syncobj points to a struct pw_tl: an ordered list of nodes (point, payload fence),
 *    oldest first. A node is the equivalent of one dma_fence_chain link: it carries prev_seqno and
 *    a chain "context", so dma_fence_chain_find_seqno() and the QUERY walk can be reproduced
 *    node-for-node, including the unordered-point rule of dma_fence_chain_init() (a point <= the
 *    last one starts a new context with seqno = max(last, point) and prev_seqno = 0).
 *  - Linux garbage-collects chain links whose payload is signalled when it walks the chain; here a
 *    walk simply skips non-head nodes whose payload is signalled, which gives the same answers.
 *  - The "chain fence" of a node (signalled once its payload and every older payload are) is only
 *    allocated when somebody asks for that point while older points are still pending; otherwise
 *    the payload itself is handed out. Nodes are popped from the front and their chain fences
 *    signalled, in order, by tl_advance(), which runs from the payload callbacks: no recursion,
 *    however long the timeline. Payload callbacks triggered by those chain-fence signals (a chain
 *    fence used as another point's payload, e.g. after TRANSFER) are queued by fence_signal() and
 *    completed by the same tl_advance() loop, so the stack depth stays constant there too.
 *  - The timeline is refcounted separately from the syncobj (pending payload callbacks hold a
 *    reference), so chain fences handed out to the scheduler still signal after a RESET, a binary
 *    replace or a DESTROY of the syncobj, like a dma_fence_chain that holds its links.
 *
 * Lock order: syncobj->lock -> tl->lock -> fence->lock; notify registry lock is taken alone or
 * after syncobj->lock. Fence callbacks run with no fence/timeline lock held, but possibly with a
 * syncobj lock held (an attach that fires a notify immediately); none of them takes a syncobj lock.
 */

#include "pw_sync.h"

#define pw_container_of(ptr, type, member) \
	((type *)(void *)((char *)(ptr) - offsetof(type, member)))

/* Linux DRM_SYNCOBJ_WAIT_FOR_SUBMIT_TIMEOUT (drm_syncobj.c:419): TRANSFER with WAIT_FOR_SUBMIT. */
#define PW_SYNC_TRANSFER_SUBMIT_TIMEOUT_NS 5000000000ull

/* ------------------------------------------------------------------------------------------ */
/* small intrusive list                                                                       */
/* ------------------------------------------------------------------------------------------ */

struct pw_sl {
	struct pw_sl *next, *prev;
};

static void sl_init(struct pw_sl *h)
{
	h->next = h->prev = h;
}

static bool sl_empty(const struct pw_sl *h)
{
	return h->next == h;
}

static void sl_add_tail(struct pw_sl *h, struct pw_sl *n)
{
	n->prev = h->prev;
	n->next = h;
	h->prev->next = n;
	h->prev = n;
}

static void sl_del(struct pw_sl *n)
{
	n->prev->next = n->next;
	n->next->prev = n->prev;
	n->next = n->prev = n;
}

/* ------------------------------------------------------------------------------------------ */
/* fences                                                                                     */
/* ------------------------------------------------------------------------------------------ */

struct pw_fence {
	uint32_t refcount;		/* __atomic */
	uint32_t signalled;		/* __atomic; written under @lock (or at creation) */
	int error;			/* valid once signalled */
	uint64_t seqno;
	uint64_t timestamp;		/* pw_os_time_ns() at signal time */
	struct pw_os *os;
	struct pw_os_mutex *lock;	/* NULL for fences created signalled */
	struct pw_os_event *event;	/* lazily, for pw_fence_wait(); under @lock */
	struct pw_fence_cb cbs;		/* circular list sentinel, under @lock */
	/* private to the timeline code: deferred-signal list built under a timeline lock */
	struct pw_fence *sig_next;
	int sig_error;
};

static bool fence_signalled(const struct pw_fence *f)
{
	return __atomic_load_n(&f->signalled, __ATOMIC_ACQUIRE) != 0;
}

struct pw_fence *pw_fence_create(struct pw_device *dev, uint64_t seqno)
{
	struct pw_fence *f = pw_os_zalloc(dev->os, sizeof(*f));

	if (!f)
		return NULL;
	f->lock = pw_os_mutex_create(dev->os);
	if (!f->lock) {
		pw_os_free(dev->os, f);
		return NULL;
	}
	f->os = dev->os;
	f->refcount = 1;
	f->seqno = seqno;
	f->cbs.next = f->cbs.prev = &f->cbs;
	return f;
}

/* Linux: dma_fence_allocate_private_stub(ktime_get()) (dma-fence.c) for SIGNAL/CREATE_SIGNALED. */
struct pw_fence *pw_fence_create_signalled(struct pw_device *dev, int error)
{
	struct pw_fence *f = pw_os_zalloc(dev->os, sizeof(*f));

	if (!f)
		return NULL;
	f->os = dev->os;
	f->refcount = 1;
	f->error = error > 0 ? -error : error;
	f->timestamp = pw_os_time_ns(dev->os);
	f->cbs.next = f->cbs.prev = &f->cbs;
	f->signalled = 1;
	return f;
}

struct pw_fence *pw_fence_get(struct pw_fence *f)
{
	if (f)
		__atomic_fetch_add(&f->refcount, 1, __ATOMIC_RELAXED);
	return f;
}

void pw_fence_put(struct pw_fence *f)
{
	if (!f || __atomic_sub_fetch(&f->refcount, 1, __ATOMIC_ACQ_REL) != 0)
		return;
	if (f->event)
		pw_os_event_destroy(f->os, f->event);
	if (f->lock)
		pw_os_mutex_destroy(f->os, f->lock);
	pw_os_free(f->os, f);
}

struct pw_tl_node;
static void tl_node_cb(struct pw_fence *f, struct pw_fence_cb *cb);
static void tl_defer_node(struct pw_fence_cb *cb, struct pw_tl_node **defer);

/*
 * @defer (timeline code only): instead of calling the timeline's own payload callback
 * (tl_node_cb), queue its node on *@defer for the caller to complete iteratively. Without this a
 * chain fence that is the payload of another point (TRANSFER h:k -> h:k+1, any number of times)
 * would recurse pw_fence_signal -> tl_node_cb -> tl_advance -> pw_fence_signal once per link, to
 * a depth userspace controls (Linux avoids the same recursion by bouncing dma_fence_chain_cb
 * through irq_work).
 */
static bool fence_signal(struct pw_fence *f, int error, struct pw_tl_node **defer)
{
	struct pw_fence_cb *cb, *next;

	if (!f || fence_signalled(f))
		return false;
	pw_os_mutex_lock(f->lock);
	if (fence_signalled(f)) {
		pw_os_mutex_unlock(f->lock);
		return false;
	}
	f->error = error > 0 ? -error : error;
	f->timestamp = pw_os_time_ns(f->os);
	__atomic_store_n(&f->signalled, 1, __ATOMIC_RELEASE);
	/* Detach the whole callback list; remove_callback() now returns false for all of them. */
	cb = f->cbs.next;
	if (cb == &f->cbs) {
		cb = NULL;
	} else {
		f->cbs.prev->next = NULL;
	}
	f->cbs.next = f->cbs.prev = &f->cbs;
	if (f->event)
		pw_os_event_signal(f->event);
	pw_os_mutex_unlock(f->lock);

	/* The callback's owner may free @cb from within fn (or right after it ran), so read the
	 * link first. Owners cannot free a callback that has not run yet (pw_fence.h contract). */
	for (; cb; cb = next) {
		pw_fence_cb_fn fn = cb->fn;

		next = cb->next;
		cb->next = cb->prev = cb;
		if (defer && fn == tl_node_cb)
			tl_defer_node(cb, defer);
		else
			fn(f, cb);
	}
	return true;
}

bool pw_fence_signal(struct pw_fence *f, int error)
{
	return fence_signal(f, error, NULL);
}

bool pw_fence_is_signalled(struct pw_fence *f)
{
	return fence_signalled(f);
}

int pw_fence_error(struct pw_fence *f)
{
	return fence_signalled(f) ? f->error : 0;
}

uint64_t pw_fence_seqno(struct pw_fence *f)
{
	return f->seqno;
}

uint64_t pw_fence_signal_time_ns(struct pw_fence *f)
{
	return fence_signalled(f) ? f->timestamp : 0;
}

int pw_fence_add_callback(struct pw_fence *f, struct pw_fence_cb *cb, pw_fence_cb_fn fn)
{
	if (fence_signalled(f)) {
		cb->next = cb->prev = cb;
		return PW_ENOENT;
	}
	pw_os_mutex_lock(f->lock);
	if (fence_signalled(f)) {
		pw_os_mutex_unlock(f->lock);
		cb->next = cb->prev = cb;
		return PW_ENOENT;
	}
	cb->fn = fn;
	cb->prev = f->cbs.prev;
	cb->next = &f->cbs;
	f->cbs.prev->next = cb;
	f->cbs.prev = cb;
	pw_os_mutex_unlock(f->lock);
	return PW_OK;
}

bool pw_fence_remove_callback(struct pw_fence *f, struct pw_fence_cb *cb)
{
	bool removed = false;

	if (fence_signalled(f))
		return false;
	pw_os_mutex_lock(f->lock);
	/* Signalled: the callback ran or is running (the signaller owns the detached list).
	 * cb->next == cb: already removed. */
	if (!fence_signalled(f) && cb->next && cb->next != cb) {
		cb->prev->next = cb->next;
		cb->next->prev = cb->prev;
		cb->next = cb->prev = cb;
		removed = true;
	}
	pw_os_mutex_unlock(f->lock);
	return removed;
}

int pw_fence_wait(struct pw_fence *f, uint64_t deadline_ns)
{
	struct pw_os_event *ev;
	int ret;

	if (fence_signalled(f))
		return PW_OK;
	if (deadline_ns != UINT64_MAX && pw_os_time_ns(f->os) >= deadline_ns)
		return PW_ETIME;
	pw_os_mutex_lock(f->lock);
	if (fence_signalled(f)) {
		pw_os_mutex_unlock(f->lock);
		return PW_OK;
	}
	if (!f->event) {
		/* Manual-reset and signalled by pw_fence_signal() under the lock: shared by every
		 * waiter, no lost wake-up. */
		f->event = pw_os_event_create(f->os);
		if (!f->event) {
			pw_os_mutex_unlock(f->lock);
			return PW_ENOMEM;
		}
	}
	ev = f->event;
	pw_os_mutex_unlock(f->lock);

	ret = pw_os_event_wait_user(ev, deadline_ns);
	if (fence_signalled(f))
		return PW_OK;
	return ret == PW_EINTR ? PW_EINTR : PW_ETIME;
}

/* ------------------------------------------------------------------------------------------ */
/* timelines (dma_fence_chain equivalent)                                                     */
/* ------------------------------------------------------------------------------------------ */

struct pw_tl;

struct pw_tl_node {
	struct pw_tl_node *prev, *next;	/* older / newer neighbour in tl's list */
	struct pw_tl *tl;
	struct pw_fence *payload;
	struct pw_fence *chain;		/* lazily; signals when payload and all older are done */
	struct pw_fence_cb cb;		/* on payload */
	uint64_t point;			/* dma_fence_chain base.seqno */
	uint64_t prev_seqno;		/* dma_fence_chain prev_seqno */
	uint64_t context;		/* dma_fence_chain base.context (unique per timeline) */
	bool cb_registered;		/* cb was added to payload ... */
	bool cb_done;			/* ... and has run */
	bool detached;			/* popped before its callback ran: the callback frees it */
	bool chain_signalled;
	struct pw_tl_node *free_next;
	struct pw_tl_node *defer_next;	/* fence_signal() deferral list */
};

struct pw_tl {
	uint32_t refcount;		/* __atomic: owning syncobj + pending node callbacks + temps */
	struct pw_device *dev;
	struct pw_os *os;
	struct pw_os_mutex *lock;
	struct pw_tl_node *first, *last;	/* oldest not-yet-complete node .. head */
	uint64_t next_context;
};

struct pw_syncobj_point {
	struct pw_device *dev;
	struct pw_tl_node *node;	/* the new point */
	struct pw_tl_node *base;	/* wraps a binary fence the timeline is built on */
	struct pw_tl *tl;		/* if the syncobj has no timeline yet */
};

static void tl_node_free(struct pw_os *os, struct pw_tl_node *n)
{
	pw_fence_put(n->payload);
	pw_fence_put(n->chain);
	pw_os_free(os, n);
}

static struct pw_tl *tl_alloc(struct pw_device *dev)
{
	struct pw_tl *tl = pw_os_zalloc(dev->os, sizeof(*tl));

	if (!tl)
		return NULL;
	tl->lock = pw_os_mutex_create(dev->os);
	if (!tl->lock) {
		pw_os_free(dev->os, tl);
		return NULL;
	}
	tl->dev = dev;
	tl->os = dev->os;
	tl->refcount = 1;
	tl->next_context = 1;
	return tl;
}

static void tl_get(struct pw_tl *tl)
{
	__atomic_fetch_add(&tl->refcount, 1, __ATOMIC_RELAXED);
}

static void tl_put(struct pw_tl *tl)
{
	struct pw_tl_node *n, *next;

	if (!tl || __atomic_sub_fetch(&tl->refcount, 1, __ATOMIC_ACQ_REL) != 0)
		return;
	/* No syncobj and no pending payload callback: every payload has signalled and the last
	 * tl_advance() completed every node. Signal defensively anyway so nobody can hang. */
	for (n = tl->first; n; n = next) {
		next = n->next;
		if (n->chain && !n->chain_signalled)
			pw_fence_signal(n->chain, pw_fence_is_signalled(n->payload) ?
					pw_fence_error(n->payload) : PW_ECANCELED);
		tl_node_free(tl->os, n);
	}
	pw_os_mutex_destroy(tl->os, tl->lock);
	pw_os_free(tl->os, tl);
}

/* Fence that signals when @n's payload and all older payloads have signalled. */
static int tl_chain_fence_locked(struct pw_tl *tl, struct pw_tl_node *n, struct pw_fence **out)
{
	if (n == tl->first) {
		*out = pw_fence_get(n->payload);
		return PW_OK;
	}
	if (!n->chain) {
		n->chain = pw_fence_create(tl->dev, n->point);
		if (!n->chain)
			return PW_ENOMEM;
	}
	*out = pw_fence_get(n->chain);
	return PW_OK;
}

/* Next older node a dma_fence_chain walk would reach: links whose payload is signalled are
 * garbage-collected by dma_fence_chain_walk() (dma-fence-chain.c:39-76), so skip them. */
static struct pw_tl_node *tl_prev_live(struct pw_tl_node *n)
{
	struct pw_tl_node *m = n->prev;

	while (m && pw_fence_is_signalled(m->payload))
		m = m->prev;
	return m;
}

/* drm_syncobj_fence_get() + dma_fence_chain_find_seqno() (dma-fence-chain.c:89-107):
 * PW_OK with *out = fence (NULL: already signalled and collected, Linux returns the stub),
 * PW_EINVAL if @point has not been submitted, PW_ENOMEM. */
static int tl_find_locked(struct pw_tl *tl, uint64_t point, struct pw_fence **out)
{
	struct pw_tl_node *head = tl->last, *n = head;

	if (!point)
		return tl_chain_fence_locked(tl, head, out);
	if (head->point < point)
		return PW_EINVAL;
	for (;;) {
		if (n->context != head->context || n->prev_seqno < point)
			break;
		n = tl_prev_live(n);
		if (!n) {
			*out = NULL;
			return PW_OK;
		}
	}
	return tl_chain_fence_locked(tl, n, out);
}

static bool tl_chain_signalled_locked(struct pw_tl *tl, struct pw_tl_node *n)
{
	for (struct pw_tl_node *m = tl->first; m; m = m->next) {
		if (!pw_fence_is_signalled(m->payload))
			return false;
		if (m == n)
			return true;
	}
	return true;
}

/* drm_syncobj_query_ioctl() (drm_syncobj.c:1633-1697): last signalled point. */
static uint64_t tl_query_locked(struct pw_tl *tl, uint32_t flags)
{
	struct pw_tl_node *head = tl->last, *last = head, *m;

	if (flags & DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED)
		return head->point;
	/* Walk to the oldest live node of the head's context (an unordered point starts a new
	 * context, which ends the walk). */
	for (m = tl_prev_live(head); m && m->context == head->context; m = tl_prev_live(m))
		last = m;
	return tl_chain_signalled_locked(tl, last) ? last->point : last->prev_seqno;
}

static void tl_append_locked(struct pw_tl *tl, struct pw_tl_node *n, struct pw_fence *payload,
			     uint64_t point, uint64_t prev_seqno, uint64_t context)
{
	n->tl = tl;
	n->payload = payload;		/* reference transferred by the caller */
	n->point = point;
	n->prev_seqno = prev_seqno;
	n->context = context;
	n->prev = tl->last;
	n->next = NULL;
	if (tl->last)
		tl->last->next = n;
	else
		tl->first = n;
	tl->last = n;

	/* The callback holds a timeline reference. Registered under tl->lock so cb_done (set by
	 * the callback under the same lock) can never precede cb_registered. */
	tl_get(tl);
	n->cb_registered = true;
	if (pw_fence_add_callback(payload, &n->cb, tl_node_cb)) {
		n->cb_registered = false;
		__atomic_fetch_sub(&tl->refcount, 1, __ATOMIC_RELAXED);	/* caller holds one */
	}
}

/* Pop completed nodes from the front and signal their chain fences, oldest first. Payload
 * callbacks of other points that those signals trigger are queued on *@defer, not run.
 * The caller holds a timeline reference. */
static void tl_advance_one(struct pw_tl *tl, struct pw_tl_node **defer)
{
	struct pw_tl_node *n, *free_list = NULL;
	struct pw_fence *sig = NULL, **sig_tail = &sig, *f;

	pw_os_mutex_lock(tl->lock);
	while ((n = tl->first) && pw_fence_is_signalled(n->payload)) {
		if (n->chain && !n->chain_signalled) {
			n->chain_signalled = true;
			if (n == tl->last) {
				f = pw_fence_get(n->chain);	/* the head keeps its reference */
			} else {
				f = n->chain;
				n->chain = NULL;
			}
			f->sig_error = pw_fence_error(n->payload);
			f->sig_next = NULL;
			*sig_tail = f;
			sig_tail = &f->sig_next;
		}
		if (n == tl->last)
			break;		/* the head stays: it is the syncobj's current point */
		tl->first = n->next;
		n->next->prev = NULL;
		n->next = n->prev = NULL;
		if (n->cb_registered && !n->cb_done) {
			/* Payload signalled but its callback has not run yet (it is queued on the
			 * signaller's detached list): it will free the node. */
			n->detached = true;
			continue;
		}
		n->free_next = free_list;
		free_list = n;
	}
	pw_os_mutex_unlock(tl->lock);

	while (sig) {
		f = sig;
		sig = f->sig_next;
		fence_signal(f, f->sig_error, defer);
		pw_fence_put(f);
	}
	while (free_list) {
		n = free_list;
		free_list = n->free_next;
		tl_node_free(tl->os, n);
	}
}

/* The payload of @n signalled: mark its callback done (freeing @n if it was already popped).
 * Returns its timeline, whose reference the callback held, for the caller to advance and put. */
static struct pw_tl *tl_node_done(struct pw_tl_node *n)
{
	struct pw_tl *tl = n->tl;
	bool free_it;

	pw_os_mutex_lock(tl->lock);
	n->cb_done = true;
	free_it = n->detached;
	pw_os_mutex_unlock(tl->lock);
	if (free_it)
		tl_node_free(tl->os, n);
	return tl;
}

static void tl_defer_node(struct pw_fence_cb *cb, struct pw_tl_node **defer)
{
	struct pw_tl_node *n = pw_container_of(cb, struct pw_tl_node, cb);

	n->defer_next = *defer;
	*defer = n;
}

/* tl_advance_one() on @tl, then on every timeline whose payload callback that deferred, until
 * none is left: constant stack depth however long the chain of chain fences. */
static void tl_advance(struct pw_tl *tl)
{
	struct pw_tl_node *defer = NULL, *n;

	tl_advance_one(tl, &defer);
	while (defer) {
		n = defer;
		defer = n->defer_next;
		tl = tl_node_done(n);
		tl_advance_one(tl, &defer);
		tl_put(tl);	/* the deferred callback's reference */
	}
}

static void tl_node_cb(struct pw_fence *f, struct pw_fence_cb *cb)
{
	struct pw_tl *tl = tl_node_done(pw_container_of(cb, struct pw_tl_node, cb));

	(void)f;
	tl_advance(tl);
	tl_put(tl);		/* the callback's reference */
}

/* ------------------------------------------------------------------------------------------ */
/* syncobjs                                                                                   */
/* ------------------------------------------------------------------------------------------ */

enum pw_so_kind { PW_SO_NONE = 0, PW_SO_PLAIN, PW_SO_TL };

/*
 * A "wait for submit" entry: @fn is called under the syncobj lock, after the entry was unlinked,
 * as soon as a fence for @point is available (@fence: a reference, or NULL if the point is already
 * signalled and collected; @err: PW_ENOMEM if the chain fence could not be allocated). Owners
 * unlink under the same lock, so once they see !linked, fn has completed.
 */
struct pw_syncobj_wait;
typedef void (*pw_so_wait_fn)(struct pw_syncobj_wait *w, struct pw_fence *fence, int err);

struct pw_syncobj_wait {
	struct pw_sl link;
	bool linked;
	uint64_t point;
	pw_so_wait_fn fn;
};

struct pw_syncobj {
	uint32_t refcount;		/* __atomic: handle table + lookups + waiters */
	struct pw_device *dev;
	struct pw_os *os;
	struct pw_os_mutex *lock;
	enum pw_so_kind kind;
	struct pw_fence *plain;		/* PW_SO_PLAIN */
	struct pw_tl *tl;		/* PW_SO_TL */
	struct pw_sl waiters;		/* struct pw_syncobj_wait */
};

static struct pw_syncobj *so_create(struct pw_device *dev)
{
	struct pw_syncobj *so = pw_os_zalloc(dev->os, sizeof(*so));

	if (!so)
		return NULL;
	so->lock = pw_os_mutex_create(dev->os);
	if (!so->lock) {
		pw_os_free(dev->os, so);
		return NULL;
	}
	so->dev = dev;
	so->os = dev->os;
	so->refcount = 1;
	sl_init(&so->waiters);
	return so;
}

struct pw_syncobj *pw_syncobj_get(struct pw_syncobj *so)
{
	if (so)
		__atomic_fetch_add(&so->refcount, 1, __ATOMIC_RELAXED);
	return so;
}

void pw_syncobj_put(struct pw_syncobj *so)
{
	if (!so || __atomic_sub_fetch(&so->refcount, 1, __ATOMIC_ACQ_REL) != 0)
		return;
	/* Every waiter holds a reference, so the list is empty here. */
	pw_fence_put(so->plain);
	tl_put(so->tl);
	pw_os_mutex_destroy(so->os, so->lock);
	pw_os_free(so->os, so);
}

static void so_get_cb(void *obj)
{
	pw_syncobj_get(obj);
}

static void so_put_cb(void *obj)
{
	pw_syncobj_put(obj);
}

/* Current fence for @point, see tl_find_locked(). */
static int so_find_locked(struct pw_syncobj *so, uint64_t point, struct pw_fence **out)
{
	int ret;

	*out = NULL;
	switch (so->kind) {
	case PW_SO_PLAIN:
		/* dma_fence_chain_find_seqno(): a non-chain fence only matches point 0. */
		if (point)
			return PW_EINVAL;
		*out = pw_fence_get(so->plain);
		return PW_OK;
	case PW_SO_TL:
		pw_os_mutex_lock(so->tl->lock);
		ret = tl_find_locked(so->tl, point, out);
		pw_os_mutex_unlock(so->tl->lock);
		return ret;
	default:
		return PW_EINVAL;
	}
}

/* syncobj_wait_syncobj_func() for every waiter: called whenever the fence changes. */
static void so_wake_locked(struct pw_syncobj *so)
{
	struct pw_sl *l, *next;

	for (l = so->waiters.next; l != &so->waiters; l = next) {
		struct pw_syncobj_wait *w = pw_container_of(l, struct pw_syncobj_wait, link);
		struct pw_fence *f;
		int ret;

		next = l->next;
		ret = so_find_locked(so, w->point, &f);
		if (ret == PW_EINVAL)
			continue;
		sl_del(&w->link);
		w->linked = false;
		w->fn(w, f, ret);
	}
}

/* drm_syncobj_fence_add_wait(): PW_OK with the fence if available now, else linked. */
static int so_add_wait_locked(struct pw_syncobj *so, struct pw_syncobj_wait *w,
			      struct pw_fence **out)
{
	int ret = so_find_locked(so, w->point, out);

	if (ret != PW_EINVAL)
		return ret;
	sl_add_tail(&so->waiters, &w->link);
	w->linked = true;
	return PW_EAGAIN;
}

static void so_remove_wait(struct pw_syncobj *so, struct pw_syncobj_wait *w)
{
	pw_os_mutex_lock(so->lock);
	if (w->linked) {
		sl_del(&w->link);
		w->linked = false;
	}
	pw_os_mutex_unlock(so->lock);
}

/* drm_syncobj_replace_fence(). Old state is returned for release after unlocking. */
static void so_replace_locked(struct pw_syncobj *so, struct pw_fence *fence,
			      struct pw_fence **old_plain, struct pw_tl **old_tl)
{
	*old_plain = so->plain;
	*old_tl = so->tl;
	so->plain = pw_fence_get(fence);
	so->tl = NULL;
	so->kind = fence ? PW_SO_PLAIN : PW_SO_NONE;
	if (fence != *old_plain || *old_tl)
		so_wake_locked(so);
}

void pw_syncobj_replace_fence(struct pw_syncobj *so, struct pw_fence *fence)
{
	struct pw_fence *old_plain;
	struct pw_tl *old_tl;

	pw_os_mutex_lock(so->lock);
	so_replace_locked(so, fence, &old_plain, &old_tl);
	pw_os_mutex_unlock(so->lock);
	pw_fence_put(old_plain);
	tl_put(old_tl);
}

struct pw_syncobj_point *pw_syncobj_point_alloc(struct pw_device *dev)
{
	struct pw_syncobj_point *pt = pw_os_zalloc(dev->os, sizeof(*pt));

	if (!pt)
		return NULL;
	pt->dev = dev;
	pt->node = pw_os_zalloc(dev->os, sizeof(*pt->node));
	pt->base = pw_os_zalloc(dev->os, sizeof(*pt->base));
	pt->tl = tl_alloc(dev);
	if (!pt->node || !pt->base || !pt->tl) {
		pw_syncobj_point_free(pt);
		return NULL;
	}
	return pt;
}

void pw_syncobj_point_free(struct pw_syncobj_point *pt)
{
	struct pw_os *os;

	if (!pt)
		return;
	os = pt->dev->os;
	pw_os_free(os, pt->node);
	pw_os_free(os, pt->base);
	tl_put(pt->tl);
	pw_os_free(os, pt);
}

/* drm_syncobj_add_point() + dma_fence_chain_init() (dma-fence-chain.c:240-288). Returns the
 * timeline with a temporary reference for tl_advance() after unlocking. */
static struct pw_tl *so_add_point_locked(struct pw_syncobj *so, struct pw_syncobj_point *pt,
					 struct pw_fence *fence, uint64_t point)
{
	struct pw_tl *tl;

	if (so->kind == PW_SO_TL) {
		struct pw_tl_node *head;

		tl = so->tl;
		pw_os_mutex_lock(tl->lock);
		head = tl->last;
		if (point > head->point) {
			/* In order: continue the head's context. */
			tl_append_locked(tl, pt->node, pw_fence_get(fence), point, head->point,
					 head->context);
		} else {
			/* drm_syncobj.c:346: "adding an unorder point to timeline" -> new context,
			 * seqno = max(prev, point), prev_seqno = 0; still waits for all older. */
			pw_os_log(so->os, PW_LOG_DBG,
				  "syncobj: unordered timeline point %llu (last %llu)",
				  (unsigned long long)point, (unsigned long long)head->point);
			tl_append_locked(tl, pt->node, pw_fence_get(fence), head->point, 0,
					 tl->next_context++);
		}
		pw_os_mutex_unlock(tl->lock);
	} else {
		tl = pt->tl;
		pt->tl = NULL;
		tl->dev = so->dev;
		pw_os_mutex_lock(tl->lock);
		if (so->kind == PW_SO_PLAIN) {
			/* The chain's prev is the binary fence: a context of its own. */
			tl_append_locked(tl, pt->base, so->plain, 0, 0, tl->next_context++);
			pt->base = NULL;
			so->plain = NULL;
		}
		tl_append_locked(tl, pt->node, pw_fence_get(fence), point, 0, tl->next_context++);
		pw_os_mutex_unlock(tl->lock);
		so->kind = PW_SO_TL;
		so->tl = tl;
	}
	pt->node = NULL;
	tl_get(tl);
	so_wake_locked(so);
	return tl;
}

void pw_syncobj_add_point(struct pw_syncobj *so, struct pw_syncobj_point *pt,
			  struct pw_fence *fence, uint64_t point)
{
	struct pw_tl *tl;

	pw_os_mutex_lock(so->lock);
	tl = so_add_point_locked(so, pt, fence, point);
	pw_os_mutex_unlock(so->lock);
	/* The payload may already be signalled (no callback was registered). */
	tl_advance(tl);
	tl_put(tl);
	pw_syncobj_point_free(pt);
}

/* ------------------------------------------------------------------------------------------ */
/* per-file state, notify registry                                                            */
/* ------------------------------------------------------------------------------------------ */

/* Refcounted separately from the file: a fence callback of a cancelled notify can still be
 * running when the file is closed. */
struct pw_notify_reg {
	uint32_t refcount;		/* __atomic */
	struct pw_os *os;
	struct pw_os_mutex *lock;
	struct pw_sl pending;		/* struct pw_notify, under @lock */
	uint64_t next_id;
};

struct pw_sync_file {
	struct pw_handle_table handles;	/* syncobj handles -> struct pw_syncobj * */
	struct pw_notify_reg *reg;
};

/*
 * A PANWIN_IOCTL_SYNCOBJ_EVENT registration. References: R (pending in the registry: dropped by
 * whoever unlists it, fire or cancel), W (linked as a syncobj waiter), C (fence callback added).
 */
struct pw_notify {
	struct pw_sl link;		/* in reg->pending while listed */
	bool listed;			/* under reg->lock */
	uint32_t refcount;		/* __atomic */
	uint64_t id;			/* the cookie */
	struct pw_notify_reg *reg;	/* reference */
	pw_notify_fn fn;
	void *ctx;
	bool available;			/* DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE */
	struct pw_syncobj *so;		/* reference */
	struct pw_syncobj_wait sw;
	struct pw_fence *fence;		/* under so->lock */
	struct pw_fence_cb cb;
	bool cb_added;			/* under so->lock */
};

static void reg_put(struct pw_notify_reg *reg)
{
	if (__atomic_sub_fetch(&reg->refcount, 1, __ATOMIC_ACQ_REL) != 0)
		return;
	pw_os_mutex_destroy(reg->os, reg->lock);
	pw_os_free(reg->os, reg);
}

static void notify_put(struct pw_notify *n)
{
	struct pw_notify_reg *reg = n->reg;

	if (__atomic_sub_fetch(&n->refcount, 1, __ATOMIC_ACQ_REL) != 0)
		return;
	pw_fence_put(n->fence);
	pw_syncobj_put(n->so);		/* never the last reference when called under so->lock */
	pw_os_free(reg->os, n);
	reg_put(reg);
}

/* Fire once: whoever unlists the notify owns the call (and reference R). */
static void notify_fire(struct pw_notify *n)
{
	bool fire;

	pw_os_mutex_lock(n->reg->lock);
	fire = n->listed;
	if (fire) {
		sl_del(&n->link);
		n->listed = false;
	}
	pw_os_mutex_unlock(n->reg->lock);
	if (!fire)
		return;
	n->fn(n->ctx);
	notify_put(n);
}

static void notify_fence_cb(struct pw_fence *f, struct pw_fence_cb *cb)
{
	struct pw_notify *n = pw_container_of(cb, struct pw_notify, cb);

	(void)f;
	notify_fire(n);
	notify_put(n);		/* C */
}

/* A fence is available (under so->lock). Returns true if the notify must fire now. */
static bool notify_arm_locked(struct pw_notify *n, struct pw_fence *f)
{
	n->fence = f;
	if (n->available || !f)
		return true;
	__atomic_fetch_add(&n->refcount, 1, __ATOMIC_RELAXED);
	n->cb_added = true;
	if (pw_fence_add_callback(f, &n->cb, notify_fence_cb)) {
		n->cb_added = false;
		__atomic_fetch_sub(&n->refcount, 1, __ATOMIC_RELAXED);
		return true;
	}
	return false;
}

static void notify_sw_fn(struct pw_syncobj_wait *w, struct pw_fence *f, int err)
{
	struct pw_notify *n = pw_container_of(w, struct pw_notify, sw);

	/* On PW_ENOMEM fire anyway: a spurious wake-up beats a hang (userspace re-checks). */
	if (err || notify_arm_locked(n, f))
		notify_fire(n);
	notify_put(n);		/* W */
}

/* Drop W and C after the notify was unlisted (cancel / file close). */
static void notify_disarm(struct pw_notify *n)
{
	bool drop_w, cb;
	struct pw_fence *f;

	pw_os_mutex_lock(n->so->lock);
	drop_w = n->sw.linked;
	if (drop_w) {
		sl_del(&n->sw.link);
		n->sw.linked = false;
	}
	cb = n->cb_added;
	f = n->fence;
	pw_os_mutex_unlock(n->so->lock);
	if (drop_w)
		notify_put(n);
	if (cb && pw_fence_remove_callback(f, &n->cb))
		notify_put(n);
	/* else: the callback ran or is running, it drops C itself. */
}

int pw_file_syncobj_notify(struct pw_file *file, uint32_t handle, uint64_t point, uint32_t flags,
			   pw_notify_fn fn, void *ctx, void **cookie)
{
	const uint32_t valid = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
			       DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
			       DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE |
			       DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE;
	struct pw_notify_reg *reg = file->sync->reg;
	struct pw_syncobj *so;
	struct pw_notify *n;
	struct pw_fence *f;
	bool fire = false;
	uint64_t id;
	int ret;

	*cookie = NULL;
	if (flags & ~valid)
		return PW_EINVAL;
	so = pw_syncobj_lookup(file, handle);
	if (!so)
		return PW_ENOENT;	/* drm_syncobj_eventfd_ioctl() */
	n = pw_os_zalloc(file->os, sizeof(*n));
	if (!n) {
		pw_syncobj_put(so);
		return PW_ENOMEM;
	}
	n->refcount = 1;	/* R */
	n->reg = reg;
	__atomic_fetch_add(&reg->refcount, 1, __ATOMIC_RELAXED);
	n->fn = fn;
	n->ctx = ctx;
	n->available = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;
	n->so = pw_syncobj_get(so);	/* ours stays until we are done with so->lock */
	n->sw.point = point;
	n->sw.fn = notify_sw_fn;
	sl_init(&n->link);
	sl_init(&n->sw.link);

	pw_os_mutex_lock(reg->lock);
	n->id = id = ++reg->next_id;
	sl_add_tail(&reg->pending, &n->link);
	n->listed = true;
	pw_os_mutex_unlock(reg->lock);

	/*
	 * Take W only when linking: a speculative W dropped after notify_arm_locked() could race
	 * with the fence callback dropping R and C on another thread (neither needs so->lock), and
	 * a plain decrement would then reach zero without freeing the notify.
	 */
	pw_os_mutex_lock(so->lock);
	ret = so_find_locked(so, point, &f);
	if (ret == PW_OK) {
		fire = notify_arm_locked(n, f);
	} else if (ret == PW_EINVAL && (flags & (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
						 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE))) {
		__atomic_fetch_add(&n->refcount, 1, __ATOMIC_RELAXED);	/* W */
		sl_add_tail(&so->waiters, &n->sw.link);
		n->sw.linked = true;
		ret = PW_OK;
	}
	pw_os_mutex_unlock(so->lock);

	if (ret) {
		/* No fence (EINVAL) or ENOMEM: nothing armed, take it back. */
		pw_os_mutex_lock(reg->lock);
		sl_del(&n->link);
		n->listed = false;
		pw_os_mutex_unlock(reg->lock);
		notify_put(n);
		pw_syncobj_put(so);
		return ret;
	}
	pw_syncobj_put(so);
	/* Once armed (fence callback or syncobj waiter) another thread may fire the notify and
	 * free it as soon as so->lock is dropped: only touch @n again if we own the firing. */
	*cookie = (void *)(uintptr_t)id;
	if (fire)
		notify_fire(n);
	return PW_OK;
}

bool pw_file_syncobj_notify_cancel(struct pw_file *file, void *cookie)
{
	struct pw_notify_reg *reg = file->sync->reg;
	uint64_t id = (uint64_t)(uintptr_t)cookie;
	struct pw_notify *n = NULL;
	struct pw_sl *l;

	if (!id)
		return false;
	pw_os_mutex_lock(reg->lock);
	for (l = reg->pending.next; l != &reg->pending; l = l->next) {
		struct pw_notify *it = pw_container_of(l, struct pw_notify, link);

		if (it->id == id) {
			n = it;
			sl_del(&n->link);
			n->listed = false;
			break;
		}
	}
	pw_os_mutex_unlock(reg->lock);
	if (!n)
		return false;	/* fired (and freed) already, or unknown cookie */
	notify_disarm(n);
	notify_put(n);		/* R */
	return true;
}

int pw_sync_file_init(struct pw_file *file)
{
	struct pw_sync_file *sf = pw_os_zalloc(file->os, sizeof(*sf));
	struct pw_notify_reg *reg;
	int ret;

	if (!sf)
		return PW_ENOMEM;
	ret = pw_handle_table_init(file->os, &sf->handles);
	if (ret)
		goto err_table;
	reg = pw_os_zalloc(file->os, sizeof(*reg));
	if (!reg) {
		ret = PW_ENOMEM;
		goto err_table;
	}
	reg->lock = pw_os_mutex_create(file->os);
	if (!reg->lock) {
		pw_os_free(file->os, reg);
		ret = PW_ENOMEM;
		goto err_table;
	}
	reg->os = file->os;
	reg->refcount = 1;
	sl_init(&reg->pending);
	sf->reg = reg;
	file->sync = sf;
	return PW_OK;

err_table:
	pw_handle_table_fini(file->os, &sf->handles, NULL);
	pw_os_free(file->os, sf);
	file->sync = NULL;
	return ret;
}

void pw_sync_file_fini(struct pw_file *file)
{
	struct pw_sync_file *sf = file->sync;
	struct pw_notify_reg *reg;
	struct pw_sl local;

	if (!sf)
		return;
	reg = sf->reg;
	/* Pending notifies the OS layer did not cancel: cancel them (fn is not called). */
	sl_init(&local);
	pw_os_mutex_lock(reg->lock);
	while (!sl_empty(&reg->pending)) {
		struct pw_notify *n = pw_container_of(reg->pending.next, struct pw_notify, link);

		sl_del(&n->link);
		n->listed = false;
		sl_add_tail(&local, &n->link);
	}
	pw_os_mutex_unlock(reg->lock);
	while (!sl_empty(&local)) {
		struct pw_notify *n = pw_container_of(local.next, struct pw_notify, link);

		sl_del(&n->link);
		notify_disarm(n);
		notify_put(n);
	}
	pw_handle_table_fini(file->os, &sf->handles, so_put_cb);
	reg_put(reg);
	pw_os_free(file->os, sf);
	file->sync = NULL;
}

struct pw_syncobj *pw_syncobj_lookup(struct pw_file *file, uint32_t handle)
{
	return pw_handle_get(&file->sync->handles, handle, so_get_cb);
}

/* drm_syncobj_array_find(): copy the handle array in and take a reference on each. */
static int so_array_find(struct pw_file *file, uint64_t user_handles, uint32_t count,
			 struct pw_syncobj ***out)
{
	struct pw_syncobj **sos;
	uint32_t *handles, i;
	int ret;

	if (count > PW_SYNC_MAX_HANDLES)
		return PW_EINVAL;
	handles = pw_os_zalloc(file->os, (size_t)count * sizeof(*handles));
	if (!handles)
		return PW_ENOMEM;
	ret = pw_os_copy_from_user(file->os, handles, user_handles,
				   (size_t)count * sizeof(*handles));
	if (ret) {
		ret = PW_EFAULT;
		goto out;
	}
	sos = pw_os_zalloc(file->os, (size_t)count * sizeof(*sos));
	if (!sos) {
		ret = PW_ENOMEM;
		goto out;
	}
	for (i = 0; i < count; i++) {
		sos[i] = pw_syncobj_lookup(file, handles[i]);
		if (!sos[i]) {
			while (i--)
				pw_syncobj_put(sos[i]);
			pw_os_free(file->os, sos);
			ret = PW_ENOENT;
			goto out;
		}
	}
	*out = sos;
out:
	pw_os_free(file->os, handles);
	return ret;
}

static void so_array_free(struct pw_file *file, struct pw_syncobj **sos, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++)
		pw_syncobj_put(sos[i]);
	pw_os_free(file->os, sos);
}

/* ------------------------------------------------------------------------------------------ */
/* kernel-internal find / attach                                                              */
/* ------------------------------------------------------------------------------------------ */

struct pw_find_wait {
	struct pw_syncobj_wait sw;
	struct pw_os_event *ev;
	struct pw_fence *fence;
	int err;
	uint32_t done;		/* __atomic */
};

static void find_sw_fn(struct pw_syncobj_wait *w, struct pw_fence *f, int err)
{
	struct pw_find_wait *fw = pw_container_of(w, struct pw_find_wait, sw);

	fw->fence = f;
	fw->err = err;
	__atomic_store_n(&fw->done, 1, __ATOMIC_RELEASE);
	pw_os_event_signal(fw->ev);
}

/* drm_syncobj_find_fence() (drm_syncobj.c:436-526). *out may be NULL (signalled). */
static int so_find_fence(struct pw_syncobj *so, uint64_t point, uint32_t flags,
			 uint64_t deadline_ns, struct pw_fence **out)
{
	struct pw_find_wait fw;
	int ret;

	*out = NULL;
	if (flags & ~(uint32_t)DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)
		return PW_EINVAL;
	pw_os_mutex_lock(so->lock);
	ret = so_find_locked(so, point, out);
	pw_os_mutex_unlock(so->lock);
	if (ret != PW_EINVAL || !(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT))
		return ret;

	pw_memset(&fw, 0, sizeof(fw));
	fw.sw.point = point;
	fw.sw.fn = find_sw_fn;
	fw.ev = pw_os_event_create(so->os);
	if (!fw.ev)
		return PW_ENOMEM;
	pw_os_mutex_lock(so->lock);
	ret = so_add_wait_locked(so, &fw.sw, &fw.fence);
	if (ret != PW_EAGAIN) {
		fw.err = ret;
		fw.done = 1;
	}
	pw_os_mutex_unlock(so->lock);

	ret = PW_OK;
	while (!__atomic_load_n(&fw.done, __ATOMIC_ACQUIRE)) {
		if (deadline_ns != UINT64_MAX && pw_os_time_ns(so->os) >= deadline_ns) {
			ret = PW_ETIME;
			break;
		}
		ret = pw_os_event_wait_user(fw.ev, deadline_ns);
		if (ret == PW_EINTR)
			break;
		ret = PW_OK;
	}
	so_remove_wait(so, &fw.sw);
	/* After so_remove_wait() find_sw_fn() cannot run any more: fw.* is stable. */
	if (fw.done) {
		ret = fw.err;
		*out = fw.fence;
	} else if (ret == PW_OK) {
		ret = PW_ETIME;
	}
	pw_os_event_destroy(so->os, fw.ev);
	return ret;
}

/* As so_find_fence(), but a collected point yields a real signalled fence (Linux: the stub). */
static int so_find_fence_ref(struct pw_syncobj *so, uint64_t point, uint32_t flags,
			     uint64_t deadline_ns, struct pw_fence **out)
{
	int ret = so_find_fence(so, point, flags, deadline_ns, out);

	if (ret == PW_OK && !*out) {
		*out = pw_fence_create_signalled(so->dev, 0);
		if (!*out)
			ret = PW_ENOMEM;
	}
	return ret;
}

/*
 * Contract (pw_sync.h): PW_EINVAL for a missing syncobj. Linux drm_syncobj_find_fence() returns
 * -ENOENT there (and panthor_drv.c:633 propagates it from GROUP_SUBMIT/VM_BIND waits); kept as
 * EINVAL because pw_bo.c and its tests depend on it (see docs/status/core-sync.md). Other errors:
 * PW_EINVAL for unknown @flags or no fence, PW_ETIME / PW_EINTR with WAIT_FOR_SUBMIT, PW_ENOMEM.
 * A point that is already signalled and collected yields a fresh signalled fence (Linux:
 * dma_fence_get_stub()).
 */
int pw_syncobj_find_fence(struct pw_file *file, uint32_t handle, uint64_t point, uint32_t flags,
			  uint64_t deadline_ns, struct pw_fence **out)
{
	struct pw_syncobj *so = pw_syncobj_lookup(file, handle);
	int ret;

	*out = NULL;
	if (!so)
		return PW_EINVAL;
	ret = so_find_fence_ref(so, point, flags, deadline_ns, out);
	pw_syncobj_put(so);
	return ret;
}

int pw_syncobj_attach_fence(struct pw_file *file, uint32_t handle, uint64_t point,
			    struct pw_fence *fence)
{
	struct pw_syncobj *so = pw_syncobj_lookup(file, handle);
	struct pw_syncobj_point *pt;

	if (!so)
		return PW_EINVAL;	/* consistent with pw_syncobj_find_fence() */
	if (!point) {
		pw_syncobj_replace_fence(so, fence);
	} else {
		pt = pw_syncobj_point_alloc(file->dev);
		if (!pt) {
			pw_syncobj_put(so);
			return PW_ENOMEM;
		}
		pw_syncobj_add_point(so, pt, fence, point);
	}
	pw_syncobj_put(so);
	return PW_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* ioctls                                                                                     */
/* ------------------------------------------------------------------------------------------ */

int pw_ioctl_syncobj_create(struct pw_file *file, void *arg)
{
	struct drm_syncobj_create *args = arg;
	struct pw_syncobj *so;
	int ret;

	if (args->flags & ~(uint32_t)DRM_SYNCOBJ_CREATE_SIGNALED)
		return PW_EINVAL;
	so = so_create(file->dev);
	if (!so)
		return PW_ENOMEM;
	if (args->flags & DRM_SYNCOBJ_CREATE_SIGNALED) {
		struct pw_fence *f = pw_fence_create_signalled(file->dev, 0);

		if (!f) {
			pw_syncobj_put(so);
			return PW_ENOMEM;
		}
		so->kind = PW_SO_PLAIN;
		so->plain = f;
	}
	ret = pw_handle_alloc(file->os, &file->sync->handles, so, &args->handle);
	if (ret)
		pw_syncobj_put(so);
	return ret;
}

int pw_ioctl_syncobj_destroy(struct pw_file *file, void *arg)
{
	struct drm_syncobj_destroy *args = arg;
	struct pw_syncobj *so;

	if (args->pad)
		return PW_EINVAL;
	so = pw_handle_remove(&file->sync->handles, args->handle);
	if (!so)
		return PW_EINVAL;	/* drm_syncobj_destroy() */
	/* Waiters hold their own references and keep waiting (until their fence or deadline). */
	pw_syncobj_put(so);
	return PW_OK;
}

/* ---- WAIT / TIMELINE_WAIT (drm_syncobj_array_wait_timeout(), drm_syncobj.c:1035-1201) ---- */

struct pw_waiter;

struct pw_wait_entry {
	struct pw_syncobj_wait sw;
	struct pw_fence_cb cb;
	struct pw_waiter *w;
	struct pw_fence *fence;		/* valid once @have; NULL = already signalled */
	uint32_t have;			/* __atomic */
	bool cb_added;
};

/* Heap object shared with fence callbacks (which hold a reference each). */
struct pw_waiter {
	uint32_t refcount;		/* __atomic */
	struct pw_os *os;
	struct pw_os_event *ev;
	int err;			/* __atomic: PW_ENOMEM from a wake-up */
	struct pw_wait_entry e[];
};

static void waiter_put(struct pw_waiter *w)
{
	if (__atomic_sub_fetch(&w->refcount, 1, __ATOMIC_ACQ_REL) != 0)
		return;
	pw_os_event_destroy(w->os, w->ev);
	pw_os_free(w->os, w);
}

static void wait_fence_cb(struct pw_fence *f, struct pw_fence_cb *cb)
{
	struct pw_wait_entry *e = pw_container_of(cb, struct pw_wait_entry, cb);
	struct pw_waiter *w = e->w;

	(void)f;
	pw_os_event_signal(w->ev);
	waiter_put(w);
}

static void wait_sw_fn(struct pw_syncobj_wait *sw, struct pw_fence *f, int err)
{
	struct pw_wait_entry *e = pw_container_of(sw, struct pw_wait_entry, sw);

	if (err) {
		__atomic_store_n(&e->w->err, err, __ATOMIC_RELEASE);
	} else {
		e->fence = f;
		__atomic_store_n(&e->have, 1, __ATOMIC_RELEASE);
	}
	pw_os_event_signal(e->w->ev);
}

/* True if entry @e counts as signalled; arms its fence callback otherwise. */
static bool wait_entry_check(struct pw_wait_entry *e, bool available)
{
	if (available || !e->fence || pw_fence_is_signalled(e->fence))
		return true;
	if (e->cb_added)
		return false;
	__atomic_fetch_add(&e->w->refcount, 1, __ATOMIC_RELAXED);
	e->cb_added = true;
	if (pw_fence_add_callback(e->fence, &e->cb, wait_fence_cb)) {
		e->cb_added = false;
		__atomic_fetch_sub(&e->w->refcount, 1, __ATOMIC_RELAXED);
		return true;
	}
	return false;
}

static int so_array_wait(struct pw_file *file, struct pw_syncobj **sos, uint64_t user_points,
			 uint32_t count, uint32_t flags, int64_t timeout_nsec, uint32_t *first)
{
	const bool all = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	const bool available = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;
	const bool submit = flags & (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
				     DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE);
	uint32_t i, idx = ~0u, signalled;
	bool poll, timed_out = false;
	uint64_t *points, deadline;
	struct pw_waiter *w;
	int ret;

	points = pw_os_zalloc(file->os, (size_t)count * sizeof(*points));
	if (!points)
		return PW_ENOMEM;
	if (user_points && pw_os_copy_from_user(file->os, points, user_points,
						(size_t)count * sizeof(*points))) {
		pw_os_free(file->os, points);
		return PW_EFAULT;
	}
	w = pw_os_zalloc(file->os, sizeof(*w) + (size_t)count * sizeof(w->e[0]));
	if (!w) {
		pw_os_free(file->os, points);
		return PW_ENOMEM;
	}
	w->os = file->os;
	w->refcount = 1;
	w->ev = pw_os_event_create(file->os);
	if (!w->ev) {
		pw_os_free(file->os, w);
		pw_os_free(file->os, points);
		return PW_ENOMEM;
	}
	for (i = 0; i < count; i++) {
		w->e[i].w = w;
		w->e[i].sw.point = points[i];
		w->e[i].sw.fn = wait_sw_fn;
		sl_init(&w->e[i].sw.link);
	}

	/* drm_timeout_abs_to_jiffies(): 0, negative or past means poll. Sample "now" before the
	 * pre-pass so a deadline in the future always gets at least one blocking wait. */
	{
		uint64_t now = pw_os_time_ns(file->os);

		poll = timeout_nsec <= 0 || (uint64_t)timeout_nsec <= now;
		deadline = timeout_nsec == INT64_MAX ? UINT64_MAX : (uint64_t)timeout_nsec;
	}

	/* Pre-pass: -EINVAL for a missing fence without WAIT_FOR_SUBMIT/AVAILABLE is only possible
	 * here, before anything waited. */
	signalled = 0;
	for (i = 0; i < count; i++) {
		struct pw_wait_entry *e = &w->e[i];

		pw_os_mutex_lock(sos[i]->lock);
		ret = so_find_locked(sos[i], points[i], &e->fence);
		pw_os_mutex_unlock(sos[i]->lock);
		if (ret == PW_EINVAL && submit)
			continue;
		if (ret)
			goto cleanup;
		e->have = 1;
		if (available || !e->fence || pw_fence_is_signalled(e->fence)) {
			if (!signalled)
				idx = i;
			signalled++;
		}
	}
	ret = PW_OK;
	if (signalled == count || (signalled && !all))
		goto cleanup;

	if (submit) {
		for (i = 0; i < count; i++) {
			struct pw_wait_entry *e = &w->e[i];

			if (e->have)
				continue;
			pw_os_mutex_lock(sos[i]->lock);
			ret = so_add_wait_locked(sos[i], &e->sw, &e->fence);
			if (ret == PW_OK)
				e->have = 1;
			pw_os_mutex_unlock(sos[i]->lock);
			if (ret != PW_OK && ret != PW_EAGAIN)
				goto cleanup;
		}
	}

	for (;;) {
		bool hit = false;

		/* Manual-reset event: reset before checking, so a signal after the check is kept. */
		pw_os_event_reset(w->ev);
		ret = __atomic_load_n(&w->err, __ATOMIC_ACQUIRE);
		if (ret)
			break;
		signalled = 0;
		for (i = 0; i < count; i++) {
			struct pw_wait_entry *e = &w->e[i];

			if (!__atomic_load_n(&e->have, __ATOMIC_ACQUIRE))
				continue;
			if (wait_entry_check(e, available)) {
				if (all) {
					signalled++;
				} else {
					idx = i;
					hit = true;
					break;
				}
			}
		}
		if (hit || signalled == count) {
			ret = PW_OK;
			break;
		}
		if (poll || timed_out) {
			ret = PW_ETIME;
			break;
		}
		ret = pw_os_event_wait_user(w->ev, deadline);
		if (ret == PW_EINTR)
			break;	/* Linux: -ERESTARTSYS */
		if (ret == PW_ETIMEDOUT)
			timed_out = true;	/* one last check, then -ETIME */
	}

cleanup:
	for (i = 0; i < count; i++) {
		struct pw_wait_entry *e = &w->e[i];

		if (submit)
			so_remove_wait(sos[i], &e->sw);
		/* No wake-up can run any more: e->have/e->fence are stable. */
		if (e->cb_added && pw_fence_remove_callback(e->fence, &e->cb))
			waiter_put(w);
		if (e->have)
			pw_fence_put(e->fence);
	}
	pw_os_free(file->os, points);
	waiter_put(w);
	if (ret == PW_OK)
		*first = idx;
	return ret;
}

int pw_ioctl_syncobj_wait(struct pw_file *file, void *arg)
{
	struct drm_syncobj_wait *args = arg;
	const uint32_t possible = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
				  DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
				  DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE;
	struct pw_syncobj **sos;
	uint32_t first;
	int ret;

	if (args->flags & ~possible)
		return PW_EINVAL;
	if (!args->count_handles)
		return PW_OK;
	ret = so_array_find(file, args->handles, args->count_handles, &sos);
	if (ret)
		return ret;
	/* WAIT_DEADLINE (deadline_nsec) is a hint only; ignored. */
	ret = so_array_wait(file, sos, 0, args->count_handles, args->flags, args->timeout_nsec,
			    &first);
	if (ret == PW_OK)
		args->first_signaled = first;
	so_array_free(file, sos, args->count_handles);
	return ret;
}

int pw_ioctl_syncobj_timeline_wait(struct pw_file *file, void *arg)
{
	struct drm_syncobj_timeline_wait *args = arg;
	const uint32_t possible = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
				  DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
				  DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE |
				  DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE;
	struct pw_syncobj **sos;
	uint32_t first;
	int ret;

	if (args->flags & ~possible)
		return PW_EINVAL;
	if (!args->count_handles)
		return PW_OK;
	ret = so_array_find(file, args->handles, args->count_handles, &sos);
	if (ret)
		return ret;
	ret = so_array_wait(file, sos, args->points, args->count_handles, args->flags,
			    args->timeout_nsec, &first);
	if (ret == PW_OK)
		args->first_signaled = first;
	so_array_free(file, sos, args->count_handles);
	return ret;
}

int pw_ioctl_syncobj_reset(struct pw_file *file, void *arg)
{
	struct drm_syncobj_array *args = arg;
	struct pw_syncobj **sos;
	int ret;

	if (args->pad || !args->count_handles)
		return PW_EINVAL;
	ret = so_array_find(file, args->handles, args->count_handles, &sos);
	if (ret)
		return ret;
	for (uint32_t i = 0; i < args->count_handles; i++)
		pw_syncobj_replace_fence(sos[i], NULL);
	so_array_free(file, sos, args->count_handles);
	return PW_OK;
}

int pw_ioctl_syncobj_signal(struct pw_file *file, void *arg)
{
	struct drm_syncobj_array *args = arg;
	struct pw_syncobj **sos;
	int ret;

	if (args->pad || !args->count_handles)
		return PW_EINVAL;
	ret = so_array_find(file, args->handles, args->count_handles, &sos);
	if (ret)
		return ret;
	/* drm_syncobj_assign_null_handle() per syncobj; stops at the first failure like Linux. */
	for (uint32_t i = 0; i < args->count_handles; i++) {
		struct pw_fence *f = pw_fence_create_signalled(file->dev, 0);

		if (!f) {
			ret = PW_ENOMEM;
			break;
		}
		pw_syncobj_replace_fence(sos[i], f);
		pw_fence_put(f);
	}
	so_array_free(file, sos, args->count_handles);
	return ret;
}

int pw_ioctl_syncobj_timeline_signal(struct pw_file *file, void *arg)
{
	struct drm_syncobj_timeline_array *args = arg;
	struct pw_syncobj_point **pts = NULL;
	struct pw_fence **stubs = NULL;
	struct pw_syncobj **sos;
	uint64_t *points = NULL;
	uint32_t i, count = args->count_handles;
	int ret;

	if (args->flags || !count)
		return PW_EINVAL;
	ret = so_array_find(file, args->handles, count, &sos);
	if (ret)
		return ret;
	points = pw_os_zalloc(file->os, (size_t)count * sizeof(*points));
	pts = pw_os_zalloc(file->os, (size_t)count * sizeof(*pts));
	stubs = pw_os_zalloc(file->os, (size_t)count * sizeof(*stubs));
	if (!points || !pts || !stubs) {
		ret = PW_ENOMEM;
		goto out;
	}
	/* NULL points array: all zero (drm_syncobj.c:1612). */
	if (args->points && pw_os_copy_from_user(file->os, points, args->points,
						 (size_t)count * sizeof(*points))) {
		ret = PW_EFAULT;
		goto out;
	}
	/* Allocate everything first: nothing is changed unless all of it succeeds. */
	for (i = 0; i < count; i++) {
		pts[i] = pw_syncobj_point_alloc(file->dev);
		stubs[i] = pw_fence_create_signalled(file->dev, 0);
		if (!pts[i] || !stubs[i]) {
			ret = PW_ENOMEM;
			goto out;
		}
	}
	for (i = 0; i < count; i++) {
		pw_syncobj_add_point(sos[i], pts[i], stubs[i], points[i]);
		pts[i] = NULL;
	}
out:
	for (i = 0; i < count && pts && stubs; i++) {
		pw_syncobj_point_free(pts[i]);
		pw_fence_put(stubs[i]);
	}
	pw_os_free(file->os, stubs);
	pw_os_free(file->os, pts);
	pw_os_free(file->os, points);
	so_array_free(file, sos, count);
	return ret;
}

int pw_ioctl_syncobj_query(struct pw_file *file, void *arg)
{
	struct drm_syncobj_timeline_array *args = arg;
	struct pw_syncobj **sos;
	int ret;

	if (args->flags & ~(uint32_t)DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED)
		return PW_EINVAL;
	if (!args->count_handles)
		return PW_EINVAL;
	ret = so_array_find(file, args->handles, args->count_handles, &sos);
	if (ret)
		return ret;
	for (uint32_t i = 0; i < args->count_handles; i++) {
		struct pw_syncobj *so = sos[i];
		uint64_t point = 0;

		pw_os_mutex_lock(so->lock);
		if (so->kind == PW_SO_TL) {
			pw_os_mutex_lock(so->tl->lock);
			point = tl_query_locked(so->tl, args->flags);
			pw_os_mutex_unlock(so->tl->lock);
		}
		pw_os_mutex_unlock(so->lock);
		if (pw_os_copy_to_user(file->os, args->points + (uint64_t)i * sizeof(point), &point,
				       sizeof(point))) {
			ret = PW_EFAULT;
			break;
		}
	}
	so_array_free(file, sos, args->count_handles);
	return ret;
}

/* drm_syncobj_transfer_ioctl() + _to_timeline/_to_binary (drm_syncobj.c:921-1002). */
int pw_ioctl_syncobj_transfer(struct pw_file *file, void *arg)
{
	struct drm_syncobj_transfer *args = arg;
	struct pw_syncobj *dst, *src;
	struct pw_syncobj_point *pt;
	struct pw_fence *f;
	int ret;

	if (args->pad)
		return PW_EINVAL;
	dst = pw_syncobj_lookup(file, args->dst_handle);
	if (!dst)
		return PW_ENOENT;
	src = pw_syncobj_lookup(file, args->src_handle);
	if (!src) {
		pw_syncobj_put(dst);
		return PW_ENOENT;
	}
	/* Linux uses dma_fence_unwrap_merge() of the found fence for the timeline case: a fence
	 * that signals when all of its components have, i.e. our chain fence / payload. */
	ret = so_find_fence_ref(src, args->src_point, args->flags,
				pw_os_time_ns(file->os) + PW_SYNC_TRANSFER_SUBMIT_TIMEOUT_NS, &f);
	pw_syncobj_put(src);
	if (ret)
		goto out;
	if (args->dst_point) {
		pt = pw_syncobj_point_alloc(file->dev);
		if (!pt)
			ret = PW_ENOMEM;
		else
			pw_syncobj_add_point(dst, pt, f, args->dst_point);
	} else {
		pw_syncobj_replace_fence(dst, f);
	}
	pw_fence_put(f);
out:
	pw_syncobj_put(dst);
	return ret;
}
