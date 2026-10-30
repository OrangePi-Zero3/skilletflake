// SPDX-License-Identifier: MIT
/*
 * fake_wdk.c - pthread implementation of the NT/KMDF subset declared in wdk_shim/, so the real
 * kmd/win glue (pw_os_win.c, pw_win_ioctl.c, pw_win_user.c) can run on the Linux host against
 * the real kmd/core. Written from the public API documentation; it models the behaviour the
 * glue depends on (dispatcher waits, alertable UserMode waits, rundown protection, timers +
 * DPCs, IO_WORKITEMs, MDL mappings per process) and counts misuse instead of bugchecking.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include "pw_win.h"
#include "fake_wdk.h"

struct pw_win_routines pw_win_rt;	/* nothing resolved: the glue uses its fallbacks */
__thread jmp_buf fake_seh_jb;

long fake_user_maps_live, fake_unmap_wrong_process, fake_attach_count, fake_work_items_live;
static long fake_misuse;

#define MISUSE(...)                                                       \
	do {                                                              \
		__atomic_fetch_add(&fake_misuse, 1, __ATOMIC_RELAXED);    \
		fprintf(stderr, "fake_wdk: MISUSE: " __VA_ARGS__);        \
		fprintf(stderr, "\n");                                    \
	} while (0)

long fake_wdk_misuse_count(void);
long fake_wdk_misuse_count(void)
{
	return __atomic_load_n(&fake_misuse, __ATOMIC_RELAXED);
}

uint64_t fake_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void ns_to_ts(uint64_t ns, struct timespec *ts)
{
	ts->tv_sec = (time_t)(ns / 1000000000ull);
	ts->tv_nsec = (long)(ns % 1000000000ull);
}

static void cond_init_mono(pthread_cond_t *c)
{
	pthread_condattr_t a;

	pthread_condattr_init(&a);
	pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
	pthread_cond_init(c, &a);
	pthread_condattr_destroy(&a);
}

/* --------------------------------------------------------------------------- logging */

ULONG DbgPrintEx(ULONG component, ULONG level, const char *fmt, ...)
{
	va_list ap;

	(void)component;
	if (!getenv("PW_TEST_VERBOSE") && level > DPFLTR_WARNING_LEVEL)
		return 0;
	if (!getenv("PW_TEST_VERBOSE"))
		return 0;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	return 0;
}

/* ------------------------------------------------------------------- dispatcher objects */

static pthread_mutex_t disp_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t disp_cond;
static pthread_once_t disp_once = PTHREAD_ONCE_INIT;

static void disp_init(void)
{
	cond_init_mono(&disp_cond);
}

/* KeEnterCriticalRegion depth: normal kernel APCs (thread termination's first stage) are not
 * delivered while it is > 0. */
static __thread int crit_region;

#define MAX_APC_THREADS 64
static struct {
	HANDLE tid;
	int pending;
} apcs[MAX_APC_THREADS];	/* under disp_lock */

void fake_queue_user_apc(HANDLE tid)
{
	pthread_once(&disp_once, disp_init);
	pthread_mutex_lock(&disp_lock);
	for (int i = 0; i < MAX_APC_THREADS; i++) {
		if (apcs[i].tid == tid || !apcs[i].tid) {
			apcs[i].tid = tid;
			apcs[i].pending = 1;
			break;
		}
	}
	pthread_cond_broadcast(&disp_cond);
	pthread_mutex_unlock(&disp_lock);
}

static int apc_take_locked(HANDLE tid)
{
	for (int i = 0; i < MAX_APC_THREADS; i++) {
		if (apcs[i].tid == tid && apcs[i].pending) {
			apcs[i].pending = 0;
			return 1;
		}
	}
	return 0;
}

void KeInitializeEvent(PRKEVENT e, EVENT_TYPE type, BOOLEAN state)
{
	pthread_once(&disp_once, disp_init);
	e->type = type;
	e->signalled = state ? 1 : 0;
}

LONG KeSetEvent(PRKEVENT e, LONG incr, BOOLEAN wait)
{
	LONG prev;

	(void)incr;
	(void)wait;
	pthread_mutex_lock(&disp_lock);
	prev = e->signalled;
	e->signalled = 1;
	pthread_cond_broadcast(&disp_cond);
	pthread_mutex_unlock(&disp_lock);
	return prev;
}

void KeClearEvent(PRKEVENT e)
{
	pthread_mutex_lock(&disp_lock);
	e->signalled = 0;
	pthread_mutex_unlock(&disp_lock);
}

BOOLEAN fake_event_is_set(PKEVENT e)
{
	BOOLEAN s;

	pthread_mutex_lock(&disp_lock);
	s = e->signalled != 0;
	pthread_mutex_unlock(&disp_lock);
	return s;
}

NTSTATUS KeWaitForMultipleObjects(ULONG count, PVOID objs[], WAIT_TYPE type, KWAIT_REASON reason,
				  KPROCESSOR_MODE mode, BOOLEAN alertable, PLARGE_INTEGER timeout,
				  PVOID wait_blocks)
{
	HANDLE tid = PsGetCurrentThreadId();
	uint64_t deadline = 0;
	NTSTATUS st;

	(void)reason;
	if (type != WaitAny)
		MISUSE("only WaitAny is modelled");
	/* Termination reaches an alertable UserMode wait as a normal kernel APC that re-queues
	 * itself as a user APC; inside a critical region (any pw_os_mutex / FAST_MUTEX held) the
	 * first stage never runs and the thread cannot be killed while it waits. */
	if (mode == UserMode && alertable && crit_region > 0)
		MISUSE("alertable UserMode wait inside a critical region (a lock is held)");
	if (count > THREAD_WAIT_OBJECTS && !wait_blocks)
		MISUSE("KeWaitForMultipleObjects: %u objects without a wait block array", count);
	if (timeout) {
		if (timeout->QuadPart > 0)
			MISUSE("absolute KeWait timeouts are not modelled");
		deadline = fake_now_ns() + (uint64_t)(-timeout->QuadPart) * 100u;
	}
	pthread_once(&disp_once, disp_init);
	pthread_mutex_lock(&disp_lock);
	for (;;) {
		for (ULONG i = 0; i < count; i++) {
			PKEVENT e = objs[i];

			if (e->signalled) {
				if (e->type == SynchronizationEvent)
					e->signalled = 0;
				st = STATUS_WAIT_0 + (NTSTATUS)i;
				goto out;
			}
		}
		if (mode == UserMode && alertable && apc_take_locked(tid)) {
			st = STATUS_USER_APC;
			goto out;
		}
		if (timeout) {
			struct timespec ts;

			if (fake_now_ns() >= deadline) {
				st = STATUS_TIMEOUT;
				goto out;
			}
			ns_to_ts(deadline, &ts);
			pthread_cond_timedwait(&disp_cond, &disp_lock, &ts);
		} else {
			pthread_cond_wait(&disp_cond, &disp_lock);
		}
	}
out:
	pthread_mutex_unlock(&disp_lock);
	return st;
}

NTSTATUS KeWaitForSingleObject(PVOID obj, KWAIT_REASON reason, KPROCESSOR_MODE mode,
			       BOOLEAN alertable, PLARGE_INTEGER timeout)
{
	PVOID objs[1] = { obj };

	return KeWaitForMultipleObjects(1, objs, WaitAny, reason, mode, alertable, timeout, NULL);
}

NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE mode, BOOLEAN alertable, PLARGE_INTEGER interval)
{
	struct timespec ts;
	uint64_t ns = interval->QuadPart < 0 ? (uint64_t)(-interval->QuadPart) * 100u : 0;

	(void)mode;
	(void)alertable;
	ns_to_ts(ns, &ts);
	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
	return STATUS_SUCCESS;
}

void KeStallExecutionProcessor(ULONG us)
{
	usleep(us);
}

LARGE_INTEGER KeQueryPerformanceCounter(PLARGE_INTEGER freq)
{
	LARGE_INTEGER now;

	if (freq)
		freq->QuadPart = 1000000000ll;
	now.QuadPart = (LONGLONG)fake_now_ns();
	return now;
}

/* --------------------------------------------------------------- locks and rundown */

void KeInitializeSpinLock(PKSPIN_LOCK l)
{
	pthread_mutex_init(l, NULL);
}

void KeAcquireSpinLock(PKSPIN_LOCK l, KIRQL *old)
{
	pthread_mutex_lock(l);
	*old = PASSIVE_LEVEL;
}

void KeReleaseSpinLock(PKSPIN_LOCK l, KIRQL old)
{
	(void)old;
	pthread_mutex_unlock(l);
}

void KeAcquireSpinLockAtDpcLevel(PKSPIN_LOCK l)
{
	pthread_mutex_lock(l);
}

void KeReleaseSpinLockFromDpcLevel(PKSPIN_LOCK l)
{
	pthread_mutex_unlock(l);
}

void ExInitializeFastMutex(PFAST_MUTEX m)
{
	pthread_mutexattr_t a;

	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK); /* FAST_MUTEX is not recursive */
	pthread_mutex_init(&m->m, &a);
	pthread_mutexattr_destroy(&a);
}

void ExAcquireFastMutexUnsafe(PFAST_MUTEX m)
{
	if (crit_region <= 0)
		MISUSE("ExAcquireFastMutexUnsafe outside a critical region");
	if (pthread_mutex_lock(&m->m))
		MISUSE("recursive FAST_MUTEX acquire");
}

void ExReleaseFastMutexUnsafe(PFAST_MUTEX m)
{
	if (pthread_mutex_unlock(&m->m))
		MISUSE("FAST_MUTEX released by a non-owner");
}

void KeEnterCriticalRegion(void)
{
	crit_region++;
}

void KeLeaveCriticalRegion(void)
{
	if (--crit_region < 0)
		MISUSE("unbalanced KeLeaveCriticalRegion");
}

void ExInitializeRundownProtection(PEX_RUNDOWN_REF r)
{
	pthread_mutex_init(&r->m, NULL);
	pthread_cond_init(&r->c, NULL);
	r->count = 0;
	r->active = 0;
}

void ExReInitializeRundownProtection(PEX_RUNDOWN_REF r)
{
	pthread_mutex_lock(&r->m);
	r->active = 0;
	pthread_mutex_unlock(&r->m);
}

BOOLEAN ExAcquireRundownProtection(PEX_RUNDOWN_REF r)
{
	BOOLEAN ok;

	pthread_mutex_lock(&r->m);
	ok = !r->active;
	if (ok)
		r->count++;
	pthread_mutex_unlock(&r->m);
	return ok;
}

void ExReleaseRundownProtection(PEX_RUNDOWN_REF r)
{
	pthread_mutex_lock(&r->m);
	if (--r->count < 0)
		MISUSE("rundown released more often than acquired");
	if (!r->count)
		pthread_cond_broadcast(&r->c);
	pthread_mutex_unlock(&r->m);
}

void ExWaitForRundownProtectionRelease(PEX_RUNDOWN_REF r)
{
	pthread_mutex_lock(&r->m);
	r->active = 1;
	while (r->count)
		pthread_cond_wait(&r->c, &r->m);
	pthread_mutex_unlock(&r->m);
}

/* ------------------------------------------------------------------- timers + DPCs */

static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t timer_cond;
static pthread_once_t timer_once = PTHREAD_ONCE_INIT;
static PKTIMER timers;		/* armed timers */
static int dpcs_running;

static void timer_unlink_locked(PKTIMER t)
{
	for (PKTIMER *pp = &timers; *pp; pp = &(*pp)->next) {
		if (*pp == t) {
			*pp = t->next;
			t->next = NULL;
			return;
		}
	}
}

static void *timer_thread(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&timer_lock);
	for (;;) {
		uint64_t now = fake_now_ns();
		PKTIMER due = NULL;
		struct timespec ts;

		for (PKTIMER t = timers; t; t = t->next) {
			if (t->due_ns <= now) {
				due = t;
				break;
			}
		}
		if (due) {
			PKDPC dpc = due->dpc;

			timer_unlink_locked(due);
			due->armed = 0;
			if (dpc) {
				dpcs_running++;
				pthread_mutex_unlock(&timer_lock);
				dpc->routine(dpc, dpc->ctx, NULL, NULL);
				pthread_mutex_lock(&timer_lock);
				dpcs_running--;
				pthread_cond_broadcast(&timer_cond);
			}
			continue;
		}
		ns_to_ts(now + 1000000u, &ts);	/* 1 ms "clock tick" */
		pthread_cond_timedwait(&timer_cond, &timer_lock, &ts);
	}
	return NULL;
}

static void timer_start(void)
{
	pthread_t thr;

	cond_init_mono(&timer_cond);
	pthread_create(&thr, NULL, timer_thread, NULL);
	pthread_detach(thr);
}

void KeInitializeTimerEx(PKTIMER t, TIMER_TYPE type)
{
	(void)type;
	pthread_once(&timer_once, timer_start);
	memset(t, 0, sizeof(*t));
}

BOOLEAN KeSetTimer(PKTIMER t, LARGE_INTEGER due, PKDPC dpc)
{
	BOOLEAN was;

	if (due.QuadPart > 0)
		MISUSE("absolute KeSetTimer not modelled");
	pthread_mutex_lock(&timer_lock);
	was = t->armed;
	if (!was) {
		t->next = timers;
		timers = t;
	}
	t->armed = 1;
	t->due_ns = fake_now_ns() + (uint64_t)(-due.QuadPart) * 100u;
	t->dpc = dpc;
	pthread_cond_broadcast(&timer_cond);
	pthread_mutex_unlock(&timer_lock);
	return was;
}

BOOLEAN KeCancelTimer(PKTIMER t)
{
	BOOLEAN was;

	pthread_mutex_lock(&timer_lock);
	was = t->armed;
	if (was)
		timer_unlink_locked(t);
	t->armed = 0;
	pthread_mutex_unlock(&timer_lock);
	return was;
}

void KeInitializeDpc(PRKDPC dpc, PKDEFERRED_ROUTINE routine, PVOID ctx)
{
	dpc->routine = routine;
	dpc->ctx = ctx;
}

void KeFlushQueuedDpcs(void)
{
	pthread_mutex_lock(&timer_lock);
	while (dpcs_running)
		pthread_cond_wait(&timer_cond, &timer_lock);
	pthread_mutex_unlock(&timer_lock);
}

/* --------------------------------------------------------------------- work items */

struct fake_device_object {
	int dummy;
};
static struct fake_device_object fake_devobj;

struct _IO_WORKITEM {
	PDEVICE_OBJECT dev;
	int queued;	/* __atomic */
};

struct work_run {
	PIO_WORKITEM item;
	PIO_WORKITEM_ROUTINE routine;
	PVOID ctx;
};

static pthread_mutex_t work_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cond = PTHREAD_COND_INITIALIZER;

static void *work_thread(void *arg)
{
	struct work_run r = *(struct work_run *)arg;
	PDEVICE_OBJECT dev = r.item->dev;

	free(arg);
	/* The item may be re-queued (or freed after a flush) from inside its routine. */
	__atomic_store_n(&r.item->queued, 0, __ATOMIC_RELEASE);
	r.routine(dev, r.ctx);
	pthread_mutex_lock(&work_lock);
	fake_work_items_live--;
	pthread_cond_broadcast(&work_cond);
	pthread_mutex_unlock(&work_lock);
	return NULL;
}

PIO_WORKITEM IoAllocateWorkItem(PDEVICE_OBJECT dev)
{
	PIO_WORKITEM w = calloc(1, sizeof(*w));

	if (w)
		w->dev = dev;
	return w;
}

void IoQueueWorkItem(PIO_WORKITEM item, PIO_WORKITEM_ROUTINE routine, WORK_QUEUE_TYPE q, PVOID ctx)
{
	struct work_run *r = malloc(sizeof(*r));
	pthread_t thr;

	(void)q;
	if (__atomic_exchange_n(&item->queued, 1, __ATOMIC_ACQ_REL))
		MISUSE("IO_WORKITEM queued twice");
	r->item = item;
	r->routine = routine;
	r->ctx = ctx;
	pthread_mutex_lock(&work_lock);
	fake_work_items_live++;
	pthread_mutex_unlock(&work_lock);
	pthread_create(&thr, NULL, work_thread, r);
	pthread_detach(thr);
}

void IoFreeWorkItem(PIO_WORKITEM item)
{
	if (__atomic_load_n(&item->queued, __ATOMIC_ACQUIRE))
		MISUSE("IoFreeWorkItem on a queued item");
	free(item);
}

void fake_wait_work_items_idle(void)
{
	pthread_mutex_lock(&work_lock);
	while (fake_work_items_live)
		pthread_cond_wait(&work_cond, &work_lock);
	pthread_mutex_unlock(&work_lock);
}

/* ------------------------------------------------------------- processes and objects */

struct fake_process fake_proc_a = { { FAKE_OB_PROCESS, 1 }, "A" };
struct fake_process fake_proc_b = { { FAKE_OB_PROCESS, 1 }, "B" };
static __thread PEPROCESS cur_proc;
static struct fake_object_type { int dummy; } event_type;
static POBJECT_TYPE event_type_ptr = &event_type;
POBJECT_TYPE *ExEventObjectType = &event_type_ptr;

void fake_set_current_process(PEPROCESS p)
{
	cur_proc = p;
}

PEPROCESS IoGetCurrentProcess(void)
{
	return cur_proc ? cur_proc : &fake_proc_a;
}

HANDLE PsGetCurrentThreadId(void)
{
	return (HANDLE)(uintptr_t)gettid();
}

KPROCESSOR_MODE ExGetPreviousMode(void)
{
	return UserMode;
}

void KeStackAttachProcess(PEPROCESS p, PW_KAPC_STATE *state)
{
	state->opaque[0] = (ULONG64)(uintptr_t)cur_proc;
	cur_proc = p;
	__atomic_fetch_add(&fake_attach_count, 1, __ATOMIC_RELAXED);
}

void KeUnstackDetachProcess(PW_KAPC_STATE *state)
{
	cur_proc = (PEPROCESS)(uintptr_t)state->opaque[0];
}

void ObfReferenceObject(PVOID o)
{
	struct fake_obhdr *h = o;

	if (h->magic != FAKE_OB_EVENT && h->magic != FAKE_OB_PROCESS)
		MISUSE("ObReferenceObject on a non-object");
	__atomic_fetch_add(&h->refs, 1, __ATOMIC_RELAXED);
}

void ObfDereferenceObject(PVOID o)
{
	struct fake_obhdr *h = o;
	int left;

	if (h->magic != FAKE_OB_EVENT && h->magic != FAKE_OB_PROCESS) {
		MISUSE("ObDereferenceObject on a non-object");
		return;
	}
	left = __atomic_sub_fetch(&h->refs, 1, __ATOMIC_ACQ_REL);
	if (left < 0)
		MISUSE("object over-dereferenced");
	if (!left && h->magic == FAKE_OB_EVENT) {
		h->magic = 0;
		free(o);
	}
}

NTSTATUS ObReferenceObjectByHandle(HANDLE h, ULONG access, POBJECT_TYPE type,
				   KPROCESSOR_MODE mode, PVOID *obj, PVOID info)
{
	struct fake_obhdr *hdr = h;

	(void)access;
	(void)mode;
	(void)info;
	if ((uintptr_t)h < 0x10000 || (uintptr_t)h == FAKE_BAD_USER_PTR)
		return STATUS_INVALID_HANDLE;
	if (hdr->magic == FAKE_OB_PROCESS)
		return STATUS_OBJECT_TYPE_MISMATCH;
	if (hdr->magic != FAKE_OB_EVENT || type != *ExEventObjectType)
		return STATUS_INVALID_HANDLE;
	ObfReferenceObject(h);
	*obj = h;
	return STATUS_SUCCESS;
}

PKEVENT fake_user_event_create(void)
{
	PKEVENT e = calloc(1, sizeof(*e));

	e->hdr.magic = FAKE_OB_EVENT;
	e->hdr.refs = 1;
	KeInitializeEvent(e, NotificationEvent, FALSE);
	return e;
}

int fake_user_event_refs(PKEVENT e)
{
	return __atomic_load_n(&e->hdr.refs, __ATOMIC_ACQUIRE);
}

void fake_user_event_close(PKEVENT e)
{
	ObfDereferenceObject(e);
}

/* -------------------------------------------------------------------------- pool */

PVOID ExAllocatePoolWithTag(POOL_TYPE type, SIZE_T size, ULONG tag)
{
	(void)type;
	(void)tag;
	return malloc(size ? size : 1);
}

void ExFreePoolWithTag(PVOID p, ULONG tag)
{
	(void)tag;
	free(p);
}

void ExFreePool(PVOID p)
{
	free(p);
}

/* ---------------------------------------------------------------------- user memory */

static int bad_user_range(const volatile void *addr, SIZE_T len)
{
	uintptr_t a = (uintptr_t)addr;

	return len && (a < 0x10000 || (a >= FAKE_BAD_USER_PTR && a < FAKE_BAD_USER_PTR + 0x10000));
}

void ProbeForRead(const volatile void *addr, SIZE_T len, ULONG align)
{
	(void)align;
	if (bad_user_range(addr, len))
		longjmp(fake_seh_jb, 1);
}

void ProbeForWrite(volatile void *addr, SIZE_T len, ULONG align)
{
	(void)align;
	if (bad_user_range(addr, len))
		longjmp(fake_seh_jb, 1);
}

/* ------------------------------------------------------------------------- MDLs */

struct user_map {
	struct user_map *next;
	void *va;
	PMDL mdl;
	PEPROCESS proc;
};
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
static struct user_map *user_maps;

PMDL IoAllocateMdl(PVOID va, ULONG len, BOOLEAN secondary, BOOLEAN charge, PVOID irp)
{
	PMDL m = calloc(1, sizeof(*m));

	(void)secondary;
	(void)charge;
	(void)irp;
	if (m) {
		m->start_va = va;
		m->byte_count = len;
	}
	return m;
}

void IoFreeMdl(PMDL m)
{
	free(m);
}

void IoBuildPartialMdl(PMDL src, PMDL dst, PVOID va, ULONG len)
{
	uintptr_t off = (uintptr_t)va - (uintptr_t)src->start_va;

	if (off + len > src->byte_count || !src->base)
		MISUSE("IoBuildPartialMdl outside the source MDL");
	dst->base = src->base + off;
	dst->byte_count = len;
}

void MmBuildMdlForNonPagedPool(PMDL m)
{
	m->base = m->start_va;
}

PMDL MmAllocatePagesForMdlEx(PHYSICAL_ADDRESS lo, PHYSICAL_ADDRESS hi, PHYSICAL_ADDRESS skip,
			     SIZE_T bytes, MEMORY_CACHING_TYPE cache, ULONG flags)
{
	size_t n = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	PMDL m = calloc(1, sizeof(*m) + n * sizeof(PFN_NUMBER));

	(void)lo;
	(void)hi;
	(void)skip;
	(void)cache;
	(void)flags;
	if (!m)
		return NULL;
	m->base = aligned_alloc(PAGE_SIZE, n * PAGE_SIZE);
	if (!m->base) {
		free(m);
		return NULL;
	}
	memset(m->base, 0, n * PAGE_SIZE);
	m->byte_count = (ULONG)(n * PAGE_SIZE);
	m->pages_owned = 1;
	for (size_t i = 0; i < n; i++)
		m->pfns[i] = ((uintptr_t)m->base >> PAGE_SHIFT) + i;
	return m;
}

void MmFreePagesFromMdl(PMDL m)
{
	if (!m->pages_owned)
		MISUSE("MmFreePagesFromMdl on a foreign MDL");
	free(m->base);
	m->base = NULL;
}

PVOID MmMapLockedPagesSpecifyCache(PMDL m, KPROCESSOR_MODE mode, MEMORY_CACHING_TYPE cache,
				   PVOID req_va, ULONG bugcheck, ULONG prio)
{
	struct user_map *u;

	(void)cache;
	(void)req_va;
	(void)bugcheck;
	(void)prio;
	if (!m->base)
		MISUSE("mapping an MDL without pages");
	if (mode != UserMode)
		return m->base;
	u = calloc(1, sizeof(*u));
	u->va = m->base;
	u->mdl = m;
	u->proc = IoGetCurrentProcess();
	pthread_mutex_lock(&map_lock);
	u->next = user_maps;
	user_maps = u;
	fake_user_maps_live++;
	pthread_mutex_unlock(&map_lock);
	return m->base;
}

void MmUnmapLockedPages(PVOID va, PMDL m)
{
	struct user_map *u = NULL;

	pthread_mutex_lock(&map_lock);
	for (struct user_map **pp = &user_maps; *pp; pp = &(*pp)->next) {
		if ((*pp)->va == va && (*pp)->mdl == m) {
			u = *pp;
			*pp = u->next;
			fake_user_maps_live--;
			break;
		}
	}
	pthread_mutex_unlock(&map_lock);
	if (!u)
		return;	/* a kernel mapping */
	if (u->proc != IoGetCurrentProcess())
		__atomic_fetch_add(&fake_unmap_wrong_process, 1, __ATOMIC_RELAXED);
	free(u);
}

PVOID MmAllocateContiguousNodeMemory(SIZE_T bytes, PHYSICAL_ADDRESS lo, PHYSICAL_ADDRESS hi,
				     PHYSICAL_ADDRESS boundary, ULONG protect, NODE_REQUIREMENT node)
{
	(void)lo;
	(void)hi;
	(void)boundary;
	(void)protect;
	(void)node;
	return aligned_alloc(PAGE_SIZE, (bytes + PAGE_SIZE - 1) & ~(SIZE_T)(PAGE_SIZE - 1));
}

void MmFreeContiguousMemory(PVOID p)
{
	free(p);
}

PHYSICAL_ADDRESS MmGetPhysicalAddress(PVOID va)
{
	PHYSICAL_ADDRESS pa;

	pa.QuadPart = (LONGLONG)(uintptr_t)va;
	return pa;
}

/* -------------------------------------------------------------------------- files */

NTSTATUS ZwCreateFile(HANDLE *h, ULONG access, OBJECT_ATTRIBUTES *oa, IO_STATUS_BLOCK *iosb,
		      PLARGE_INTEGER alloc, ULONG attrs, ULONG share, ULONG disp, ULONG opts,
		      PVOID ea, ULONG ealen)
{
	(void)h, (void)access, (void)oa, (void)iosb, (void)alloc, (void)attrs, (void)share;
	(void)disp, (void)opts, (void)ea, (void)ealen;
	return STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS ZwQueryInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, PVOID info, ULONG len, int cls)
{
	(void)h, (void)iosb, (void)info, (void)len, (void)cls;
	return STATUS_INVALID_HANDLE;
}

NTSTATUS ZwReadFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, IO_STATUS_BLOCK *iosb, PVOID buf,
		    ULONG len, PLARGE_INTEGER pos, PULONG key)
{
	(void)h, (void)ev, (void)apc, (void)ctx, (void)iosb, (void)buf, (void)len, (void)pos;
	(void)key;
	return STATUS_INVALID_HANDLE;
}

NTSTATUS ZwClose(HANDLE h)
{
	(void)h;
	return STATUS_SUCCESS;
}

/* --------------------------------------------------------------------------- WDF */

struct fake_wdf_object *fake_wdf_object_new(size_t ctx_size)
{
	struct fake_wdf_object *o = calloc(1, sizeof(*o));

	o->context = calloc(1, ctx_size ? ctx_size : 1);
	return o;
}

void fake_wdf_object_free(struct fake_wdf_object *o)
{
	if (!o)
		return;
	free(o->context);
	free(o->sysbuf);
	free(o);
}

void WdfRequestGetParameters(WDFREQUEST r, WDF_REQUEST_PARAMETERS *p)
{
	p->Type = WdfRequestTypeDeviceControl;
	p->Parameters.DeviceIoControl.IoControlCode = r->code;
	p->Parameters.DeviceIoControl.InputBufferLength = r->in_len;
	p->Parameters.DeviceIoControl.OutputBufferLength = r->out_len;
}

WDFFILEOBJECT WdfRequestGetFileObject(WDFREQUEST r)
{
	return r->file;
}

NTSTATUS WdfRequestRetrieveInputBuffer(WDFREQUEST r, SIZE_T min, PVOID *buf, SIZE_T *len)
{
	if (!r->in_len || r->in_len < min)
		return STATUS_BUFFER_TOO_SMALL;
	*buf = r->sysbuf;
	if (len)
		*len = r->in_len;
	return STATUS_SUCCESS;
}

NTSTATUS WdfRequestRetrieveOutputBuffer(WDFREQUEST r, SIZE_T min, PVOID *buf, SIZE_T *len)
{
	if (!r->out_len || r->out_len < min)
		return STATUS_BUFFER_TOO_SMALL;
	*buf = r->sysbuf;
	if (len)
		*len = r->out_len;
	return STATUS_SUCCESS;
}

void WdfRequestCompleteWithInformation(WDFREQUEST r, NTSTATUS st, ULONG_PTR info)
{
	if (r->completed)
		MISUSE("request completed twice");
	r->completed = 1;
	r->status = st;
	r->info = info;
}

void WdfRequestComplete(WDFREQUEST r, NTSTATUS st)
{
	WdfRequestCompleteWithInformation(r, st, 0);
}

KPROCESSOR_MODE WdfRequestGetRequestorMode(WDFREQUEST r)
{
	(void)r;
	return UserMode;
}

BOOLEAN WdfRequestIsFrom32BitProcess(WDFREQUEST r)
{
	(void)r;
	return FALSE;
}

NTSTATUS WdfDeviceEnqueueRequest(WDFDEVICE d, WDFREQUEST r)
{
	(void)d;
	r->enqueued = 1;
	WdfRequestComplete(r, STATUS_INVALID_DEVICE_REQUEST); /* what the default queue does */
	return STATUS_SUCCESS;
}

PDEVICE_OBJECT WdfDeviceWdmGetDeviceObject(WDFDEVICE d)
{
	(void)d;
	return &fake_devobj;
}

NTSTATUS WdfIoQueueCreate(WDFDEVICE d, WDF_IO_QUEUE_CONFIG *c, PVOID attr, WDFQUEUE *q)
{
	(void)d;
	(void)c;
	(void)attr;
	(void)q;
	return STATUS_SUCCESS;
}
