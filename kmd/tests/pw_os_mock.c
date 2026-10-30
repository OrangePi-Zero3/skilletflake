// SPDX-License-Identifier: MIT
/*
 * pw_os_mock.c - pw_os.h for Linux host tests: mock register file + behavioural G610 model.
 * (jm-arch) mock_config.model selects a G31 (job manager) instead: GPU_ID/features of a G31 MP2
 * (mock_gpu.h, partly guessed), JM GPU_CMD encodings (SOFT_RESET 0x01, CLEAN(_INV)_CACHES
 * 0x07/0x08), shader/tiler/stack PWRON -> READY, a 256 KiB register window, no MCU, and the
 * JOB_CONTROL slot registers routed to mock_jm.c.
 *
 * Model summary (see mock_gpu.h):
 *  - GPU: GPU_ID/features from a real G610 dump, soft reset -> RESET_COMPLETED, L2_PWRON ->
 *    PWRTRANS for a few reads -> L2_READY (+ POWER_CHANGED bits the driver must keep masked),
 *    FLUSH_CACHES -> CLEAN_CACHES_COMPLETED.
 *  - MMU: AS_COMMAND keeps AS_STATUS.ACTIVE for a few reads, UPDATE latches TRANSTAB/MEMATTR/
 *    TRANSCFG, LOCK/UNLOCK tracked (UNLOCK without an intervening GPU cache flush is flagged),
 *    REQ_COMPLETE bits (16+as) raised in MMU RAWSTAT. Page tables are walked from "DRAM".
 *    (jm-mem) G31: FLUSH_PT/FLUSH_MEM end a LOCK; legacy Mali LPAE tables are walked too.
 *  - MCU: MCU_CONTROL=AUTO checks the code/shared mappings through AS0, writes a global/CSG/CS
 *    interface into the shared section (through AS0) and raises JOB_INT GLOBAL_IF; doorbell 0
 *    echoes GLB req->ack and doorbell_req->doorbell_ack, raising GLOBAL_IF if ack_irq_mask says so.
 *    CSG/CS requests and CS doorbells go to the command-stream executor in mock_csf.c.
 *  - Memory: fake PAs above 4 GiB, non-contiguous allocations get reversed, gapped PAs. Cached
 *    kernel mappings are a separate buffer: the GPU only sees what was pw_os_mem_flush()ed.
 */

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <malloc.h>
#include <windows.h>
#endif

#include "mock_gpu.h"
#include "mock_internal.h"
#include "pw_core.h"

/*
 * Portability (added by e2e): the mock is also built with x86_64-w64-mingw32 into libdrm-win's
 * in-process "core" transport (tests/e2e). Linux keeps the original calls; on Windows:
 *  - the pw_os_time_ns() clock is QueryPerformanceCounter-based, computed exactly like Mesa's
 *    os_time_get_nano() (c11 timespec_get(TIME_MONOTONIC)), as panwin_ioctl.h requires;
 *  - winpthreads ignores pthread_condattr_setclock() and measures absolute timeouts on the
 *    realtime clock, so monotonic deadlines are turned into relative waits;
 *  - aligned_alloc()/memmem() don't exist in the UCRT.
 */
static uint64_t mono_now_ns(void)
{
#ifdef _WIN32
	LARGE_INTEGER f, c;
	uint64_t sec;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	sec = (uint64_t)(c.QuadPart / f.QuadPart);
	return sec * 1000000000u +
	       (uint64_t)(c.QuadPart - (int64_t)sec * f.QuadPart) * 1000000000u / (uint64_t)f.QuadPart;
#else
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
}

static void mono_cond_init(pthread_cond_t *c)
{
#ifdef _WIN32
	pthread_cond_init(c, NULL);
#else
	pthread_condattr_t a;

	pthread_condattr_init(&a);
	pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
	pthread_cond_init(c, &a);
	pthread_condattr_destroy(&a);
#endif
}

/* pthread_cond_timedwait() with an absolute CLOCK_MONOTONIC deadline (mono_now_ns() clock). */
static int mono_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *ts)
{
#ifdef _WIN32
	uint64_t deadline = (uint64_t)ts->tv_sec * 1000000000u + (uint64_t)ts->tv_nsec;
	uint64_t now = mono_now_ns(), rel = deadline > now ? deadline - now : 0;
	struct timespec r;

	int ret;

	if (!rel)
		return ETIMEDOUT;
	rel += 999999u;	/* winpthreads waits whole milliseconds: never wake up early */
	if (rel > 3600000000000ull)
		rel = 3600000000000ull;	/* far deadlines: wait in 1 h slices (no ms overflow) */
	r.tv_sec = (time_t)(rel / 1000000000u);
	r.tv_nsec = (long)(rel % 1000000000u);
	ret = pthread_cond_timedwait_relative_np(c, m, &r);
	/* Only report ETIMEDOUT once the monotonic deadline really passed; anything earlier is a
	 * spurious wakeup the callers' predicate loops handle. */
	if (ret == ETIMEDOUT && mono_now_ns() < deadline)
		ret = 0;
	return ret;
#else
	return pthread_cond_timedwait(c, m, ts);
#endif
}

static void mono_timespec(struct timespec *ts)
{
#ifdef _WIN32
	uint64_t ns = mono_now_ns();

	ts->tv_sec = (time_t)(ns / 1000000000u);
	ts->tv_nsec = (long)(ns % 1000000000u);
#else
	clock_gettime(CLOCK_MONOTONIC, ts);
#endif
}

#ifdef _WIN32
#define mock_page_alloc(size)	_aligned_malloc((size), 4096)
#define mock_page_free(p)	_aligned_free(p)
static void *mock_memmem(const void *h, size_t hl, const void *n, size_t nl)
{
	const char *p = h;

	if (!nl)
		return (void *)p;
	for (size_t i = 0; i + nl <= hl; i++)
		if (!memcmp(p + i, n, nl))
			return (void *)(p + i);
	return NULL;
}
#define memmem mock_memmem
#else
#define mock_page_alloc(size)	aligned_alloc(4096, (size))
#define mock_page_free(p)	free(p)
#endif

#define REG_SPACE	0x200000u
#define PW_MIN_U64(a, b) ((a) < (b) ? (a) : (b))
#define PA_NPAGES	(1u << 20)	/* 4 GiB of fake physical address space */

/* Register offsets used by the model, from the Mali register map (independent of the core's
 * headers on purpose). */
#define R_GPU_ID		0x0
#define R_GPU_CMD		0x30
#define R_L2_READY		0x160
#define R_L2_PWRON		0x1a0
#define R_L2_PWROFF		0x1e0
#define R_L2_PWRTRANS		0x220
#define R_GPU_FAULT_STATUS	0x3c
#define R_GPU_FAULT_ADDR	0x40
#define R_MCU_CONTROL		0x700
#define R_MCU_STATUS		0x704
#define R_AS(as)		(0x2400u + ((as) << 6))
#define R_AS_TRANSTAB		0x0
#define R_AS_MEMATTR		0x8
#define R_AS_LOCKADDR		0x10
#define R_AS_COMMAND		0x18
#define R_AS_FAULTSTATUS	0x1c
#define R_AS_FAULTADDRESS	0x20
#define R_AS_STATUS		0x28
#define R_AS_TRANSCFG		0x30
#define R_DOORBELL0		0x80000u
/* (jm-arch) JM-only registers used by the G31 model. */
#define R_SHADER_READY		0x140
#define R_TILER_READY		0x150
#define R_SHADER_PWRON		0x180
#define R_TILER_PWRON		0x190
#define R_SHADER_PWROFF		0x1c0
#define R_TILER_PWROFF		0x1d0
#define R_GPU_STATUS		0x34
#define R_JS_PRESENT		0x1c
#define R_JS_FEATURES(n)	(0xc0 + (n) * 4)
#define R_AFBC_FEATURES		0x4c
#define R_THREAD_TLS_ALLOC	0x310
#define R_STACK_PRESENT		0xe00
#define R_STACK_READY		0xe10
#define R_STACK_PWRON		0xe20
#define R_STACK_PWROFF		0xe30
#define R_JOB_CONTROL_EXTRA	0x1010u		/* JS_STATE..: after the JOB_INT block */
#define R_JOB_CONTROL_END	0x2000u
#define G31_REG_WINDOW		0x40000u	/* gpu@1800000 is 0x40000 bytes */
#define R_LATEST_FLUSH_ID	0x38		/* (jm-hw) */
#define R_CYCLE_COUNT		0x90
#define R_TIMESTAMP		0x98

#define GPU_IRQ_RESET_COMPLETED		(1u << 8)
#define GPU_IRQ_POWER_CHANGED_BOTH	(3u << 9)
#define GPU_IRQ_CLEAN_CACHES		(1u << 17)
#define GPU_IRQ_FAULT			(1u << 0)
#define JOB_GLOBAL_IF			(1u << 31)

/* AArch64 descriptor bits (Arm ARM, 4 KB granule). */
#define D_VALID		(1ull << 0)
#define D_TABLE		(1ull << 1)
#define D_AF		(1ull << 10)
#define D_RDONLY	(1ull << 7)
#define D_XN		(1ull << 54)
#define D_OA		0x0000fffffffff000ull

/* Shared-section interface layout: mock_internal.h. */

struct pw_os_mutex {
	pthread_mutex_t m;
};

struct pw_os_event {
	pthread_mutex_t m;
	pthread_cond_t c;
	bool set;
};

struct pw_os_mem {
	size_t size;
	uint32_t flags;
	size_t npages;
	uint8_t *dram;		/* what the GPU sees */
	uint8_t *cpu;		/* kernel mapping (== dram when uncached) */
	uint64_t *pa;
	uint8_t *user;		/* mock_mem_user_view() (e2e): == dram when uncached, else the
				 * CPU-cache view shared with `cpu` (allocated on first use) */
#ifdef _WIN32
	HANDLE section;		/* dram is a view of this pagefile section, so user mappings
				 * can be distinct views of the same pages (mock_mem_user_map) */
#endif
};

/* "DRAM" backing store of a pw_os_mem (page aligned). */
static uint8_t *dram_alloc(struct pw_os_mem *m, size_t size)
{
#ifdef _WIN32
	void *p;

	m->section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
					(DWORD)((uint64_t)size >> 32), (DWORD)size, NULL);
	if (!m->section)
		return NULL;
	p = MapViewOfFile(m->section, FILE_MAP_ALL_ACCESS, 0, 0, size);
	if (!p) {
		CloseHandle(m->section);
		m->section = NULL;
	}
	return p;
#else
	(void)m;
	return mock_page_alloc(size);
#endif
}

static void dram_free(struct pw_os_mem *m)
{
#ifdef _WIN32
	if (m->dram)
		UnmapViewOfFile(m->dram);
	if (m->section)
		CloseHandle(m->section);
#else
	mock_page_free(m->dram);
#endif
}

struct irq_thread {
	struct pw_os *os;
	unsigned int line;
	pthread_t thr;
};

struct pw_os {
	struct mock_config cfg;
	pthread_mutex_t lock;		/* protects everything below */
	pthread_cond_t cond;

	uint32_t *regs;
	uint32_t rawstat[3], mask[3];	/* 0 job, 1 mmu, 2 gpu */
	uint32_t delayed_bits[3];
	uint64_t delayed_at[3];

	uint64_t l2_ready, l2_pending;
	int l2_trans_reads;
	uint32_t soft_resets, flushes, last_flush;

	struct mock_as_state as[16];
	uint32_t as_busy[16];
	uint32_t flushes_at_lock[16];

	uint32_t mcu_status;
	bool mcu_running;
	struct mock_glb_snapshot glb;
	uint32_t model_errors;

	uint8_t **pa_pages;
	uint64_t pa_base;
	uint64_t next_pfn;

	struct irq_thread thr[3];
	bool running;
	struct pw_device *dev;
	int in_flight;
	uint32_t delivered[3], unclaimed;

	char *log;
	size_t log_len, log_cap;
	bool verbose;
	char fw_name[128];
	void *fw_buf;

	long allocs, mems, syncs;
	struct mock_csf *csf;
	struct mock_jm *jm;		/* (jm-arch) G31 model */
	/* (jm-hw) G31 cycle counter: GPU_CYCLE_COUNT runs at a nominal 400 MHz while started */
	uint64_t jm_cycles_acc, jm_cycles_since_ns;
	bool jm_cycles_on;
	uint32_t fail_mem_allocs;	/* (jm-mem) mock_fail_mem_allocs() */
};

static const uint32_t int_base[3] = { 0x1000, 0x2000, 0x20 };

#define model_error mock_model_error
static _Thread_local bool tls_in_irq_thread;

void mock_model_error(struct pw_os *os, const char *fmt, ...)
{
	va_list ap;

	os->model_errors++;
	fprintf(stderr, "MOCK MODEL ERROR: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------------------------------ */
/* memory                                                                                     */
/* ------------------------------------------------------------------------------------------ */

static uint8_t *pa_host_locked(struct pw_os *os, uint64_t pa)
{
	uint64_t pfn;

	if (pa < os->pa_base)
		return NULL;
	pfn = (pa - os->pa_base) >> 12;
	if (pfn >= PA_NPAGES || !os->pa_pages[pfn])
		return NULL;
	return os->pa_pages[pfn] + (pa & 0xfff);
}

uint8_t *mock_pa_to_host(struct pw_os *os, uint64_t pa)
{
	uint8_t *p;

	pthread_mutex_lock(&os->lock);
	p = pa_host_locked(os, pa);
	pthread_mutex_unlock(&os->lock);
	return p;
}

int pw_os_mem_alloc(struct pw_os *os, size_t size, uint32_t flags, struct pw_os_mem **out)
{
	struct pw_os_mem *m;
	size_t npages;

	if (!size || !out)
		return PW_EINVAL;
	if (size > (256u << 20))
		return PW_ENOMEM;	/* keep fuzzed inputs from eating the host */
	/* (jm-mem) injected allocation failures (mock_fail_mem_allocs) */
	pthread_mutex_lock(&os->lock);
	if (os->fail_mem_allocs) {
		os->fail_mem_allocs--;
		pthread_mutex_unlock(&os->lock);
		return PW_ENOMEM;
	}
	pthread_mutex_unlock(&os->lock);
	size = (size + 0xfff) & ~(size_t)0xfff;
	npages = size >> 12;
	m = calloc(1, sizeof(*m));
	if (!m)
		return PW_ENOMEM;
	m->size = size;
	m->flags = flags;
	m->npages = npages;
	m->dram = dram_alloc(m, size);
	m->pa = calloc(npages, sizeof(*m->pa));
	if (!m->dram || !m->pa)
		goto err;
	/* real allocations aren't zeroed (unless the e2e config asks for PW_MEM_ZEROED) */
	memset(m->dram, (os->cfg.honor_zeroed && (flags & PW_MEM_ZEROED)) ? 0 : 0xa5, size);
	if (flags & PW_MEM_KMAP) {
		if (flags & PW_MEM_UNCACHED) {
			m->cpu = m->dram;
		} else {
			m->cpu = mock_page_alloc(size);
			if (!m->cpu)
				goto err;
			memcpy(m->cpu, m->dram, size);
		}
	}

	pthread_mutex_lock(&os->lock);
	if (flags & PW_MEM_CONTIGUOUS) {
		uint64_t align = 1;

		while (align < npages && align < 512)
			align <<= 1;
		os->next_pfn = (os->next_pfn + align - 1) & ~(align - 1);
		if (os->next_pfn + npages > PA_NPAGES) {
			pthread_mutex_unlock(&os->lock);
			goto err;
		}
		for (size_t i = 0; i < npages; i++) {
			m->pa[i] = os->pa_base + ((os->next_pfn + i) << 12);
			os->pa_pages[os->next_pfn + i] = m->dram + (i << 12);
		}
		os->next_pfn += npages;
	} else {
		if (os->next_pfn + 2 * npages > PA_NPAGES) {
			pthread_mutex_unlock(&os->lock);
			goto err;
		}
		/* Reversed and gapped: page i+1 is never at PA(page i) + 4K. */
		for (size_t i = 0; i < npages; i++) {
			size_t idx = npages - 1 - i;

			m->pa[idx] = os->pa_base + (os->next_pfn << 12);
			os->pa_pages[os->next_pfn] = m->dram + (idx << 12);
			os->next_pfn += 2;
		}
	}
	os->mems++;
	pthread_mutex_unlock(&os->lock);
	*out = m;
	return PW_OK;

err:
	if (m->cpu && m->cpu != m->dram)
		mock_page_free(m->cpu);
	dram_free(m);
	free(m->pa);
	free(m);
	return PW_ENOMEM;
}

void pw_os_mem_free(struct pw_os *os, struct pw_os_mem *m)
{
	if (!m)
		return;
	pthread_mutex_lock(&os->lock);
	for (size_t i = 0; i < m->npages; i++) {
		uint64_t pfn = (m->pa[i] - os->pa_base) >> 12;

		if (pfn < PA_NPAGES)
			os->pa_pages[pfn] = NULL;
	}
	os->mems--;
	pthread_mutex_unlock(&os->lock);
	if (m->user && m->user != m->dram && m->user != m->cpu)
		mock_page_free(m->user);
	if (m->cpu && m->cpu != m->dram)
		mock_page_free(m->cpu);
	dram_free(m);
	free(m->pa);
	free(m);
}

size_t pw_os_mem_size(const struct pw_os_mem *m)
{
	return m->size;
}

void *pw_os_mem_kva(const struct pw_os_mem *m)
{
	return m->cpu;
}

uint64_t pw_os_mem_page_pa(const struct pw_os_mem *m, size_t i)
{
	if (i >= m->npages) {
		fprintf(stderr, "MOCK: page index %zu out of range (%zu pages)\n", i, m->npages);
		abort();
	}
	return m->pa[i];
}

static void check_range(const struct pw_os_mem *m, size_t off, size_t size, const char *what)
{
	if (off > m->size || size > m->size - off) {
		fprintf(stderr, "MOCK: %s out of range (off 0x%zx size 0x%zx, mem 0x%zx)\n", what,
			off, size, m->size);
		abort();
	}
}

void pw_os_mem_flush(struct pw_os *os, struct pw_os_mem *m, size_t off, size_t size)
{
	(void)os;
	check_range(m, off, size, "flush");
	if (m->cpu && m->cpu != m->dram)
		memcpy(m->dram + off, m->cpu + off, size);
	else if (!m->cpu && m->user && m->user != m->dram)
		memcpy(m->dram + off, m->user + off, size);
}

void pw_os_mem_invalidate(struct pw_os *os, struct pw_os_mem *m, size_t off, size_t size)
{
	(void)os;
	check_range(m, off, size, "invalidate");
	if (m->cpu && m->cpu != m->dram)
		memcpy(m->cpu + off, m->dram + off, size);
	else if (!m->cpu && m->user && m->user != m->dram)
		memcpy(m->user + off, m->dram + off, size);
}

/*
 * (e2e) What a user mapping of @m shows: for PW_MEM_UNCACHED (write-combined) memory the same
 * bytes the GPU sees; for cached memory the CPU-cache view (the kernel view when there is one),
 * which the GPU only sees through pw_os_mem_flush() and which only sees GPU writes after
 * pw_os_mem_invalidate(). NULL on allocation failure. Stable for the life of @m.
 */
void *mock_mem_user_view(struct pw_os *os, struct pw_os_mem *m)
{
	void *v;

	pthread_mutex_lock(&os->lock);
	if (!m->user) {
		if (m->flags & PW_MEM_UNCACHED) {
			m->user = m->dram;
		} else if (m->cpu) {
			m->user = m->cpu;
		} else {
			m->user = mock_page_alloc(m->size);
			if (m->user)
				memcpy(m->user, m->dram, m->size);
		}
	}
	v = m->user;
	pthread_mutex_unlock(&os->lock);
	return v;
}

void *mock_mem_user_map(struct pw_os *os, struct pw_os_mem *m, bool writable)
{
#ifdef _WIN32
	if (m->flags & PW_MEM_UNCACHED)
		return MapViewOfFile(m->section, writable ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0,
				     m->size);
#endif
	(void)writable;
	return mock_mem_user_view(os, m);
}

void mock_mem_user_unmap(struct pw_os *os, struct pw_os_mem *m, void *view)
{
	(void)os;
#ifdef _WIN32
	if (m->flags & PW_MEM_UNCACHED) {
		UnmapViewOfFile(view);
		return;
	}
#endif
	(void)m;
	(void)view;
}

void *pw_os_zalloc(struct pw_os *os, size_t size)
{
	void *p = calloc(1, size ? size : 1);

	if (p)
		__atomic_fetch_add(&os->allocs, 1, __ATOMIC_RELAXED);
	return p;
}

void pw_os_free(struct pw_os *os, void *p)
{
	if (p)
		__atomic_fetch_sub(&os->allocs, 1, __ATOMIC_RELAXED);
	free(p);
}

/* ------------------------------------------------------------------------------------------ */
/* sync, time, log, firmware                                                                  */
/* ------------------------------------------------------------------------------------------ */

struct pw_os_mutex *pw_os_mutex_create(struct pw_os *os)
{
	struct pw_os_mutex *m = calloc(1, sizeof(*m));
	pthread_mutexattr_t a;

	if (!m)
		return NULL;
	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
	pthread_mutex_init(&m->m, &a);
	pthread_mutexattr_destroy(&a);
	__atomic_fetch_add(&os->syncs, 1, __ATOMIC_RELAXED);
	return m;
}

void pw_os_mutex_destroy(struct pw_os *os, struct pw_os_mutex *m)
{
	if (!m)
		return;
	if (pthread_mutex_destroy(&m->m)) {
		fprintf(stderr, "MOCK: destroying a locked mutex\n");
		abort();
	}
	__atomic_fetch_sub(&os->syncs, 1, __ATOMIC_RELAXED);
	free(m);
}

void pw_os_mutex_lock(struct pw_os_mutex *m)
{
	int r = pthread_mutex_lock(&m->m);

	if (r) {
		fprintf(stderr, "MOCK: mutex lock failed: %s (recursive locking?)\n", strerror(r));
		abort();
	}
}

void pw_os_mutex_unlock(struct pw_os_mutex *m)
{
	int r = pthread_mutex_unlock(&m->m);

	if (r) {
		fprintf(stderr, "MOCK: mutex unlock failed: %s\n", strerror(r));
		abort();
	}
}

struct pw_os_event *pw_os_event_create(struct pw_os *os)
{
	struct pw_os_event *e = calloc(1, sizeof(*e));

	if (!e)
		return NULL;
	pthread_mutex_init(&e->m, NULL);
	mono_cond_init(&e->c);
	__atomic_fetch_add(&os->syncs, 1, __ATOMIC_RELAXED);
	return e;
}

void pw_os_event_destroy(struct pw_os *os, struct pw_os_event *e)
{
	if (!e)
		return;
	pthread_cond_destroy(&e->c);
	pthread_mutex_destroy(&e->m);
	__atomic_fetch_sub(&os->syncs, 1, __ATOMIC_RELAXED);
	free(e);
}

void pw_os_event_signal(struct pw_os_event *e)
{
	pthread_mutex_lock(&e->m);
	e->set = true;
	pthread_cond_broadcast(&e->c);
	pthread_mutex_unlock(&e->m);
}

void pw_os_event_reset(struct pw_os_event *e)
{
	pthread_mutex_lock(&e->m);
	e->set = false;
	pthread_mutex_unlock(&e->m);
}

static void ts_add_ns(struct timespec *ts, uint64_t ns)
{
	ns += (uint64_t)ts->tv_nsec;
	ts->tv_sec += (time_t)(ns / 1000000000u);
	ts->tv_nsec = (long)(ns % 1000000000u);
}

int pw_os_event_wait(struct pw_os_event *e, uint32_t timeout_ms)
{
	struct timespec ts;
	int ret = PW_OK;

	mono_timespec(&ts);
	ts_add_ns(&ts, (uint64_t)timeout_ms * 1000000u);
	pthread_mutex_lock(&e->m);
	while (!e->set) {
		if (mono_cond_timedwait(&e->c, &e->m, &ts) == ETIMEDOUT) {
			ret = e->set ? PW_OK : PW_ETIMEDOUT;
			break;
		}
	}
	pthread_mutex_unlock(&e->m);
	return ret;
}

uint64_t pw_os_time_ns(struct pw_os *os)
{
	(void)os;
	return mono_now_ns();
}

void pw_os_udelay(struct pw_os *os, uint32_t us)
{
	struct timespec ts = { 0, (long)us * 1000 };

	(void)os;
	nanosleep(&ts, NULL);
}

void pw_os_msleep(struct pw_os *os, uint32_t ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };

	(void)os;
	nanosleep(&ts, NULL);
}

void pw_os_log(struct pw_os *os, enum pw_log_level lvl, const char *fmt, ...)
{
	static const char *const names[] = { "ERR", "WARN", "INFO", "DBG" };
	char line[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof(line))
		n = sizeof(line) - 1;

	pthread_mutex_lock(&os->lock);
	if (os->log_len + (size_t)n + 16 > os->log_cap) {
		size_t cap = (os->log_cap + (size_t)n + 16) * 2;
		char *p = realloc(os->log, cap);

		if (p) {
			os->log = p;
			os->log_cap = cap;
		}
	}
	if (os->log_len + (size_t)n + 16 <= os->log_cap)
		os->log_len += (size_t)sprintf(os->log + os->log_len, "%s: %s\n",
					       names[lvl <= PW_LOG_DBG ? lvl : PW_LOG_DBG], line);
	pthread_mutex_unlock(&os->lock);
	if (os->verbose)
		fprintf(stderr, "  [pw %s] %s\n", names[lvl <= PW_LOG_DBG ? lvl : PW_LOG_DBG], line);
}

bool mock_log_contains(struct pw_os *os, const char *needle)
{
	bool r;

	pthread_mutex_lock(&os->lock);
	r = os->log && memmem(os->log, os->log_len, needle, strlen(needle));
	pthread_mutex_unlock(&os->lock);
	return r;
}

void mock_log_clear(struct pw_os *os)
{
	pthread_mutex_lock(&os->lock);
	os->log_len = 0;
	pthread_mutex_unlock(&os->lock);
}

const char *mock_last_fw_name(struct pw_os *os)
{
	return os->fw_name;
}

int pw_os_firmware_load(struct pw_os *os, const char *name, const void **data, size_t *size)
{
	FILE *f;
	long len;

	snprintf(os->fw_name, sizeof(os->fw_name), "%s", name);
	if (os->cfg.fw_data) {
		*data = os->cfg.fw_data;
		*size = os->cfg.fw_size;
		return PW_OK;
	}
	if (!os->cfg.fw_path)
		return PW_ENOENT;
	f = fopen(os->cfg.fw_path, "rb");
	if (!f)
		return PW_ENOENT;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	os->fw_buf = malloc(len > 0 ? (size_t)len : 1);
	if (!os->fw_buf || fread(os->fw_buf, 1, (size_t)len, f) != (size_t)len) {
		fclose(f);
		free(os->fw_buf);
		os->fw_buf = NULL;
		return PW_EIO;
	}
	fclose(f);
	*data = os->fw_buf;
	*size = (size_t)len;
	return PW_OK;
}

void pw_os_firmware_release(struct pw_os *os, const void *data)
{
	if (data && data == os->fw_buf) {
		free(os->fw_buf);
		os->fw_buf = NULL;
	}
}

long mock_live_allocs(struct pw_os *os) { return __atomic_load_n(&os->allocs, __ATOMIC_RELAXED); }
long mock_live_mems(struct pw_os *os) { return os->mems; }
long mock_live_sync(struct pw_os *os) { return __atomic_load_n(&os->syncs, __ATOMIC_RELAXED); }

/* ------------------------------------------------------------------------------------------ */
/* emulated GPU MMU                                                                           */
/* ------------------------------------------------------------------------------------------ */

/*
 * (jm-mem) Legacy Mali LPAE walk (G31 model, TRANSCFG ADRMODE_LEGACY): written from the format
 * facts listed in mock_gpu.h, independently of the core's pw_mmu.c. Always 4 levels / 48-bit
 * input; 0b11 = table at levels 0-2, 0b01 = block (levels 1-2) or page (level 3), anything else
 * at a level is a translation fault; bit 6 must be set for reads, bit 7 for writes; XN (54)
 * forbids execution. There is no access flag.
 */
#define L_RD		(1ull << 6)
#define L_WR		(1ull << 7)

static int translate_lpae_locked(struct pw_os *os, unsigned int as, uint64_t va, char access,
				 uint64_t *pa, int *level, uint64_t *pte)
{
	struct mock_as_state *st = &os->as[as];
	uint64_t table;
	unsigned int lvl;

	if (os->cfg.model != MOCK_MODEL_G31) {
		model_error(os, "AS%u: legacy TRANSCFG on a GPU without Mali LPAE support", as);
		return 0xc0;
	}
	if ((st->transtab & 3) != 3) {
		model_error(os, "AS%u: legacy TRANSTAB 0x%llx without ADRMODE_TABLE", as,
			    (unsigned long long)st->transtab);
		return 0xc0;
	}
	if (va >> 48)
		return 0xc0;
	table = st->transtab & D_OA;
	for (lvl = 0;; lvl++) {
		unsigned int shift = 12 + 9 * (3 - lvl);
		unsigned int idx = (unsigned int)(va >> shift) & 511;
		uint8_t *p = pa_host_locked(os, table + idx * 8);
		uint64_t e;

		if (!p)
			return 0x80;
		memcpy(&e, p, 8);
		if (lvl < 3 && (e & 3) == 3) {
			table = e & D_OA;
			continue;
		}
		if ((e & 3) != 1 || lvl == 0)
			return 0xc0 + (int)lvl;	/* invalid, or a table/reserved encoding */
		if ((access == 'r' && !(e & L_RD)) || (access == 'w' && !(e & L_WR)) ||
		    (access == 'x' && (e & D_XN)))
			return 0xc8 + (int)lvl;
		if (pa) {
			uint64_t bs = 1ull << shift;

			*pa = (e & D_OA & ~(bs - 1)) | (va & (bs - 1));
		}
		if (level)
			*level = (int)lvl;
		if (pte)
			*pte = e;
		return 0;
	}
}

static int translate_locked(struct pw_os *os, unsigned int as, uint64_t va, char access,
			    uint64_t *pa, int *level, uint64_t *pte)
{
	struct mock_as_state *st = &os->as[as];
	unsigned int ina, va_bits, levels, lvl;
	uint64_t table;

	if (as >= 16 || !st->enabled)
		return 0xc0;
	if ((st->transcfg & 0xf) == 0)		/* (jm-mem) ADRMODE_LEGACY */
		return translate_lpae_locked(os, as, va, access, pa, level, pte);
	if ((st->transcfg & 0xf) != 6) {
		model_error(os, "AS%u: TRANSCFG 0x%llx is not AARCH64_4K", as,
			    (unsigned long long)st->transcfg);
		return 0xc0;
	}
	ina = (unsigned int)(st->transcfg >> 6) & 0x1f;
	va_bits = 55 - ina;
	if (va_bits > 48 || va_bits < 25) {
		model_error(os, "AS%u: INA_BITS %u gives %u VA bits", as, ina, va_bits);
		return 0xe0;
	}
	if (va >> va_bits)
		return 0xe0;	/* ADDR_SIZE_FAULT_IN */
	levels = (va_bits - 12 + 8) / 9;
	lvl = 4 - levels;
	table = st->transtab & D_OA;

	for (;;) {
		unsigned int shift = 12 + 9 * (3 - lvl);
		unsigned int idx = (unsigned int)(va >> shift) & 511;
		uint8_t *p = pa_host_locked(os, table + idx * 8);
		uint64_t e;

		if (!p)
			return 0x80;	/* walker hit unbacked memory */
		memcpy(&e, p, 8);
		if (!(e & D_VALID))
			return 0xc0 + (int)lvl;
		if (lvl < 3 && (e & D_TABLE)) {
			table = e & D_OA;
			lvl++;
			continue;
		}
		if ((lvl == 3 && !(e & D_TABLE)) || lvl == 0)
			return 0xc0 + (int)lvl;	/* reserved encoding */
		if (!(e & D_AF))
			return 0xd8 + (int)lvl;
		if (access == 'w' && (e & D_RDONLY))
			return 0xc8 + (int)lvl;
		if (access == 'x' && (e & D_XN))
			return 0xc8 + (int)lvl;
		if (pa) {
			uint64_t bs = 1ull << shift;

			*pa = (e & D_OA & ~(bs - 1)) | (va & (bs - 1));
		}
		if (level)
			*level = (int)lvl;
		if (pte)
			*pte = e;
		return 0;
	}
}

int mock_gpu_translate(struct pw_os *os, unsigned int as, uint64_t va, char access, uint64_t *pa,
		       int *level, uint64_t *pte)
{
	int r;

	pthread_mutex_lock(&os->lock);
	r = translate_locked(os, as, va, access, pa, level, pte);
	pthread_mutex_unlock(&os->lock);
	return r;
}

static int gpu_rw_locked(struct pw_os *os, unsigned int as, uint64_t va, void *buf, size_t len,
			 bool write)
{
	uint8_t *b = buf;

	while (len) {
		size_t chunk = 0x1000 - (va & 0xfff);
		uint64_t pa;
		uint8_t *host;
		int r;

		if (chunk > len)
			chunk = len;
		r = translate_locked(os, as, va, write ? 'w' : 'r', &pa, NULL, NULL);
		if (r)
			return r;
		host = pa_host_locked(os, pa);
		if (!host)
			return 0x80;
		if (write)
			memcpy(host, b, chunk);
		else
			memcpy(b, host, chunk);
		va += chunk;
		b += chunk;
		len -= chunk;
	}
	return 0;
}

int mock_gpu_read(struct pw_os *os, unsigned int as, uint64_t va, void *buf, size_t len)
{
	int r;

	pthread_mutex_lock(&os->lock);
	r = gpu_rw_locked(os, as, va, buf, len, false);
	pthread_mutex_unlock(&os->lock);
	return r;
}

int mock_gpu_write(struct pw_os *os, unsigned int as, uint64_t va, const void *buf, size_t len)
{
	int r;

	pthread_mutex_lock(&os->lock);
	r = gpu_rw_locked(os, as, va, (void *)buf, len, true);
	pthread_mutex_unlock(&os->lock);
	return r;
}

int mock_gpu_rw_locked(struct pw_os *os, unsigned int as, uint64_t va, void *buf, size_t len,
		       bool write)
{
	return gpu_rw_locked(os, as, va, buf, len, write);
}

void mock_lock(struct pw_os *os)
{
	pthread_mutex_lock(&os->lock);
}

void mock_unlock(struct pw_os *os)
{
	pthread_mutex_unlock(&os->lock);
}

struct mock_csf *mock_csf_of(struct pw_os *os)
{
	return os->csf;
}

/* ---- (jm-arch) mock_internal.h accessors for mock_jm.c ---- */

struct mock_jm *mock_jm_of(struct pw_os *os)
{
	return os->jm;
}

enum mock_model mock_model_of(struct pw_os *os)
{
	return os->cfg.model;
}

uint32_t mock_reg_get_locked(struct pw_os *os, uint32_t off)
{
	return off < REG_SPACE ? os->regs[off / 4] : 0;
}

void mock_reg_set_locked(struct pw_os *os, uint32_t off, uint32_t v)
{
	if (off < REG_SPACE)
		os->regs[off / 4] = v;
}

static bool is_jm(const struct pw_os *os)
{
	return os->cfg.model == MOCK_MODEL_G31;
}

/* ---- (jm-hw) more mock_internal.h helpers for mock_jm.c ---- */

uint32_t mock_rawstat_locked(struct pw_os *os, unsigned int line)
{
	return line < 3 ? os->rawstat[line] : 0;
}

bool mock_as_enabled_locked(struct pw_os *os, unsigned int as)
{
	return as < 16 && os->as[as].enabled;
}

int mock_gpu_translate_locked(struct pw_os *os, unsigned int as, uint64_t va, char access)
{
	return translate_locked(os, as, va, access, NULL, NULL, NULL);
}

/* GPU_TIMESTAMP counts the 24 MHz system counter: derived from the monotonic clock. */
uint64_t mock_gpu_timestamp_locked(struct pw_os *os)
{
	(void)os;
	return mono_now_ns() * 3u / 125u;
}

uint64_t mock_gpu_cycles_locked(struct pw_os *os)
{
	uint64_t v = os->jm_cycles_acc;

	if (os->jm_cycles_on)
		v += (mono_now_ns() - os->jm_cycles_since_ns) * 2u / 5u;
	return v;
}

static void jm_cycles_set_locked(struct pw_os *os, bool on)
{
	if (on == os->jm_cycles_on)
		return;
	if (on) {
		os->jm_cycles_since_ns = mono_now_ns();
		os->regs[R_GPU_STATUS / 4] |= 1u << 6;
	} else {
		os->jm_cycles_acc = mock_gpu_cycles_locked(os);
		os->regs[R_GPU_STATUS / 4] &= ~(1u << 6);
	}
	os->jm_cycles_on = on;
}

void mock_kick_locked(struct pw_os *os)
{
	pthread_cond_broadcast(&os->cond);
}

void mock_cond_wait_locked(struct pw_os *os, uint64_t timeout_ns)
{
	struct timespec ts;

	mono_timespec(&ts);
	ts_add_ns(&ts, timeout_ns);
	mono_cond_timedwait(&os->cond, &os->lock, &ts);
}

/* ------------------------------------------------------------------------------------------ */
/* emulated MCU                                                                               */
/* ------------------------------------------------------------------------------------------ */

static void mw32(struct pw_os *os, uint32_t off, uint32_t v)
{
	if (gpu_rw_locked(os, 0, SH_VA + off, &v, 4, true))
		model_error(os, "MCU write to shared+0x%x faulted", off);
}

static uint32_t mr32(struct pw_os *os, uint32_t off)
{
	uint32_t v = 0;

	if (gpu_rw_locked(os, 0, SH_VA + off, &v, 4, false))
		model_error(os, "MCU read from shared+0x%x faulted", off);
	return v;
}

static uint64_t mr64(struct pw_os *os, uint32_t off)
{
	return mr32(os, off) | ((uint64_t)mr32(os, off + 4) << 32);
}

uint32_t mock_sh_r32_locked(struct pw_os *os, uint32_t off)
{
	return mr32(os, off);
}

void mock_sh_w32_locked(struct pw_os *os, uint32_t off, uint32_t v)
{
	mw32(os, off, v);
}

static void raise_locked(struct pw_os *os, unsigned int line, uint32_t bits)
{
	os->rawstat[line] |= bits;
	pthread_cond_broadcast(&os->cond);
}

void mock_raise_job_locked(struct pw_os *os, uint32_t bits)
{
	raise_locked(os, 0, bits);
}

static uint64_t mono_ns(void)
{
	return mono_now_ns();
}

/* Completion events show up asynchronously, after the configured latency. */
static void raise_later_locked(struct pw_os *os, unsigned int line, uint32_t bits)
{
	if (os->cfg.instant_completion) {
		raise_locked(os, line, bits);
		return;
	}
	os->delayed_bits[line] |= bits;
	os->delayed_at[line] = mono_ns() + (uint64_t)os->cfg.completion_delay_us * 1000u;
	pthread_cond_broadcast(&os->cond);
}

static void apply_delayed_locked(struct pw_os *os)
{
	uint64_t now = mono_ns();

	for (unsigned int i = 0; i < 3; i++) {
		if (os->delayed_bits[i] && now >= os->delayed_at[i]) {
			raise_locked(os, i, os->delayed_bits[i]);
			os->delayed_bits[i] = 0;
		}
	}
}

static void mcu_boot_locked(struct pw_os *os)
{
	enum mock_fw_variant v = os->cfg.fw_variant;
	uint64_t pa;

	os->mcu_status = 1;	/* ENABLED */
	if (v == MOCK_FW_NEVER_BOOTS)
		return;

	/* The code section must be executable and read-only, the shared section writable. */
	if (translate_locked(os, 0, 0x800000, 'x', &pa, NULL, NULL)) {
		model_error(os, "MCU: code at 0x800000 not executable through AS0");
		if (!os->cfg.mcu_lenient) {
			os->mcu_status = 3;	/* FATAL */
			return;
		}
	}
	if (!translate_locked(os, 0, 0x800000, 'w', &pa, NULL, NULL))
		model_error(os, "MCU: code at 0x800000 is writable");
	for (uint32_t off = 0; off < SH_SIZE; off += 0x1000) {
		if (translate_locked(os, 0, SH_VA + off, 'w', &pa, NULL, NULL)) {
			model_error(os, "MCU: shared section page 0x%x not writable", SH_VA + off);
			if (!os->cfg.mcu_lenient) {
				os->mcu_status = 3;
				return;
			}
			break;
		}
	}

	/* Global control block (struct panthor_fw_global_control_iface). */
	mw32(os, 0x00, v == MOCK_FW_VERSION_ZERO ? 0 : MOCK_FW_IFACE_VERSION);
	mw32(os, 0x04, 0);				/* features */
	mw32(os, 0x08, v == MOCK_FW_BAD_INPUT_VA ? 0x08000000u : SH_VA + GLB_IN);
	mw32(os, 0x0c, SH_VA + GLB_OUT);
	mw32(os, 0x10, v == MOCK_FW_BAD_GROUP_NUM ? 2 : MOCK_FW_GROUP_NUM);
	mw32(os, 0x14, 0x100);				/* group_stride */
	mw32(os, 0x18, 0);				/* perfcnt_size */
	mw32(os, 0x1c, 0x71);				/* instr_features */
	for (uint32_t i = 0; i < 0x88; i += 4)
		mw32(os, GLB_IN + i, 0);
	for (uint32_t i = 0; i < 0x1c; i += 4)
		mw32(os, GLB_OUT + i, 0);

	for (uint32_t i = 0; i < MOCK_FW_GROUP_NUM; i++) {
		uint32_t c = CSG_CTRL(i);

		mw32(os, c + 0x00, 0);			/* features */
		mw32(os, c + 0x04, SH_VA + CSG_IN(i));
		mw32(os, c + 0x08, SH_VA + CSG_OUT(i));
		mw32(os, c + 0x0c, (v == MOCK_FW_MISMATCHED_CSG && i == 3) ? 0x4000 : 0x5000);
		mw32(os, c + 0x10, 0x5000);		/* protm_suspend_size */
		mw32(os, c + 0x14, v == MOCK_FW_BAD_STREAM_NUM ? 40 : MOCK_FW_STREAM_NUM);
		mw32(os, c + 0x18, 0xc);		/* stream_stride */
		for (uint32_t j = 0; j < MOCK_FW_STREAM_NUM; j++) {
			mw32(os, CS_CTRL(i, j) + 0, MOCK_FW_CS_FEATURES);
			mw32(os, CS_CTRL(i, j) + 4, SH_VA + CS_IN(i, j));
			mw32(os, CS_CTRL(i, j) + 8, SH_VA + CS_OUT(i, j));
		}
	}

	mock_csf_reset_locked(os);
	os->mcu_running = true;
	raise_later_locked(os, 0, JOB_GLOBAL_IF);
}

static void mcu_glb_doorbell_locked(struct pw_os *os)
{
	uint32_t req, ack, new_ack, irq_mask, db, csg_mask;

	os->glb.doorbells++;
	if (!os->mcu_running || os->cfg.fw_variant == MOCK_FW_IGNORE_DOORBELL)
		return;

	req = mr32(os, GLB_IN + 0x00);
	irq_mask = mr32(os, GLB_IN + 0x04);
	ack = mr32(os, GLB_OUT + 0x00);

	os->glb.valid = true;
	os->glb.req = req;
	os->glb.ack_irq_mask = irq_mask;
	os->glb.progress_timer = mr32(os, GLB_IN + 0x10);
	os->glb.poweroff_timer = mr32(os, GLB_IN + 0x14);
	os->glb.core_en_mask = mr64(os, GLB_IN + 0x18);
	os->glb.idle_timer = mr32(os, GLB_IN + 0x80);

	/* Echo request bits (toggles and config); HALT is reported via MCU_STATUS instead. */
	new_ack = (ack & ~0x7ffu) | (req & 0x7ffu & ~1u);
	if (req & 1u)
		os->mcu_status = 2;	/* HALT */
	mw32(os, GLB_OUT + 0x00, new_ack);
	/* doorbell_ack = doorbell_req; the toggled bits are CSG doorbells. */
	db = mr32(os, GLB_IN + 0x08);
	csg_mask = (db ^ mr32(os, GLB_OUT + 0x08)) & ((1u << MOCK_FW_GROUP_NUM) - 1u);
	mw32(os, GLB_OUT + 0x08, db);
	if ((new_ack ^ ack) & irq_mask)
		raise_locked(os, 0, JOB_GLOBAL_IF);
	if (csg_mask)
		mock_csf_glb_doorbell_locked(os, csg_mask);
}

void mock_mcu_raise_glb_event(struct pw_os *os, uint32_t bits)
{
	pthread_mutex_lock(&os->lock);
	mw32(os, GLB_OUT, mr32(os, GLB_OUT) ^ bits);
	raise_locked(os, 0, JOB_GLOBAL_IF);
	pthread_mutex_unlock(&os->lock);
}

uint32_t mock_mcu_glb_req(struct pw_os *os)
{
	uint32_t v;

	pthread_mutex_lock(&os->lock);
	v = mr32(os, GLB_IN);
	pthread_mutex_unlock(&os->lock);
	return v;
}

uint32_t mock_mcu_glb_ack(struct pw_os *os)
{
	uint32_t v;

	pthread_mutex_lock(&os->lock);
	v = mr32(os, GLB_OUT);
	pthread_mutex_unlock(&os->lock);
	return v;
}

/* ------------------------------------------------------------------------------------------ */
/* register file                                                                              */
/* ------------------------------------------------------------------------------------------ */

static int int_line(uint32_t off, uint32_t *sub)
{
	for (int i = 0; i < 3; i++) {
		if (off >= int_base[i] && off < int_base[i] + 0x10) {
			*sub = off - int_base[i];
			return i;
		}
	}
	return -1;
}

static uint64_t reg64_locked(struct pw_os *os, uint32_t off)
{
	return os->regs[off / 4] | ((uint64_t)os->regs[off / 4 + 1] << 32);
}

static void soft_reset_locked(struct pw_os *os)
{
	os->soft_resets++;
	if (os->cfg.no_reset_complete)
		return;
	os->l2_ready = 0;
	os->l2_pending = 0;
	os->l2_trans_reads = 0;
	memset(os->as, 0, sizeof(os->as));
	memset(os->as_busy, 0, sizeof(os->as_busy));
	os->mcu_running = false;
	os->mcu_status = 0;
	if (is_jm(os)) {
		os->regs[R_SHADER_READY / 4] = os->regs[R_SHADER_READY / 4 + 1] = 0;
		os->regs[R_TILER_READY / 4] = os->regs[R_TILER_READY / 4 + 1] = 0;
		os->regs[R_STACK_READY / 4] = os->regs[R_STACK_READY / 4 + 1] = 0;
		/* (jm-hw) a reset drops pending job/MMU interrupts and stops the cycle counter */
		os->rawstat[0] = 0;
		os->rawstat[1] = 0;
		jm_cycles_set_locked(os, false);
		mock_jm_reset_locked(os);
	} else {
		mock_csf_reset_locked(os);
	}
	memset(os->delayed_bits, 0, sizeof(os->delayed_bits));
	raise_later_locked(os, 2, GPU_IRQ_RESET_COMPLETED);
}

static void as_command_locked(struct pw_os *os, unsigned int as, uint32_t cmd)
{
	struct mock_as_state *st = &os->as[as];
	uint32_t base = R_AS(as);

	if (os->as_busy[as])
		model_error(os, "AS%u: command %u issued while AS_ACTIVE", as, cmd);
	os->as_busy[as] = os->cfg.as_busy_reads;

	switch (cmd) {
	case 0:		/* NOP */
		break;
	case 1:		/* UPDATE */
		st->transtab = reg64_locked(os, base + R_AS_TRANSTAB);
		st->memattr = reg64_locked(os, base + R_AS_MEMATTR);
		st->transcfg = reg64_locked(os, base + R_AS_TRANSCFG);
		st->enabled = (st->transcfg & 0xf) != 1;
		st->updates++;
		break;
	case 2:		/* LOCK */
		if (st->locked)
			model_error(os, "AS%u: LOCK while locked", as);
		st->locked = true;
		st->lockaddr = reg64_locked(os, base + R_AS_LOCKADDR);
		os->flushes_at_lock[as] = os->flushes;
		st->locks++;
		break;
	case 3:		/* UNLOCK */
		if (!st->locked)
			model_error(os, "AS%u: UNLOCK while not locked", as);
		if (os->flushes == os->flushes_at_lock[as])
			model_error(os, "AS%u: UNLOCK without a GPU cache flush since LOCK", as);
		st->locked = false;
		st->unlocks++;
		break;
	case 4:		/* FLUSH_PT */
	case 5:		/* FLUSH_MEM */
		/* (jm-mem) Bifrost JM: the flush commands flush the L2 and the walk caches for
		 * the locked region and end the lock (Linux panfrost never sends UNLOCK after
		 * them). The G610 model only counts them. */
		if (cmd == 4)
			st->flush_pts++;
		else
			st->flush_mems++;
		if (os->cfg.model == MOCK_MODEL_G31)
			st->locked = false;
		break;
	default:
		model_error(os, "AS%u: bad command %u", as, cmd);
	}
	raise_locked(os, 1, 1u << (16 + as));	/* REQ_COMPLETE, must stay masked */
	if (is_jm(os))
		mock_jm_as_command_locked(os, as, cmd);	/* (jm-hw) retry/kill stalled jobs */
}

/*
 * (jm-arch) GPU_CONTROL writes that mean something else on the G31: GPU_CMD encodings, shader /
 * tiler / stack power (READY follows PWRON at once: the core polls READY), no MCU. Returns true
 * if handled. L2 power, AS and interrupt registers use the shared model.
 */
static bool jm_gpu_reg_write_locked(struct pw_os *os, uint32_t off, uint32_t v)
{
	switch (off) {
	case R_GPU_CMD:
		switch (v) {
		case 0x00:		/* NOP */
			break;
		case 0x01:		/* SOFT_RESET */
		case 0x02:		/* HARD_RESET */
			soft_reset_locked(os);
			break;
		case 0x05:		/* CYCLE_COUNT_START */
			jm_cycles_set_locked(os, true);
			break;
		case 0x06:		/* CYCLE_COUNT_STOP */
			jm_cycles_set_locked(os, false);
			break;
		case 0x07:		/* CLEAN_CACHES */
		case 0x08:		/* CLEAN_INV_CACHES */
			os->flushes++;
			os->last_flush = v;
			os->regs[R_LATEST_FLUSH_ID / 4]++;	/* (jm-hw) */
			raise_later_locked(os, 2, GPU_IRQ_CLEAN_CACHES);
			break;
		default:
			model_error(os, "G31: unexpected GPU_CMD 0x%x", v);
		}
		return true;
	case R_SHADER_PWRON:
	case R_TILER_PWRON:
		/* (jm-hw) power-up timeout knobs */
		if (!(off == R_SHADER_PWRON ? os->cfg.no_shader_ready : os->cfg.no_tiler_ready))
			os->regs[(off - 0x40) / 4] |= v;	/* *_READY is PWRON - 0x40 */
		raise_locked(os, 2, GPU_IRQ_POWER_CHANGED_BOTH);
		return true;
	case R_STACK_PWRON:
		os->regs[R_STACK_READY / 4] |= v;
		raise_locked(os, 2, GPU_IRQ_POWER_CHANGED_BOTH);
		return true;
	case R_SHADER_PWROFF:
	case R_TILER_PWROFF:
		os->regs[(off - 0x80) / 4] &= ~v;	/* *_READY is PWROFF - 0x80 */
		raise_locked(os, 2, GPU_IRQ_POWER_CHANGED_BOTH);
		return true;
	case R_STACK_PWROFF:
		os->regs[R_STACK_READY / 4] &= ~v;
		raise_locked(os, 2, GPU_IRQ_POWER_CHANGED_BOTH);
		return true;
	case R_SHADER_PWRON + 4:
	case R_TILER_PWRON + 4:
	case R_SHADER_PWROFF + 4:
	case R_TILER_PWROFF + 4:
		return true;
	case R_MCU_CONTROL:
		model_error(os, "G31: MCU_CONTROL write (no MCU on a job-manager GPU)");
		return true;
	default:
		return false;
	}
}

uint32_t pw_os_reg_read32(struct pw_os *os, uint32_t off)
{
	uint32_t v, sub;
	int line;

	pthread_mutex_lock(&os->lock);
	apply_delayed_locked(os);
	if (off >= REG_SPACE || (off & 3)) {
		model_error(os, "bad register read at 0x%x", off);
		pthread_mutex_unlock(&os->lock);
		return 0;
	}
	if (is_jm(os) && off >= G31_REG_WINDOW) {
		model_error(os, "G31: register read at 0x%x, outside the 256 KiB window", off);
		pthread_mutex_unlock(&os->lock);
		return 0;
	}
	line = int_line(off, &sub);
	if (is_jm(os) && line < 0 && off >= R_JOB_CONTROL_EXTRA && off < R_JOB_CONTROL_END &&
	    mock_jm_reg_read_locked(os, off, &v)) {
		pthread_mutex_unlock(&os->lock);
		return v;
	}
	/* (jm-hw) G31 free-running counters */
	if (is_jm(os) && off >= R_CYCLE_COUNT && off < R_TIMESTAMP + 8) {
		uint64_t c = off >= R_TIMESTAMP ? mock_gpu_timestamp_locked(os) :
						  mock_gpu_cycles_locked(os);

		v = (off & 4) ? (uint32_t)(c >> 32) : (uint32_t)c;
		pthread_mutex_unlock(&os->lock);
		return v;
	}
	if (line >= 0) {
		switch (sub) {
		case 0x0:
			v = os->rawstat[line];
			if (line == 2 && os->cfg.hide_completions_from_waiters && !tls_in_irq_thread)
				v &= ~(GPU_IRQ_RESET_COMPLETED | GPU_IRQ_CLEAN_CACHES);
			break;
		case 0x8:
			v = os->mask[line];
			break;
		case 0xc:
			v = os->rawstat[line] & os->mask[line];
			break;
		default:
			v = 0;
		}
	} else if (off == R_L2_PWRTRANS || off == R_L2_PWRTRANS + 4) {
		v = 0;
		if (off == R_L2_PWRTRANS) {
			if (os->l2_trans_reads > 0) {
				os->l2_trans_reads--;
				v = (uint32_t)os->l2_pending;
			} else if (os->l2_pending && !os->cfg.no_l2_ready) {
				os->l2_ready |= os->l2_pending;
				os->l2_pending = 0;
				raise_locked(os, 2, GPU_IRQ_POWER_CHANGED_BOTH);
			}
		}
	} else if (off == R_L2_READY || off == R_L2_READY + 4) {
		if (off == R_L2_READY && os->l2_pending && !os->l2_trans_reads &&
		    !os->cfg.no_l2_ready) {
			os->l2_ready |= os->l2_pending;
			os->l2_pending = 0;
			raise_locked(os, 2, GPU_IRQ_POWER_CHANGED_BOTH);
		} else if (off == R_L2_READY && os->l2_trans_reads > 0) {
			os->l2_trans_reads--;
		}
		v = off == R_L2_READY ? (uint32_t)os->l2_ready : (uint32_t)(os->l2_ready >> 32);
	} else if (off >= R_AS(0) && off < R_AS(16) && ((off - R_AS(0)) & 0x3f) == R_AS_STATUS) {
		unsigned int as = (off - R_AS(0)) >> 6;

		v = 0;
		if (os->as_busy[as]) {
			os->as_busy[as]--;
			v = 1;
		}
	} else if (off == R_MCU_STATUS) {
		v = os->mcu_status;
	} else if (off == R_GPU_ID && os->cfg.gpu_id_zero) {
		v = 0;
	} else {
		v = os->regs[off / 4];
	}
	pthread_mutex_unlock(&os->lock);
	return v;
}

void pw_os_reg_write32(struct pw_os *os, uint32_t off, uint32_t v)
{
	uint32_t sub;
	int line;

	pthread_mutex_lock(&os->lock);
	apply_delayed_locked(os);
	if (off >= REG_SPACE || (off & 3)) {
		model_error(os, "bad register write at 0x%x", off);
		goto out;
	}
	if (is_jm(os) && off >= G31_REG_WINDOW) {
		model_error(os, "G31: register write at 0x%x, outside the 256 KiB window", off);
		goto out;
	}
	line = int_line(off, &sub);
	if (is_jm(os) && line < 0 && off >= R_JOB_CONTROL_EXTRA && off < R_JOB_CONTROL_END) {
		if (!mock_jm_reg_write_locked(os, off, v))
			os->regs[off / 4] = v;
		goto out;
	}
	if (is_jm(os) && jm_gpu_reg_write_locked(os, off, v))
		goto out;
	if (line >= 0) {
		if (sub == 0x4)
			os->rawstat[line] &= ~v;
		else if (sub == 0x8)
			os->mask[line] = v;
		/* (jm-hw) clears release a halted job slot / a job stalled on a page fault */
		if (is_jm(os) && sub == 0x4 && line == 0)
			mock_jm_int_clear_locked(os, v);
		if (is_jm(os) && sub == 0x4 && line == 1)
			mock_jm_mmu_clear_locked(os, v);
		pthread_cond_broadcast(&os->cond);
		goto out;
	}
	if (off >= R_DOORBELL0 && ((off - R_DOORBELL0) & 0xffff) == 0) {
		if (off == R_DOORBELL0)
			mcu_glb_doorbell_locked(os);
		else if (os->mcu_running)
			mock_csf_doorbell_locked(os, (off - R_DOORBELL0) >> 16);
		goto out;
	}
	if (off >= R_AS(0) && off < R_AS(16)) {
		unsigned int as = (off - R_AS(0)) >> 6;

		os->regs[off / 4] = v;
		if (((off - R_AS(0)) & 0x3f) == R_AS_COMMAND)
			as_command_locked(os, as, v);
		goto out;
	}
	switch (off) {
	case R_GPU_CMD:
		if (v == 0x101) {
			soft_reset_locked(os);
		} else if ((v & 0xff) == 4) {
			os->flushes++;
			os->last_flush = v;
			raise_later_locked(os, 2, GPU_IRQ_CLEAN_CACHES);
		} else {
			model_error(os, "unexpected GPU_CMD 0x%x", v);
		}
		break;
	case R_L2_PWRON:
		os->l2_pending |= v;
		os->l2_trans_reads = 3;
		break;
	case R_L2_PWROFF:
		os->l2_ready &= ~(uint64_t)v;
		break;
	case R_L2_PWRON + 4:
	case R_L2_PWROFF + 4:
		break;
	case R_MCU_CONTROL:
		if (v == 2 || v == 1) {
			mcu_boot_locked(os);
		} else if (v == 0) {
			os->mcu_running = false;
			os->mcu_status = 0;
			mock_csf_reset_locked(os);
		}
		break;
	default:
		os->regs[off / 4] = v;
	}
out:
	pthread_mutex_unlock(&os->lock);
}

/* ------------------------------------------------------------------------------------------ */
/* interrupts                                                                                 */
/* ------------------------------------------------------------------------------------------ */

static void *irq_thread_fn(void *arg)
{
	struct irq_thread *t = arg;
	struct pw_os *os = t->os;
	unsigned int line = t->line;

	tls_in_irq_thread = true;

	pthread_mutex_lock(&os->lock);
	while (os->running) {
		apply_delayed_locked(os);
		if (os->dev && (os->rawstat[line] & os->mask[line])) {
			struct pw_device *dev = os->dev;
			bool mine;

			os->in_flight++;
			pthread_mutex_unlock(&os->lock);
			mine = pw_irq_top(dev, (enum pw_irq_line)line);
			if (mine)
				pw_irq_bottom(dev, (enum pw_irq_line)line);
			pthread_mutex_lock(&os->lock);
			os->in_flight--;
			if (mine) {
				os->delivered[line]++;
			} else if (os->rawstat[line] & os->mask[line]) {
				struct timespec ts;

				os->unclaimed++;
				mono_timespec(&ts);
				ts_add_ns(&ts, 1000000);
				mono_cond_timedwait(&os->cond, &os->lock, &ts);
			}
			pthread_cond_broadcast(&os->cond);
			continue;
		}
		struct timespec ts;
		uint64_t wait_ns = 10000000, now = mono_ns();

		for (unsigned int i = 0; i < 3; i++) {
			if (os->delayed_bits[i])
				wait_ns = os->delayed_at[i] > now ?
					  PW_MIN_U64(wait_ns, os->delayed_at[i] - now) : 0;
		}
		if (!wait_ns)
			continue;
		mono_timespec(&ts);
		ts_add_ns(&ts, wait_ns);
		mono_cond_timedwait(&os->cond, &os->lock, &ts);
	}
	pthread_mutex_unlock(&os->lock);
	return NULL;
}

void mock_attach_irq(struct pw_os *os, struct pw_device *dev)
{
	pthread_mutex_lock(&os->lock);
	os->dev = dev;
	pthread_cond_broadcast(&os->cond);
	pthread_mutex_unlock(&os->lock);
}

void mock_detach_irq(struct pw_os *os)
{
	pthread_mutex_lock(&os->lock);
	os->dev = NULL;
	while (os->in_flight)
		pthread_cond_wait(&os->cond, &os->lock);
	pthread_mutex_unlock(&os->lock);
}

uint32_t mock_irq_unclaimed(struct pw_os *os)
{
	uint32_t v;

	pthread_mutex_lock(&os->lock);
	v = os->unclaimed;
	pthread_mutex_unlock(&os->lock);
	return v;
}

uint32_t mock_irq_delivered(struct pw_os *os, unsigned int line)
{
	uint32_t v;

	pthread_mutex_lock(&os->lock);
	v = os->delivered[line];
	pthread_mutex_unlock(&os->lock);
	return v;
}

/* ------------------------------------------------------------------------------------------ */
/* test accessors                                                                             */
/* ------------------------------------------------------------------------------------------ */

uint32_t mock_reg(struct pw_os *os, uint32_t off)
{
	uint32_t v;

	pthread_mutex_lock(&os->lock);
	v = os->regs[off / 4];
	pthread_mutex_unlock(&os->lock);
	return v;
}

uint32_t mock_rawstat(struct pw_os *os, unsigned int line)
{
	uint32_t v;

	pthread_mutex_lock(&os->lock);
	v = os->rawstat[line];
	pthread_mutex_unlock(&os->lock);
	return v;
}

uint32_t mock_int_mask(struct pw_os *os, unsigned int line)
{
	uint32_t v;

	pthread_mutex_lock(&os->lock);
	v = os->mask[line];
	pthread_mutex_unlock(&os->lock);
	return v;
}

void mock_fail_mem_allocs(struct pw_os *os, uint32_t count)
{
	pthread_mutex_lock(&os->lock);
	os->fail_mem_allocs = count;
	pthread_mutex_unlock(&os->lock);
}

void mock_get_as(struct pw_os *os, unsigned int as, struct mock_as_state *st)
{
	pthread_mutex_lock(&os->lock);
	*st = os->as[as];
	pthread_mutex_unlock(&os->lock);
}

uint32_t mock_flush_count(struct pw_os *os)
{
	uint32_t v;

	pthread_mutex_lock(&os->lock);
	v = os->flushes;
	pthread_mutex_unlock(&os->lock);
	return v;
}

uint32_t mock_last_flush_cmd(struct pw_os *os)
{
	uint32_t v;

	pthread_mutex_lock(&os->lock);
	v = os->last_flush;
	pthread_mutex_unlock(&os->lock);
	return v;
}

uint32_t mock_soft_resets(struct pw_os *os)
{
	uint32_t v;

	pthread_mutex_lock(&os->lock);
	v = os->soft_resets;
	pthread_mutex_unlock(&os->lock);
	return v;
}

uint32_t mock_model_errors(struct pw_os *os)
{
	uint32_t v;

	pthread_mutex_lock(&os->lock);
	v = os->model_errors;
	pthread_mutex_unlock(&os->lock);
	return v;
}

uint32_t mock_mcu_status(struct pw_os *os)
{
	uint32_t v;

	pthread_mutex_lock(&os->lock);
	v = os->mcu_status;
	pthread_mutex_unlock(&os->lock);
	return v;
}


void mock_glb_snapshot(struct pw_os *os, struct mock_glb_snapshot *s)
{
	pthread_mutex_lock(&os->lock);
	*s = os->glb;
	pthread_mutex_unlock(&os->lock);
}

void mock_raise_mmu_fault_locked(struct pw_os *os, unsigned int as, uint32_t fault_status,
				 uint64_t va)
{
	os->regs[(R_AS(as) + R_AS_FAULTSTATUS) / 4] = fault_status;
	os->regs[(R_AS(as) + R_AS_FAULTADDRESS) / 4] = (uint32_t)va;
	os->regs[(R_AS(as) + R_AS_FAULTADDRESS + 4) / 4] = (uint32_t)(va >> 32);
	raise_locked(os, 1, 1u << as);
}

void mock_inject_mmu_fault(struct pw_os *os, unsigned int as, uint32_t fault_status, uint64_t va)
{
	pthread_mutex_lock(&os->lock);
	mock_raise_mmu_fault_locked(os, as, fault_status, va);
	pthread_mutex_unlock(&os->lock);
}

void mock_inject_gpu_fault(struct pw_os *os, uint32_t fault_status, uint64_t addr)
{
	pthread_mutex_lock(&os->lock);
	os->regs[R_GPU_FAULT_STATUS / 4] = fault_status;
	os->regs[R_GPU_FAULT_ADDR / 4] = (uint32_t)addr;
	os->regs[R_GPU_FAULT_ADDR / 4 + 1] = (uint32_t)(addr >> 32);
	raise_locked(os, 2, GPU_IRQ_FAULT);
	pthread_mutex_unlock(&os->lock);
}

void mock_raise_mmu_irq_bits(struct pw_os *os, uint32_t bits)
{
	pthread_mutex_lock(&os->lock);
	raise_locked(os, 1, bits);
	pthread_mutex_unlock(&os->lock);
}

/* ------------------------------------------------------------------------------------------ */
/* lifetime                                                                                   */
/* ------------------------------------------------------------------------------------------ */

static void set64(struct pw_os *os, uint32_t off, uint64_t v)
{
	os->regs[off / 4] = (uint32_t)v;
	os->regs[off / 4 + 1] = (uint32_t)(v >> 32);
}

/* (jm-arch) G31 MP2 register values (mock_gpu.h says which are guesses). */
static void init_g31_regs(struct pw_os *os)
{
	os->regs[0x4 / 4] = MOCK_G31_L2_FEATURES;
	os->regs[0x8 / 4] = MOCK_G31_CORE_FEATURES;
	os->regs[0xc / 4] = MOCK_G31_TILER_FEATURES;
	os->regs[0x10 / 4] = MOCK_G31_MEM_FEATURES;
	os->regs[0x14 / 4] = MOCK_G31_MMU_FEATURES;
	os->regs[0x18 / 4] = MOCK_G31_AS_PRESENT;
	os->regs[R_JS_PRESENT / 4] = MOCK_G31_JS_PRESENT;
	os->regs[R_AFBC_FEATURES / 4] = MOCK_G31_AFBC_FEATURES;
	os->regs[0xa0 / 4] = MOCK_G31_MAX_THREADS;
	os->regs[0xa4 / 4] = MOCK_G31_MAX_WG_SIZE;
	os->regs[0xa8 / 4] = MOCK_G31_MAX_BARRIER;
	os->regs[0xac / 4] = MOCK_G31_THREAD_FEATURES;
	os->regs[0xb0 / 4] = MOCK_G31_TEXTURE_FEATURES0;
	os->regs[R_JS_FEATURES(0) / 4] = MOCK_G31_JS0_FEATURES;
	os->regs[R_JS_FEATURES(1) / 4] = MOCK_G31_JS1_FEATURES;
	os->regs[R_JS_FEATURES(2) / 4] = MOCK_G31_JS2_FEATURES;
	set64(os, 0x100, MOCK_G31_SHADER_PRESENT);
	set64(os, 0x110, MOCK_G31_TILER_PRESENT);
	set64(os, 0x120, MOCK_G31_L2_PRESENT);
	os->regs[0x300 / 4] = MOCK_G31_COHERENCY_FEATURES;
	os->regs[R_THREAD_TLS_ALLOC / 4] = MOCK_G31_THREAD_TLS_ALLOC;
	set64(os, R_STACK_PRESENT, MOCK_G31_STACK_PRESENT);
}

struct pw_os *mock_create(const struct mock_config *cfg)
{
	struct pw_os *os = calloc(1, sizeof(*os));

	if (!os)
		abort();
	if (cfg)
		os->cfg = *cfg;
	if (!os->cfg.gpu_id)
		os->cfg.gpu_id = is_jm(os) ? MOCK_G31_GPU_ID : MOCK_G610_GPU_ID;
	if (!os->cfg.as_busy_reads)
		os->cfg.as_busy_reads = 2;
	if (!os->cfg.completion_delay_us)
		os->cfg.completion_delay_us = 100;
	os->pa_base = os->cfg.pa_base ? os->cfg.pa_base : MOCK_PA_BASE;
	os->verbose = getenv("PW_TEST_VERBOSE") != NULL;
	os->regs = calloc(REG_SPACE / 4, sizeof(uint32_t));
	os->pa_pages = calloc(PA_NPAGES, sizeof(*os->pa_pages));
	os->csf = mock_csf_create();
	os->jm = mock_jm_create();
	if (!os->regs || !os->pa_pages || !os->csf || !os->jm)
		abort();

	pthread_mutex_init(&os->lock, NULL);
	mono_cond_init(&os->cond);
	if (is_jm(os))
		mock_jm_start(os);	/* (jm-hw) job-chain executor thread */

	os->regs[0x0 / 4] = os->cfg.gpu_id;
	os->regs[0x4 / 4] = MOCK_G610_L2_FEATURES;
	os->regs[0x8 / 4] = 0;				/* core features (variant) */
	os->regs[0xc / 4] = MOCK_G610_TILER_FEATURES;
	os->regs[0x10 / 4] = MOCK_G610_MEM_FEATURES;
	os->regs[0x14 / 4] = MOCK_G610_MMU_FEATURES;
	os->regs[0x18 / 4] = MOCK_G610_AS_PRESENT;
	os->regs[0x1c / 4] = MOCK_G610_CSF_ID;
	os->regs[0xa0 / 4] = MOCK_G610_MAX_THREADS;
	os->regs[0xa4 / 4] = MOCK_G610_MAX_WG_SIZE;
	os->regs[0xa8 / 4] = MOCK_G610_MAX_BARRIER;
	os->regs[0xac / 4] = MOCK_G610_THREAD_FEATURES;
	os->regs[0xb0 / 4] = MOCK_G610_TEXTURE_FEATURES0;
	set64(os, 0x100, MOCK_G610_SHADER_PRESENT);
	set64(os, 0x110, MOCK_G610_TILER_PRESENT);
	set64(os, 0x120, MOCK_G610_L2_PRESENT);
	os->regs[0x300 / 4] = 0;			/* coherency features (G610 dump) */
	if (is_jm(os))
		init_g31_regs(os);
	if (os->cfg.as_present)
		os->regs[0x18 / 4] = os->cfg.as_present;

	os->running = true;
	for (unsigned int i = 0; i < 3; i++) {
		os->thr[i].os = os;
		os->thr[i].line = i;
		if (pthread_create(&os->thr[i].thr, NULL, irq_thread_fn, &os->thr[i]))
			abort();
	}
	return os;
}

void mock_destroy(struct pw_os *os)
{
	mock_jm_stop(os);	/* (jm-hw) no-op unless the G31 executor runs */
	pthread_mutex_lock(&os->lock);
	os->dev = NULL;
	os->running = false;
	pthread_cond_broadcast(&os->cond);
	pthread_mutex_unlock(&os->lock);
	for (unsigned int i = 0; i < 3; i++)
		pthread_join(os->thr[i].thr, NULL);
	pthread_cond_destroy(&os->cond);
	pthread_mutex_destroy(&os->lock);
	free(os->fw_buf);
	free(os->log);
	free(os->regs);
	free(os->pa_pages);
	mock_csf_destroy(os->csf);
	mock_jm_destroy(os->jm);
	free(os);
}

/* ---- M3+ additions: user waits, user copies, deferred work ---- */

int pw_os_event_wait_user(struct pw_os_event *e, uint64_t deadline_ns)
{
	struct timespec ts;
	int ret = PW_OK;

	ts.tv_sec = (time_t)(deadline_ns / 1000000000u);
	ts.tv_nsec = (long)(deadline_ns % 1000000000u);
	pthread_mutex_lock(&e->m);
	while (!e->set) {
		if (deadline_ns == UINT64_MAX) {
			pthread_cond_wait(&e->c, &e->m);
		} else if (mono_cond_timedwait(&e->c, &e->m, &ts) == ETIMEDOUT) {
			ret = e->set ? PW_OK : PW_ETIMEDOUT;
			break;
		}
	}
	pthread_mutex_unlock(&e->m);
	return ret;
}

/* The host tests run the "user" and the "kernel" in one address space: user pointers are host
 * pointers. NULL with a non-zero size is the one fault we can model. */
int pw_os_copy_from_user(struct pw_os *os, void *dst, uint64_t user_ptr, size_t size)
{
	(void)os;
	if (!size)
		return PW_OK;
	if (!user_ptr)
		return PW_EFAULT;
	memcpy(dst, (const void *)(uintptr_t)user_ptr, size);
	return PW_OK;
}

int pw_os_copy_to_user(struct pw_os *os, uint64_t user_ptr, const void *src, size_t size)
{
	(void)os;
	if (!size)
		return PW_OK;
	if (!user_ptr)
		return PW_EFAULT;
	memcpy((void *)(uintptr_t)user_ptr, src, size);
	return PW_OK;
}

/* One worker thread per work item: simple, and models "may run concurrently with other work". */
struct pw_os_work {
	struct pw_os *os;
	pw_os_work_fn fn;
	void *ctx;
	pthread_t thr;
	pthread_mutex_t m;
	pthread_cond_t c;
	bool pending, running, stop;
	uint64_t not_before_ns;	/* for delayed queueing; 0 = now */
};

static void *work_thread(void *arg)
{
	struct pw_os_work *w = arg;

	pthread_mutex_lock(&w->m);
	for (;;) {
		while (!w->stop && !w->pending)
			pthread_cond_wait(&w->c, &w->m);
		if (w->stop && !w->pending)
			break;
		if (w->not_before_ns) {
			uint64_t now = pw_os_time_ns(w->os);

			if (now < w->not_before_ns && !w->stop) {
				struct timespec ts = {
					.tv_sec = (time_t)(w->not_before_ns / 1000000000u),
					.tv_nsec = (long)(w->not_before_ns % 1000000000u),
				};
				mono_cond_timedwait(&w->c, &w->m, &ts);
				continue;
			}
			if (w->stop) {	/* destroy cancels a pending delayed run */
				w->pending = false;
				break;
			}
		}
		w->pending = false;
		w->not_before_ns = 0;
		w->running = true;
		pthread_mutex_unlock(&w->m);
		w->fn(w->ctx);
		pthread_mutex_lock(&w->m);
		w->running = false;
		pthread_cond_broadcast(&w->c);
	}
	pthread_cond_broadcast(&w->c);
	pthread_mutex_unlock(&w->m);
	return NULL;
}

struct pw_os_work *pw_os_work_create(struct pw_os *os, pw_os_work_fn fn, void *ctx)
{
	struct pw_os_work *w = calloc(1, sizeof(*w));

	if (!w)
		return NULL;
	w->os = os;
	w->fn = fn;
	w->ctx = ctx;
	pthread_mutex_init(&w->m, NULL);
	mono_cond_init(&w->c);
	if (pthread_create(&w->thr, NULL, work_thread, w)) {
		pthread_cond_destroy(&w->c);
		pthread_mutex_destroy(&w->m);
		free(w);
		return NULL;
	}
	__atomic_fetch_add(&os->syncs, 1, __ATOMIC_RELAXED);
	return w;
}

void pw_os_work_queue(struct pw_os_work *w)
{
	pthread_mutex_lock(&w->m);
	w->pending = true;
	w->not_before_ns = 0;
	pthread_cond_broadcast(&w->c);
	pthread_mutex_unlock(&w->m);
}

void pw_os_work_queue_delayed(struct pw_os_work *w, uint32_t delay_ms)
{
	pthread_mutex_lock(&w->m);
	if (!w->pending) {
		w->pending = true;
		w->not_before_ns = pw_os_time_ns(w->os) + (uint64_t)delay_ms * 1000000u;
		pthread_cond_broadcast(&w->c);
	}
	pthread_mutex_unlock(&w->m);
}

void pw_os_work_flush(struct pw_os_work *w)
{
	pthread_mutex_lock(&w->m);
	if (w->pending && w->not_before_ns)
		w->not_before_ns = 1;	/* flush runs a pending delayed item now */
	pthread_cond_broadcast(&w->c);
	while (w->pending || w->running)
		pthread_cond_wait(&w->c, &w->m);
	pthread_mutex_unlock(&w->m);
}

void pw_os_work_destroy(struct pw_os *os, struct pw_os_work *w)
{
	if (!w)
		return;
	pthread_mutex_lock(&w->m);
	if (w->pending && w->not_before_ns)
		w->pending = false;	/* cancel a delayed run */
	w->stop = true;
	pthread_cond_broadcast(&w->c);
	pthread_mutex_unlock(&w->m);
	pthread_join(w->thr, NULL);
	pthread_cond_destroy(&w->c);
	pthread_mutex_destroy(&w->m);
	free(w);
	__atomic_fetch_sub(&os->syncs, 1, __ATOMIC_RELAXED);
}
