// SPDX-License-Identifier: MIT
/*
 * pw_os_win.c - Windows kernel (ARM64) implementation of kmd/core/pw_os.h.
 *
 * IRQL rules: reg/time/udelay/log are safe in the ISR (DIRQL); event_signal (KeSetEvent) needs
 * <= DISPATCH_LEVEL, i.e. the bottom half; everything else is PASSIVE_LEVEL.
 * pw_os_copy_{from,to}_user live in pw_win_user.c (SEH).
 * UNTESTED ON HARDWARE.
 */
#include "pw_win.h"
#include "pw_fmt.h"

/* Highest physical address handed to the GPU. The G610 MMU outputs 40-bit PAs (RK3588 DRAM
 * ends well below that); keep allocations inside so PTEs never truncate. */
#define PW_GPU_PA_MAX ((1ULL << 40) - 1)
#define PW_FW_MAX_SIZE (64u * 1024 * 1024)
#define PW_LOG_BUF 512 /* DbgPrint truncates at 512 bytes anyway */

/* ------------------------------------------------------------------------------- MMIO */

uint32_t pw_os_reg_read32(struct pw_os *os, uint32_t offset)
{
	if (!os->mmio || !os->regs_enabled || (offset & 3) || offset > os->mmio_len - 4)
		return 0xFFFFFFFFu;
	return READ_REGISTER_ULONG((volatile ULONG *)(os->mmio + offset));
}

void pw_os_reg_write32(struct pw_os *os, uint32_t offset, uint32_t value)
{
	if (!os->mmio || !os->regs_enabled || (offset & 3) || offset > os->mmio_len - 4)
		return;
	WRITE_REGISTER_ULONG((volatile ULONG *)(os->mmio + offset), value);
}

/* ------------------------------------------------------------------------------- time */

uint64_t pw_os_time_ns(struct pw_os *os)
{
	LARGE_INTEGER freq, now;
	uint64_t c, f;

	(void)os;
	now = KeQueryPerformanceCounter(&freq);
	c = (uint64_t)now.QuadPart;
	f = (uint64_t)freq.QuadPart;
	if (!f)
		return 0;
	/* Split to avoid 64-bit overflow (and a 128-bit division libcall). */
	return (c / f) * 1000000000ull + ((c % f) * 1000000000ull) / f;
}

void pw_os_udelay(struct pw_os *os, uint32_t us)
{
	(void)os;
	KeStallExecutionProcessor(us);
}

void pw_os_msleep(struct pw_os *os, uint32_t ms)
{
	LARGE_INTEGER interval;

	(void)os;
	interval.QuadPart = -(LONGLONG)ms * 10000; /* relative, 100 ns units */
	KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

/* ---------------------------------------------------------------------------- logging */

void pw_os_log(struct pw_os *os, enum pw_log_level lvl, const char *fmt, ...)
{
	static const ULONG levels[] = {
		[PW_LOG_ERR] = DPFLTR_ERROR_LEVEL,
		[PW_LOG_WARN] = DPFLTR_WARNING_LEVEL,
		[PW_LOG_INFO] = DPFLTR_TRACE_LEVEL,
		[PW_LOG_DBG] = DPFLTR_INFO_LEVEL,
	};
	char buf[PW_LOG_BUF];
	va_list ap;
	int n;

	(void)os;
	va_start(ap, fmt);
	n = pw_win_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	DbgPrintEx(DPFLTR_IHVDRIVER_ID, levels[(unsigned)lvl <= PW_LOG_DBG ? lvl : PW_LOG_DBG],
		   (n > 0 && n < (int)sizeof(buf) && buf[n - 1] == '\n') ? "panwin: %s" : "panwin: %s\n",
		   buf);
}

/* ------------------------------------------------------------------------------- heap */

static void *pw_win_pool_alloc(size_t size)
{
	void *p;

	if (!size)
		size = 1;
	if (pw_win_rt.ExAllocatePool2)
		return pw_win_rt.ExAllocatePool2(POOL_FLAG_NON_PAGED, size, PW_POOL_TAG); /* zeroed */
	p = ExAllocatePoolWithTag(NonPagedPoolNx, size, PW_POOL_TAG);
	if (p)
		RtlZeroMemory(p, size);
	return p;
}

void *pw_os_zalloc(struct pw_os *os, size_t size)
{
	(void)os;
	return pw_win_pool_alloc(size);
}

void pw_os_free(struct pw_os *os, void *ptr)
{
	(void)os;
	if (ptr)
		ExFreePoolWithTag(ptr, PW_POOL_TAG);
}

/* ---------------------------------------------------------------- cache maintenance */

static inline uintptr_t pw_dcache_line(void)
{
	uint64_t ctr;

	__asm__ __volatile__("mrs %0, ctr_el0" : "=r"(ctr));
	return (uintptr_t)4 << ((ctr >> 16) & 0xF); /* DminLine: log2(words) */
}

/* Clean (write back) to the point of coherency, so a non-coherent GPU sees CPU writes. */
static void pw_dcache_clean(const void *va, size_t len)
{
	uintptr_t line = pw_dcache_line();
	uintptr_t p = (uintptr_t)va & ~(line - 1), end = (uintptr_t)va + len;

	for (; p < end; p += line)
		__asm__ __volatile__("dc cvac, %0" : : "r"(p) : "memory");
	__asm__ __volatile__("dsb sy" ::: "memory");
}

/* Clean+invalidate, so later CPU reads fetch what the GPU wrote. (Not "dc ivac": that could
 * discard unrelated dirty data sharing a partial line at the range edges.) */
static void pw_dcache_clean_inval(const void *va, size_t len)
{
	uintptr_t line = pw_dcache_line();
	uintptr_t p = (uintptr_t)va & ~(line - 1), end = (uintptr_t)va + len;

	for (; p < end; p += line)
		__asm__ __volatile__("dc civac, %0" : : "r"(p) : "memory");
	__asm__ __volatile__("dsb sy" ::: "memory");
}

/* ------------------------------------------------------------------ GPU-visible memory */

struct pw_os_mem {
	size_t size;           /* bytes, multiple of 4 KiB */
	size_t npages;
	uint32_t flags;
	MEMORY_CACHING_TYPE cache;
	PMDL mdl;              /* all pages: MmAllocatePagesForMdlEx, or (contiguous) an MDL built
				* over contig_va with MmBuildMdlForNonPagedPool. User mappings are
				* partial MDLs of this one (pw_win_ioctl.c). */
	void *contig_va;       /* contiguous allocations (always mapped) */
	uint64_t contig_pa;
	void *kva;             /* returned by pw_os_mem_kva() */
	BOOLEAN kva_is_mdl_map;
};

static void *pw_mem_map_mdl(struct pw_os_mem *mem)
{
	return MmMapLockedPagesSpecifyCache(mem->mdl, KernelMode, mem->cache, NULL, FALSE,
					    NormalPagePriority | MdlMappingNoExecute);
}

int pw_os_mem_alloc(struct pw_os *os, size_t size, uint32_t flags, struct pw_os_mem **out)
{
	struct pw_os_mem *mem;
	PHYSICAL_ADDRESS lo, hi, skip;

	(void)os;
	*out = NULL;
	if (!size || size > ((size_t)1 << 40))
		return PW_EINVAL;
	mem = pw_win_pool_alloc(sizeof(*mem));
	if (!mem)
		return PW_ENOMEM;
	mem->size = (size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
	mem->npages = mem->size / PAGE_SIZE;
	mem->flags = flags;
	/* Every CPU mapping of a page must use the same attributes (ARM64 forbids mismatched
	 * aliases), so the cache type is fixed at allocation and reused for each mapping. */
	mem->cache = (flags & PW_MEM_UNCACHED) ? MmWriteCombined : MmCached;
	lo.QuadPart = 0;
	hi.QuadPart = (LONGLONG)PW_GPU_PA_MAX;
	skip.QuadPart = 0;

	if (flags & PW_MEM_CONTIGUOUS) {
		ULONG protect = PAGE_READWRITE | ((flags & PW_MEM_UNCACHED) ? PAGE_WRITECOMBINE : 0);

		mem->contig_va = MmAllocateContiguousNodeMemory(mem->size, lo, hi, skip, protect,
								 MM_ANY_NODE_OK);
		if (!mem->contig_va)
			goto err_nomem;
		mem->contig_pa = (uint64_t)MmGetPhysicalAddress(mem->contig_va).QuadPart;
		if (mem->size > 0xFFFFF000u) /* IoAllocateMdl takes a ULONG length */
			goto err_free_contig;
		mem->mdl = IoAllocateMdl(mem->contig_va, (ULONG)mem->size, FALSE, FALSE, NULL);
		if (!mem->mdl)
			goto err_free_contig;
		MmBuildMdlForNonPagedPool(mem->mdl);
		RtlZeroMemory(mem->contig_va, mem->size); /* not zeroed by Mm */
		if (!(flags & PW_MEM_UNCACHED))
			pw_dcache_clean(mem->contig_va, mem->size);
		else
			__asm__ __volatile__("dsb sy" ::: "memory");
		if (flags & PW_MEM_KMAP)
			mem->kva = mem->contig_va;
	} else {
		mem->mdl = MmAllocatePagesForMdlEx(lo, hi, skip, mem->size, mem->cache,
						   MM_ALLOCATE_FULLY_REQUIRED); /* zeroed */
		if (!mem->mdl)
			goto err_nomem;
		if (MmGetMdlByteCount(mem->mdl) < mem->size)
			goto err_free_mdl;
		if (flags & PW_MEM_KMAP) {
			mem->kva = pw_mem_map_mdl(mem);
			if (!mem->kva)
				goto err_free_mdl;
			mem->kva_is_mdl_map = TRUE;
		}
		/* Mm zeroed the pages through a cached mapping: push the zeroes to memory so the
		 * GPU cannot read stale data. WC allocations are handled by Mm. */
		if (!(flags & PW_MEM_UNCACHED))
			pw_os_mem_flush(os, mem, 0, mem->size);
	}
	*out = mem;
	return PW_OK;

err_free_mdl:
	MmFreePagesFromMdl(mem->mdl);
	ExFreePool(mem->mdl);
	goto err_nomem;
err_free_contig:
	MmFreeContiguousMemory(mem->contig_va);
err_nomem:
	ExFreePoolWithTag(mem, PW_POOL_TAG);
	return PW_ENOMEM;
}

void pw_os_mem_free(struct pw_os *os, struct pw_os_mem *mem)
{
	(void)os;
	if (!mem)
		return;
	if (mem->contig_va) {
		if (mem->mdl)
			IoFreeMdl(mem->mdl);
		MmFreeContiguousMemory(mem->contig_va);
	} else if (mem->mdl) {
		if (mem->kva_is_mdl_map)
			MmUnmapLockedPages(mem->kva, mem->mdl);
		MmFreePagesFromMdl(mem->mdl);
		ExFreePool(mem->mdl); /* MDLs from MmAllocatePagesForMdlEx are freed with ExFreePool */
	}
	ExFreePoolWithTag(mem, PW_POOL_TAG);
}

size_t pw_os_mem_size(const struct pw_os_mem *mem)
{
	return mem->size;
}

void *pw_os_mem_kva(const struct pw_os_mem *mem)
{
	return mem->kva;
}

uint64_t pw_os_mem_page_pa(const struct pw_os_mem *mem, size_t page_index)
{
	if (page_index >= mem->npages)
		return 0;
	if (mem->contig_va)
		return mem->contig_pa + (uint64_t)page_index * PAGE_SIZE;
	return (uint64_t)MmGetMdlPfnArray(mem->mdl)[page_index] << PAGE_SHIFT;
}

PMDL pw_os_win_mem_mdl(const struct pw_os_mem *mem)
{
	return mem->mdl;
}

MEMORY_CACHING_TYPE pw_os_win_mem_cache(const struct pw_os_mem *mem)
{
	return mem->cache;
}

static void pw_mem_maint(struct pw_os_mem *mem, size_t offset, size_t size, BOOLEAN inval)
{
	void *va, *tmp = NULL;

	if (offset >= mem->size)
		return;
	if (size > mem->size - offset)
		size = mem->size - offset;
	if (mem->cache != MmCached) {
		/* Normal-NC/WC: no cache lines to maintain; just drain the write buffers. */
		__asm__ __volatile__("dsb sy" ::: "memory");
		return;
	}
	va = mem->contig_va ? mem->contig_va : mem->kva;
	if (!va) {
		/* No kernel mapping: map temporarily (same MmCached attribute as every alias). */
		tmp = pw_mem_map_mdl(mem);
		if (!tmp) {
			PW_WLOG(PW_LOG_ERR, "cache maintenance: cannot map %zu bytes", mem->size);
			return;
		}
		va = tmp;
	}
	if (inval)
		pw_dcache_clean_inval((char *)va + offset, size);
	else
		pw_dcache_clean((char *)va + offset, size);
	if (tmp)
		MmUnmapLockedPages(tmp, mem->mdl);
}

void pw_os_mem_flush(struct pw_os *os, struct pw_os_mem *mem, size_t offset, size_t size)
{
	(void)os;
	pw_mem_maint(mem, offset, size, FALSE);
}

void pw_os_mem_invalidate(struct pw_os *os, struct pw_os_mem *mem, size_t offset, size_t size)
{
	(void)os;
	pw_mem_maint(mem, offset, size, TRUE);
}

/* -------------------------------------------------------------------------- mutexes */

/*
 * FAST_MUTEX acquired with the "Unsafe" variants inside a critical region: the holder stays at
 * PASSIVE_LEVEL (ExAcquireFastMutex would raise to APC_LEVEL), so code under the lock may do
 * file I/O (firmware loading) and waits; normal kernel APCs are disabled, which is what
 * prevents the thread from being suspended while holding the lock. Not recursive.
 */
struct pw_os_mutex {
	FAST_MUTEX fm;
};

struct pw_os_mutex *pw_os_mutex_create(struct pw_os *os)
{
	struct pw_os_mutex *m = pw_os_zalloc(os, sizeof(*m));

	if (m)
		ExInitializeFastMutex(&m->fm);
	return m;
}

void pw_os_mutex_destroy(struct pw_os *os, struct pw_os_mutex *m)
{
	pw_os_free(os, m);
}

void pw_os_mutex_lock(struct pw_os_mutex *m)
{
	KeEnterCriticalRegion();
	ExAcquireFastMutexUnsafe(&m->fm);
}

void pw_os_mutex_unlock(struct pw_os_mutex *m)
{
	ExReleaseFastMutexUnsafe(&m->fm);
	KeLeaveCriticalRegion();
}

/* --------------------------------------------------------------------------- events */

/* Manual-reset (notification) event: stays signalled until pw_os_event_reset(). */
struct pw_os_event {
	KEVENT ev;
	struct pw_os *os;	/* for pw_os_event_wait_user(): clock + in-flight ioctl lookup */
};

struct pw_os_event *pw_os_event_create(struct pw_os *os)
{
	struct pw_os_event *e = pw_os_zalloc(os, sizeof(*e)); /* nonpaged: KeSetEvent at DISPATCH */

	if (e) {
		KeInitializeEvent(&e->ev, NotificationEvent, FALSE);
		e->os = os;
	}
	return e;
}

void pw_os_event_destroy(struct pw_os *os, struct pw_os_event *e)
{
	pw_os_free(os, e);
}

void pw_os_event_signal(struct pw_os_event *e)
{
	KeSetEvent(&e->ev, IO_NO_INCREMENT, FALSE);
}

void pw_os_event_reset(struct pw_os_event *e)
{
	KeClearEvent(&e->ev);
}

/* timeout_ms == 0xFFFFFFFF waits forever. */
int pw_os_event_wait(struct pw_os_event *e, uint32_t timeout_ms)
{
	LARGE_INTEGER t;
	NTSTATUS st;

	t.QuadPart = -(LONGLONG)timeout_ms * 10000;
	st = KeWaitForSingleObject(&e->ev, Executive, KernelMode, FALSE,
				   timeout_ms == 0xFFFFFFFFu ? NULL : &t);
	if (st == STATUS_SUCCESS)
		return PW_OK;
	if (st == STATUS_TIMEOUT) /* NB: a success code, so NT_SUCCESS() would be wrong here */
		return PW_ETIMEDOUT;
	return PW_EIO;
}

/* -------------------------------------------------------------- in-flight ioctls */

void pw_os_win_active_enter(struct pw_os *os, struct pw_win_active *a, PKEVENT abort)
{
	KIRQL irql;

	a->thread_id = PsGetCurrentThreadId();
	a->abort = abort;
	KeAcquireSpinLock(&os->active_lock, &irql);
	InsertTailList(&os->active_list, &a->link);
	KeReleaseSpinLock(&os->active_lock, irql);
}

void pw_os_win_active_leave(struct pw_os *os, struct pw_win_active *a)
{
	KIRQL irql;

	KeAcquireSpinLock(&os->active_lock, &irql);
	RemoveEntryList(&a->link);
	KeReleaseSpinLock(&os->active_lock, irql);
}

/* Abort event of the file whose ioctl the current thread is executing, or NULL. The records
 * live in nonpaged request contexts (a UserMode wait may page out the waiter's stack, and other
 * threads walk this list at DISPATCH_LEVEL). */
static PKEVENT pw_os_win_current_abort(struct pw_os *os)
{
	HANDLE tid = PsGetCurrentThreadId();
	PKEVENT abort = NULL;
	PLIST_ENTRY le;
	KIRQL irql;

	if (!os)
		return NULL;
	KeAcquireSpinLock(&os->active_lock, &irql);
	for (le = os->active_list.Flink; le != &os->active_list; le = le->Flink) {
		struct pw_win_active *a = CONTAINING_RECORD(le, struct pw_win_active, link);

		if (a->thread_id == tid) {
			abort = a->abort;
			break;
		}
	}
	KeReleaseSpinLock(&os->active_lock, irql);
	return abort;
}

/*
 * Wait on behalf of a user ioctl (syncobj waits). UserMode + Alertable, like
 * NtWaitForSingleObject: the thread can be suspended/terminated while it waits, and a user APC
 * or alert (thread termination re-queues its exit APC as a user APC) ends the wait with
 * STATUS_USER_APC/STATUS_ALERTED -> PW_EINTR; the ioctl then completes with EINTR, libdrm-win
 * restarts it, and the APC runs on the way back to user mode. The wait also ends with PW_EINTR
 * when the file's abort event is set (handle cleanup or device teardown).
 * The deadline is absolute on the QPC clock (pw_os_time_ns); KeWait* takes a relative 100 ns
 * interval on the interrupt-time clock, so it is recomputed after every wake-up and a timeout
 * is only reported once pw_os_time_ns() has really passed the deadline.
 */
int pw_os_event_wait_user(struct pw_os_event *e, uint64_t deadline_ns)
{
	PVOID objs[2];
	ULONG n = 0;
	PKEVENT abort = pw_os_win_current_abort(e->os);

	objs[n++] = &e->ev;
	if (abort)
		objs[n++] = abort;
	for (;;) {
		LARGE_INTEGER t, *pt = NULL;
		NTSTATUS st;

		if (deadline_ns != UINT64_MAX) {
			uint64_t now = pw_os_time_ns(e->os), d100 = 0;

			if (deadline_ns > now) {
				d100 = (deadline_ns - now + 99) / 100;
				if (d100 > (1ull << 62))
					d100 = 1ull << 62;
			}
			t.QuadPart = -(LONGLONG)d100; /* 0 = poll */
			pt = &t;
		}
		/* n <= THREAD_WAIT_OBJECTS (3): the thread's built-in wait blocks are used. */
		st = KeWaitForMultipleObjects(n, objs, WaitAny, UserRequest, UserMode, TRUE, pt, NULL);
		if (st == STATUS_WAIT_0)
			return PW_OK;
		if (st == STATUS_WAIT_1)
			return PW_EINTR; /* file/device going away */
		if (st == STATUS_USER_APC || st == STATUS_ALERTED)
			return PW_EINTR;
		if (st == STATUS_TIMEOUT) {
			if (pw_os_time_ns(e->os) < deadline_ns)
				continue; /* interrupt time and QPC disagree slightly: wait the rest */
			return PW_ETIMEDOUT;
		}
		PW_WLOG(PW_LOG_ERR, "pw_os_event_wait_user: unexpected wait status 0x%08x",
			(unsigned)st);
		return PW_EIO;
	}
}

/* --------------------------------------------------------------------- deferred work */

/*
 * One IO_WORKITEM per pw_os_work (queued on the system DelayedWorkQueue; IoQueueWorkItem keeps
 * the device object referenced while it is queued/running, so the driver cannot unload under
 * it). State, under `lock` (a spin lock: the delay timer's DPC takes it):
 *   pending   a run is requested (immediate, or `delayed` until the KTIMER fires)
 *   queued    the IO_WORKITEM is in the system queue or its routine is executing; while it is
 *             set, the routine keeps running fn as long as an immediate run is pending, which
 *             gives "queued while running -> runs once more afterwards" and at most one
 *             concurrent execution
 *   running   fn is executing (diagnostics)
 *   dying     destroy has started: queue_delayed is ignored (a callback that re-arms itself
 *             with a delay - a periodic tick - must not keep destroy's flush busy forever;
 *             same as kmd/tests/pw_os_mock.c, which cancels delayed runs once stopping)
 * `idle` (notification event) is set whenever !queued && !(pending && delayed); flush waits
 * for it after turning a pending delayed run into an immediate one.
 */
struct pw_os_work {
	struct pw_os *os;
	pw_os_work_fn fn;
	void *ctx;
	PIO_WORKITEM item;
	KSPIN_LOCK lock;
	KTIMER timer;
	KDPC dpc;
	KEVENT idle;
	BOOLEAN pending, delayed, queued, running, dying;
};

static IO_WORKITEM_ROUTINE pw_work_routine;
static KDEFERRED_ROUTINE pw_work_dpc;

/* Caller holds w->lock. Make sure the worker runs (pending immediate run already set). */
static void pw_work_kick_locked(struct pw_os_work *w)
{
	if (w->queued)
		return;
	w->queued = TRUE;
	KeClearEvent(&w->idle);
	IoQueueWorkItem(w->item, pw_work_routine, DelayedWorkQueue, w);
}

static void pw_work_routine(PDEVICE_OBJECT devobj, PVOID context)
{
	struct pw_os_work *w = context;
	KIRQL irql;

	(void)devobj;
	KeAcquireSpinLock(&w->lock, &irql);
	while (w->pending && !w->delayed) {
		w->pending = FALSE;
		w->running = TRUE;
		KeReleaseSpinLock(&w->lock, irql);
		w->fn(w->ctx);
		KeAcquireSpinLock(&w->lock, &irql);
		w->running = FALSE;
	}
	w->queued = FALSE;
	if (!(w->pending && w->delayed))
		KeSetEvent(&w->idle, IO_NO_INCREMENT, FALSE); /* under the lock: see pw_os_work_flush */
	/* After this unlock the routine never touches w again (destroy may free it). */
	KeReleaseSpinLock(&w->lock, irql);
}

static void pw_work_dpc(PKDPC dpc, PVOID context, PVOID arg1, PVOID arg2)
{
	struct pw_os_work *w = context;

	(void)dpc;
	(void)arg1;
	(void)arg2;
	KeAcquireSpinLockAtDpcLevel(&w->lock);
	if (w->pending && w->delayed) { /* else cancelled, flushed or made immediate meanwhile */
		w->delayed = FALSE;
		pw_work_kick_locked(w);
	}
	KeReleaseSpinLockFromDpcLevel(&w->lock);
}

struct pw_os_work *pw_os_work_create(struct pw_os *os, pw_os_work_fn fn, void *ctx)
{
	struct pw_os_work *w = pw_os_zalloc(os, sizeof(*w));

	if (!w)
		return NULL;
	w->item = IoAllocateWorkItem(WdfDeviceWdmGetDeviceObject(os->device));
	if (!w->item) {
		pw_os_free(os, w);
		return NULL;
	}
	w->os = os;
	w->fn = fn;
	w->ctx = ctx;
	KeInitializeSpinLock(&w->lock);
	KeInitializeTimerEx(&w->timer, NotificationTimer);
	KeInitializeDpc(&w->dpc, pw_work_dpc, w);
	KeInitializeEvent(&w->idle, NotificationEvent, TRUE);
	return w;
}

void pw_os_work_queue(struct pw_os_work *w)
{
	KIRQL irql;

	KeAcquireSpinLock(&w->lock, &irql);
	if (w->pending && w->delayed) {
		/* Pending but delayed: run it now (same as the mock). The DPC, if it already
		 * fired, sees !delayed and does nothing. */
		KeCancelTimer(&w->timer);
		w->delayed = FALSE;
		pw_work_kick_locked(w);
	} else if (!w->pending) {
		w->pending = TRUE;
		pw_work_kick_locked(w);
	}
	KeReleaseSpinLock(&w->lock, irql);
}

void pw_os_work_queue_delayed(struct pw_os_work *w, uint32_t delay_ms)
{
	LARGE_INTEGER due;
	KIRQL irql;

	KeAcquireSpinLock(&w->lock, &irql);
	if (!w->pending && !w->dying) {
		w->pending = TRUE;
		if (!delay_ms) {
			pw_work_kick_locked(w);
		} else {
			w->delayed = TRUE;
			KeClearEvent(&w->idle);
			due.QuadPart = -(LONGLONG)delay_ms * 10000; /* relative, 100 ns */
			KeSetTimer(&w->timer, due, &w->dpc);
		}
	}
	KeReleaseSpinLock(&w->lock, irql);
}

void pw_os_work_flush(struct pw_os_work *w)
{
	KIRQL irql;
	BOOLEAN busy;

	for (;;) {
		KeAcquireSpinLock(&w->lock, &irql);
		if (w->pending && w->delayed) { /* flush runs a pending delayed item now */
			KeCancelTimer(&w->timer);
			w->delayed = FALSE;
			pw_work_kick_locked(w);
		}
		busy = w->queued;
		KeReleaseSpinLock(&w->lock, irql);
		if (!busy)
			return;
		KeWaitForSingleObject(&w->idle, Executive, KernelMode, FALSE, NULL);
	}
}

void pw_os_work_destroy(struct pw_os *os, struct pw_os_work *w)
{
	KIRQL irql;

	if (!w)
		return;
	KeAcquireSpinLock(&w->lock, &irql);
	w->dying = TRUE;               /* from now on delayed re-arms are dropped */
	if (w->pending && w->delayed) { /* cancel a pending delayed run */
		KeCancelTimer(&w->timer);
		w->pending = FALSE;
		w->delayed = FALSE;
	}
	KeReleaseSpinLock(&w->lock, irql);
	KeFlushQueuedDpcs(); /* a DPC that raced with KeCancelTimer has finished with w */
	pw_os_work_flush(w);  /* runs a pending immediate run, waits for the worker */
	IoFreeWorkItem(w->item);
	pw_os_free(os, w);
}

/* ------------------------------------------------------------------------- firmware */

void pw_os_win_init(struct pw_os *os, WDFDEVICE device)
{
	RtlZeroMemory(os, sizeof(*os));
	os->device = device;
	KeInitializeSpinLock(&os->active_lock);
	InitializeListHead(&os->active_list);
	pw_os_win_set_firmware_root(os, PW_DEFAULT_FIRMWARE_ROOT,
				    (USHORT)(sizeof(PW_DEFAULT_FIRMWARE_ROOT) / sizeof(WCHAR) - 1));
}

void pw_os_win_set_firmware_root(struct pw_os *os, const WCHAR *root, USHORT chars)
{
	USHORT n = 0;

	while (n < chars && root[n] && n < PW_PATH_MAX - 2) {
		os->fw_root[n] = root[n];
		n++;
	}
	if (n && os->fw_root[n - 1] != L'\\')
		os->fw_root[n++] = L'\\';
	os->fw_root[n] = 0;
	os->fw_root_chars = n;
}

/* fw_root + name, '/' -> '\'. Rejects non-ASCII, "..", ':' and absolute names. */
static int pw_fw_build_path(struct pw_os *os, const char *name, WCHAR *path, USHORT *chars)
{
	USHORT n = os->fw_root_chars;
	size_t i;

	if (!name || !name[0] || name[0] == '/' || name[0] == '\\')
		return PW_EINVAL;
	RtlCopyMemory(path, os->fw_root, n * sizeof(WCHAR));
	for (i = 0; name[i]; i++) {
		unsigned char c = (unsigned char)name[i];

		if (c < 0x20 || c > 0x7E || c == ':' ||
		    (c == '.' && name[i + 1] == '.'))
			return PW_EINVAL;
		if (n >= PW_PATH_MAX - 1)
			return PW_ERANGE;
		path[n++] = (c == '/') ? L'\\' : (WCHAR)c;
	}
	path[n] = 0;
	*chars = n;
	return PW_OK;
}

int pw_os_firmware_load(struct pw_os *os, const char *name, const void **data, size_t *size)
{
	WCHAR pathbuf[PW_PATH_MAX];
	UNICODE_STRING path;
	OBJECT_ATTRIBUTES oa;
	IO_STATUS_BLOCK iosb;
	FILE_STANDARD_INFORMATION info;
	HANDLE h = NULL;
	LARGE_INTEGER pos;
	USHORT chars;
	NTSTATUS st;
	ULONG len, done = 0;
	UCHAR *buf = NULL;
	int ret;

	*data = NULL;
	*size = 0;
	ret = pw_fw_build_path(os, name, pathbuf, &chars);
	if (ret)
		return ret;
	path.Buffer = pathbuf;
	path.Length = (USHORT)(chars * sizeof(WCHAR));
	path.MaximumLength = (USHORT)sizeof(pathbuf);
	InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

	st = ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
			  FILE_SHARE_READ, FILE_OPEN,
			  FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
	if (!NT_SUCCESS(st)) {
		char apath[PW_PATH_MAX];
		USHORT i;

		for (i = 0; i < chars && i < PW_PATH_MAX - 1; i++)
			apath[i] = pathbuf[i] < 0x80 ? (char)pathbuf[i] : '?';
		apath[i] = 0;
		PW_WLOG(PW_LOG_ERR, "firmware %s: open %s failed: 0x%08x", name, apath, (unsigned)st);
		return (st == STATUS_OBJECT_NAME_NOT_FOUND || st == STATUS_OBJECT_PATH_NOT_FOUND) ?
		       PW_ENOENT : PW_EIO;
	}
	st = ZwQueryInformationFile(h, &iosb, &info, sizeof(info), FileStandardInformation);
	if (!NT_SUCCESS(st)) {
		ret = PW_EIO;
		goto out;
	}
	if (info.EndOfFile.QuadPart <= 0 || info.EndOfFile.QuadPart > PW_FW_MAX_SIZE) {
		PW_WLOG(PW_LOG_ERR, "firmware %s: bad size %lld", name,
			(long long)info.EndOfFile.QuadPart);
		ret = PW_EINVAL;
		goto out;
	}
	len = (ULONG)info.EndOfFile.QuadPart;
	buf = pw_win_pool_alloc(len);
	if (!buf) {
		ret = PW_ENOMEM;
		goto out;
	}
	while (done < len) {
		pos.QuadPart = done;
		st = ZwReadFile(h, NULL, NULL, NULL, &iosb, buf + done, len - done, &pos, NULL);
		if (!NT_SUCCESS(st) || iosb.Information == 0) {
			PW_WLOG(PW_LOG_ERR, "firmware %s: read failed at %lu: 0x%08x", name,
				(unsigned long)done, (unsigned)st);
			ret = PW_EIO;
			goto out;
		}
		done += (ULONG)iosb.Information;
	}
	PW_WLOG(PW_LOG_INFO, "firmware %s: %lu bytes", name, (unsigned long)len);
	*data = buf;
	*size = len;
	buf = NULL;
	ret = PW_OK;
out:
	if (buf)
		ExFreePoolWithTag(buf, PW_POOL_TAG);
	ZwClose(h);
	return ret;
}

void pw_os_firmware_release(struct pw_os *os, const void *data)
{
	pw_os_free(os, (void *)data);
}
