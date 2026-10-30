// SPDX-License-Identifier: MIT
/*
 * mock_jm.c - job-manager executor of the mock G31 (mock_config.model = MOCK_MODEL_G31).
 * Owner: jm-hw. pw_os_mock.c routes every JOB_CONTROL access above the JOB_INT block
 * (0x1010-0x1fff: JS_STATE, THROTTLE and the JS_* slot registers) here, under the mock lock,
 * plus JOB_INT/MMU_INT clears, AS commands and GPU resets of the G31 model.
 *
 * Model (behaviour from the public Mali job-manager register map, cross-checked against how
 * Linux panfrost drives it; job descriptors from Mesa src/panfrost/genxml/v7.xml, MIT):
 *  - Per slot: the current job chain and one queued in the NEXT registers. JS_COMMAND_NEXT =
 *    START latches HEAD/AFFINITY/CONFIG/FLUSH_ID_NEXT; JS_COMMAND_NEXT reads START until the
 *    chain moves to the current slot, NOP afterwards; writing NOP drops it. A chain moves to
 *    current when the slot is idle and no error interrupt of the slot is pending (the hardware
 *    holds NEXT back until JOB_INT_CLEAR of the ERR bit; after a DONE it starts at once).
 *  - An executor thread walks the current chain through the AS in JS_CONFIG (the GPU view of
 *    the mock page tables): Job Header (exception status, type, barrier, index, dependency 1/2,
 *    next) per v7.xml. NULL and CACHE_FLUSH do nothing, WRITE_VALUE writes its value
 *    (IMMEDIATE_8/16/32/64, ZERO, SYSTEM_TIMESTAMP, CYCLE_COUNTER), COMPUTE/VERTEX/GEOMETRY/
 *    TILER/FUSED/FRAGMENT/INDEXED_VERTEX complete without running shaders (counted, logged).
 *    Each finished job gets exception status DONE (1) written back unless
 *    DISABLE_DESCRIPTOR_WR_BK. Then JS_STATUS = DONE and JOB_INT DONE(js).
 *  - Dependencies: chains run in next-pointer order. A dependency on an index that has run in
 *    this chain is satisfied; on one that appears in the chain but has not run yet it would
 *    deadlock real hardware, so it is a JOB_CONFIG_FAULT here; an index not in the chain at all
 *    counts as satisfied (resumed chains after a soft stop).
 *  - Faults fail the chain: JS_STATUS = the exception code, JS_HEAD = JS_TAIL = the job, the
 *    header's exception status and fault pointer written when possible, JOB_INT ERR(js).
 *    Invalid descriptors (type 0 / > 10, Is 64b clear, unaligned, a type JS_FEATURES of the
 *    slot does not list, a bad WRITE_VALUE type, a chain longer than MOCK_JM_MAX_JOBS),
 *    a present-check of the AS and an affinity outside SHADER_PRESENT (JOB_AFFINITY_FAULT).
 *  - GPU page faults: the access reports an MMU fault on the AS (AS_FAULTSTATUS = exception
 *    type | access type << 8, AS_FAULTADDRESS = VA; not overwritten while that AS's fault is
 *    still pending) and the job stalls. When the fault is cleared (MMU_INT_CLEAR) or an AS
 *    command is issued, the job retries if the VA now translates (heap growth); if the AS was
 *    disabled (UNMAPPED) instead, the job fails with JS_STATUS = the fault's exception type.
 *    A fault on an AS that is already disabled fails the job at once.
 *  - JS_COMMAND SOFT_STOP / HARD_STOP (and the _0/_1 variants, honoured only if the current
 *    chain's JOB_CHAIN_FLAG matches) end the current chain with STOPPED / TERMINATED,
 *    JS_TAIL = the first job that has not completed, JOB_INT ERR(js). A hung chain (injection)
 *    ignores soft stops.
 *  - JS_STATE: bit js = current active, bit js + 16 = NEXT valid.
 *  - JS_CONFIG start/end flush bits and flush reduction are bookkeeping only (counters, and
 *    GPU_LATEST_FLUSH_ID advances per flush that is not skipped).
 *  - GPU soft/hard reset: every slot idle, NEXT dropped, nothing reported.
 * Linux panfrost (GPL-2.0-only) was a behaviour reference only.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mock_internal.h"

#define MOCK_JM_SLOTS		16
#define MOCK_JM_MAX_JOBS	4096
#define MOCK_JM_LOG		64

/* Register offsets (our own copy, independent of the core's headers). */
#define R_JS_STATE		0x1010u
#define R_JS_BASE		0x1800u
#define R_JS(js, r)		(R_JS_BASE + (js) * 0x80u + (r))
#define JS_HEAD_LO		0x00u
#define JS_HEAD_HI		0x04u
#define JS_TAIL_LO		0x08u
#define JS_TAIL_HI		0x0cu
#define JS_AFFINITY_LO		0x10u
#define JS_AFFINITY_HI		0x14u
#define JS_CONFIG		0x18u
#define JS_COMMAND		0x20u
#define JS_STATUS		0x24u
#define JS_HEAD_NEXT		0x40u
#define JS_AFFINITY_NEXT	0x50u
#define JS_CONFIG_NEXT		0x58u
#define JS_COMMAND_NEXT		0x60u
#define JS_FLUSH_ID_NEXT	0x70u
#define R_AS_PRESENT		0x18u
#define R_SHADER_PRESENT	0x100u
#define R_JS_PRESENT		0x1cu
#define R_JS_FEATURES(n)	(0xc0u + (n) * 4u)
#define R_LATEST_FLUSH_ID	0x38u
#define R_AS_FAULTSTATUS(as)	(0x2400u + ((as) << 6) + 0x1cu)

#define CFG_START_FLUSH		(3u << 8)
#define CFG_JOB_CHAIN_FLAG	(1u << 11)
#define CFG_END_FLUSH		(3u << 12)
#define CFG_FLUSH_REDUCTION	(1u << 14)
#define CFG_DISABLE_WR_BK	(1u << 15)

#define ST_DONE			0x01u
#define ST_STOPPED		0x03u
#define ST_TERMINATED		0x04u
#define ST_ACTIVE		0x08u
#define ST_CONFIG_FAULT		0x40u
#define ST_AFFINITY_FAULT	0x44u

enum job_type {
	JT_NOT_STARTED = 0, JT_NULL = 1, JT_WRITE_VALUE = 2, JT_CACHE_FLUSH = 3, JT_COMPUTE = 4,
	JT_VERTEX = 5, JT_GEOMETRY = 6, JT_TILER = 7, JT_FUSED = 8, JT_FRAGMENT = 9,
	JT_INDEXED_VERTEX = 10,
};

static const char *const job_type_names[] = {
	"not started", "null", "write value", "cache flush", "compute", "vertex", "geometry",
	"tiler", "fused", "fragment", "indexed vertex",
};

struct js_regs {
	uint64_t head, affinity;
	uint32_t config, flush_id;
};

struct mock_slot {
	bool next_valid;
	struct js_regs next;

	bool active;			/* current chain present */
	struct js_regs cur;
	uint64_t pos;			/* next descriptor to execute (0 = chain end) */
	uint64_t head_reg, tail_reg;
	uint32_t status;
	bool err_pending;		/* ERR raised, JOB_INT_CLEAR not yet written */

	/* state of the current run */
	bool runnable;			/* the executor should (re)run it */
	bool hang, stall, fail;
	uint32_t fail_status;
	bool mmu_stall;
	unsigned int fault_as;
	uint32_t fault_code;
	uint64_t fault_va;
	char fault_access;
	bool prewalked;
	uint32_t njobs;
	uint8_t done_idx[65536 / 8];
	uint8_t in_chain[65536 / 8];

	/* test injection */
	enum mock_jm_inject inj;
	uint32_t inj_status, inj_count;

	struct mock_jm_job_rec log[MOCK_JM_LOG];
	uint32_t log_total;
	struct mock_js_stats st;
};

struct mock_jm {
	struct mock_slot slot[MOCK_JM_SLOTS];
	struct pw_os *os;
	pthread_t thr;
	bool thread_running, stop;
	bool verbose;
};

static bool bit_get(const uint8_t *map, uint32_t i)
{
	return (map[i >> 3] >> (i & 7)) & 1;
}

static void bit_set(uint8_t *map, uint32_t i)
{
	map[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static uint64_t rd64(const uint8_t *p)
{
	uint64_t v;

	memcpy(&v, p, 8);
	return v;
}

static uint32_t rd32(const uint8_t *p)
{
	uint32_t v;

	memcpy(&v, p, 4);
	return v;
}

static bool slot_present(struct pw_os *os, unsigned int js)
{
	return js < MOCK_JM_SLOTS && (mock_reg_get_locked(os, R_JS_PRESENT) & (1u << js));
}

static void log_job(struct mock_slot *s, uint64_t va, uint32_t type, uint32_t index,
		    uint32_t status)
{
	struct mock_jm_job_rec *r = &s->log[s->log_total % MOCK_JM_LOG];

	r->va = va;
	r->type = type;
	r->index = index;
	r->status = status;
	s->log_total++;
}

/* ---- flush bookkeeping (JS_CONFIG flush bits, flush reduction) ---- */

static void flush_done(struct pw_os *os)
{
	mock_reg_set_locked(os, R_LATEST_FLUSH_ID, mock_reg_get_locked(os, R_LATEST_FLUSH_ID) + 1);
}

/* ---- slot state machine ---- */

static void slot_promote(struct pw_os *os, struct mock_jm *jm, unsigned int js)
{
	struct mock_slot *s = &jm->slot[js];

	if (!s->next_valid || s->active || s->err_pending)
		return;
	s->cur = s->next;
	s->next_valid = false;
	s->active = true;
	s->pos = s->cur.head;
	s->head_reg = s->tail_reg = s->cur.head;
	s->status = ST_ACTIVE;
	s->hang = s->stall = s->fail = s->mmu_stall = false;
	s->prewalked = false;
	s->njobs = 0;
	memset(s->done_idx, 0, sizeof(s->done_idx));
	memset(s->in_chain, 0, sizeof(s->in_chain));

	if (s->inj != MOCK_JM_INJ_NONE) {
		switch (s->inj) {
		case MOCK_JM_INJ_HANG:
			s->hang = true;
			break;
		case MOCK_JM_INJ_STALL:
			s->stall = true;
			break;
		case MOCK_JM_INJ_FAIL:
			s->fail = true;
			s->fail_status = s->inj_status;
			break;
		default:
			break;
		}
		if (s->inj_count && --s->inj_count == 0)
			s->inj = MOCK_JM_INJ_NONE;
	}

	s->st.runs++;
	s->st.last_head = s->cur.head;
	s->st.last_config = s->cur.config;
	s->st.last_affinity = s->cur.affinity;
	s->st.last_flush_id = s->cur.flush_id;
	if (s->cur.config & CFG_START_FLUSH) {
		if ((s->cur.config & CFG_FLUSH_REDUCTION) &&
		    s->cur.flush_id == mock_reg_get_locked(os, R_LATEST_FLUSH_ID)) {
			s->st.start_flushes_skipped++;
		} else {
			s->st.start_flushes++;
			flush_done(os);
		}
	}
	s->runnable = true;
	mock_kick_locked(os);
}

/* The current chain ended with an error/stop status: ERR(js), slot halts until cleared. */
static void slot_end_err(struct pw_os *os, struct mock_jm *jm, unsigned int js, uint32_t status,
			 uint64_t at)
{
	struct mock_slot *s = &jm->slot[js];

	s->active = false;
	s->runnable = false;
	s->mmu_stall = s->hang = s->stall = false;
	s->status = status;
	s->head_reg = s->tail_reg = at;
	s->st.last_status = status;
	s->st.last_tail = at;
	s->err_pending = true;
	mock_raise_job_locked(os, 1u << (js + 16));
	(void)jm;
}

/* A fault on job @job_va: write the header's exception status + fault pointer if the header
 * is writable, then fail the chain. */
static void slot_fault_wr(struct pw_os *os, struct mock_jm *jm, unsigned int js, uint32_t code,
			  uint64_t job_va, uint64_t fault_ptr, bool write_header)
{
	struct mock_slot *s = &jm->slot[js];
	unsigned int as = s->cur.config & 0xf;

	if (write_header && !(s->cur.config & CFG_DISABLE_WR_BK)) {
		mock_gpu_rw_locked(os, as, job_va, &code, 4, true);
		mock_gpu_rw_locked(os, as, job_va + 8, &fault_ptr, 8, true);
	}
	s->st.failed++;
	if (code == ST_CONFIG_FAULT)
		s->st.config_faults++;
	log_job(s, job_va, 0xff, 0, code);
	if (jm->verbose)
		fprintf(stderr, "  [mock-jm] JS%u: job 0x%llx failed with 0x%x\n", js,
			(unsigned long long)job_va, code);
	slot_end_err(os, jm, js, code, job_va);
}

static void slot_fault(struct pw_os *os, struct mock_jm *jm, unsigned int js, uint32_t code,
		       uint64_t job_va, uint64_t fault_ptr)
{
	slot_fault_wr(os, jm, js, code, job_va, fault_ptr, true);
}

static void slot_done(struct pw_os *os, struct mock_jm *jm, unsigned int js)
{
	struct mock_slot *s = &jm->slot[js];

	s->active = false;
	s->runnable = false;
	s->status = ST_DONE;
	s->st.done++;
	s->st.last_status = ST_DONE;
	s->st.last_tail = s->tail_reg;
	if (s->cur.config & CFG_END_FLUSH) {
		s->st.end_flushes++;
		flush_done(os);
	}
	mock_raise_job_locked(os, 1u << js);
	slot_promote(os, jm, js);	/* the JM starts NEXT without waiting for an ack */
}

/* A GPU access of the current job faulted with @code. */
static void slot_page_fault(struct pw_os *os, struct mock_jm *jm, unsigned int js, uint32_t code,
			    uint64_t va, char access)
{
	struct mock_slot *s = &jm->slot[js];
	unsigned int as = s->cur.config & 0xf;
	uint32_t access_type = access == 'w' ? 3u : access == 'x' ? 1u : 2u;

	s->st.page_faults++;
	s->st.last_fault_va = va;
	if (!mock_as_enabled_locked(os, as)) {
		/* The AS is off (an unhandled fault disabled it, or never bound). */
		slot_fault(os, jm, js, code, s->pos, va);
		return;
	}
	if (!(mock_rawstat_locked(os, 1) & (1u << as)))
		mock_raise_mmu_fault_locked(os, as, code | (access_type << 8), va);
	s->mmu_stall = true;
	s->runnable = false;
	s->fault_as = as;
	s->fault_code = code;
	s->fault_va = va;
	s->fault_access = access;
}

/* After an MMU_INT_CLEAR or an AS command: resume or kill the jobs stalled on @as. */
static void mmu_event(struct pw_os *os, struct mock_jm *jm, unsigned int as)
{
	for (unsigned int js = 0; js < MOCK_JM_SLOTS; js++) {
		struct mock_slot *s = &jm->slot[js];

		if (!s->active || !s->mmu_stall || s->fault_as != as)
			continue;
		if (!mock_as_enabled_locked(os, as)) {
			s->mmu_stall = false;
			slot_fault(os, jm, js, s->fault_code, s->pos, s->fault_va);
			continue;
		}
		if (mock_rawstat_locked(os, 1) & (1u << as))
			continue;	/* fault not acknowledged yet */
		if (mock_gpu_translate_locked(os, as, s->fault_va, s->fault_access) == 0) {
			s->mmu_stall = false;
			s->runnable = true;
			s->st.fault_retries++;
			mock_kick_locked(os);
		}
	}
}

/* ---- the chain walker ---- */

static void prewalk(struct pw_os *os, struct mock_slot *s, unsigned int as)
{
	uint64_t va = s->pos;

	for (uint32_t n = 0; va && n < MOCK_JM_MAX_JOBS; n++) {
		uint8_t h[32];

		if (mock_gpu_rw_locked(os, as, va, h, sizeof(h), false))
			break;
		bit_set(s->in_chain, rd32(h + 16) >> 16);
		va = rd64(h + 24);
	}
	s->prewalked = true;
}

static bool type_allowed(struct pw_os *os, unsigned int js, uint32_t type)
{
	uint32_t feat = mock_reg_get_locked(os, R_JS_FEATURES(js));

	if (type == JT_INDEXED_VERTEX)
		return feat & (1u << JT_TILER);
	return type >= JT_NULL && type <= JT_FRAGMENT && (feat & (1u << type));
}

/* Returns false if the job did not complete (fault, stall). */
static bool exec_write_value(struct pw_os *os, struct mock_jm *jm, unsigned int js,
			     unsigned int as, uint64_t job)
{
	struct mock_slot *s = &jm->slot[js];
	uint8_t p[32];
	uint64_t addr, value = 0;
	uint32_t type, size;
	int r;

	r = mock_gpu_rw_locked(os, as, job + 32, p, sizeof(p), false);
	if (r) {
		slot_page_fault(os, jm, js, (uint32_t)r, job + 32, 'r');
		return false;
	}
	addr = rd64(p);
	type = rd32(p + 8);
	switch (type) {
	case 1:		/* Cycle Counter */
		value = mock_gpu_cycles_locked(os);
		size = 8;
		break;
	case 2:		/* System Timestamp */
		value = mock_gpu_timestamp_locked(os);
		size = 8;
		break;
	case 3:		/* Zero (a 64-bit store of 0: model choice) */
		size = 8;
		break;
	case 4:		/* Immediate 8/16/32/64 */
	case 5:
	case 6:
	case 7:
		value = rd64(p + 16);
		size = 1u << (type - 4);
		break;
	default:
		slot_fault(os, jm, js, ST_CONFIG_FAULT, job, job);
		return false;
	}
	r = mock_gpu_rw_locked(os, as, addr, &value, size, true);
	if (r) {
		slot_page_fault(os, jm, js, (uint32_t)r, addr, 'w');
		return false;
	}
	s->st.write_values++;
	return true;
}

static void run_slot(struct pw_os *os, struct mock_jm *jm, unsigned int js)
{
	struct mock_slot *s = &jm->slot[js];
	unsigned int as = s->cur.config & 0xf;
	uint64_t shader_present = (uint64_t)mock_reg_get_locked(os, R_SHADER_PRESENT) |
				  ((uint64_t)mock_reg_get_locked(os, R_SHADER_PRESENT + 4) << 32);

	s->runnable = false;
	if (s->fail) {
		slot_fault(os, jm, js, s->fail_status, s->pos, s->pos);
		return;
	}
	if (!(s->cur.affinity & shader_present) || (s->cur.affinity & ~shader_present)) {
		slot_fault_wr(os, jm, js, ST_AFFINITY_FAULT, s->pos, 0, false);
		return;
	}
	if (!(mock_reg_get_locked(os, R_AS_PRESENT) & (1u << as))) {
		slot_fault_wr(os, jm, js, ST_CONFIG_FAULT, s->pos, 0, false);
		return;
	}
	if (!s->prewalked)
		prewalk(os, s, as);

	while (s->pos) {
		uint64_t job = s->pos, next;
		uint32_t w4, w5, type, index, dep1, dep2, done = ST_DONE;
		uint8_t h[32];
		int r;

		s->head_reg = s->tail_reg = job;
		if (s->njobs >= MOCK_JM_MAX_JOBS || (job & 63)) {
			slot_fault_wr(os, jm, js, ST_CONFIG_FAULT, job, job, !(job & 63));
			return;
		}
		r = mock_gpu_rw_locked(os, as, job, h, sizeof(h), false);
		if (r) {
			slot_page_fault(os, jm, js, (uint32_t)r, job, 'r');
			return;
		}
		w4 = rd32(h + 16);
		w5 = rd32(h + 20);
		next = rd64(h + 24);
		type = (w4 >> 1) & 0x7f;
		index = w4 >> 16;
		dep1 = w5 & 0xffff;
		dep2 = w5 >> 16;

		if (!(w4 & 1) || type > JT_INDEXED_VERTEX || !type_allowed(os, js, type)) {
			slot_fault(os, jm, js, ST_CONFIG_FAULT, job, job);
			return;
		}
		if ((dep1 && !bit_get(s->done_idx, dep1) && bit_get(s->in_chain, dep1)) ||
		    (dep2 && !bit_get(s->done_idx, dep2) && bit_get(s->in_chain, dep2))) {
			s->st.dep_violations++;
			slot_fault(os, jm, js, ST_CONFIG_FAULT, job, job);
			return;
		}

		switch (type) {
		case JT_NULL:
			s->st.null_jobs++;
			break;
		case JT_WRITE_VALUE:
			if (!exec_write_value(os, jm, js, as, job))
				return;
			break;
		case JT_CACHE_FLUSH: {
			uint8_t p[8];

			r = mock_gpu_rw_locked(os, as, job + 32, p, sizeof(p), false);
			if (r) {
				slot_page_fault(os, jm, js, (uint32_t)r, job + 32, 'r');
				return;
			}
			s->st.cache_flushes++;
			flush_done(os);
			break;
		}
		default:
			s->st.noop_jobs++;
			if (jm->verbose)
				fprintf(stderr, "  [mock-jm] JS%u: %s job 0x%llx completed without "
					"shaders\n", js, job_type_names[type],
					(unsigned long long)job);
			break;
		}

		if (!(s->cur.config & CFG_DISABLE_WR_BK)) {
			r = mock_gpu_rw_locked(os, as, job, &done, 4, true);
			if (r) {
				slot_page_fault(os, jm, js, (uint32_t)r, job, 'w');
				return;
			}
		}
		bit_set(s->done_idx, index);
		log_job(s, job, type, index, ST_DONE);
		s->st.jobs++;
		s->njobs++;
		s->pos = next;
	}
	slot_done(os, jm, js);
}

static void *exec_thread(void *arg)
{
	struct pw_os *os = arg;
	struct mock_jm *jm = mock_jm_of(os);

	mock_lock(os);
	while (!jm->stop) {
		bool ran = false;

		for (unsigned int js = 0; js < MOCK_JM_SLOTS; js++) {
			struct mock_slot *s = &jm->slot[js];

			if (s->active && s->runnable && !s->hang && !s->stall && !s->mmu_stall) {
				run_slot(os, jm, js);
				ran = true;
			}
		}
		if (!ran)
			mock_cond_wait_locked(os, 5000000);
	}
	mock_unlock(os);
	return NULL;
}

/* ---- pw_os_mock.c hooks ---- */

struct mock_jm *mock_jm_create(void)
{
	return calloc(1, sizeof(struct mock_jm));
}

void mock_jm_destroy(struct mock_jm *jm)
{
	free(jm);
}

void mock_jm_start(struct pw_os *os)
{
	struct mock_jm *jm = mock_jm_of(os);

	jm->os = os;
	jm->verbose = getenv("PW_TEST_VERBOSE") != NULL;
	if (pthread_create(&jm->thr, NULL, exec_thread, os))
		abort();
	jm->thread_running = true;
}

void mock_jm_stop(struct pw_os *os)
{
	struct mock_jm *jm = mock_jm_of(os);

	if (!jm->thread_running)
		return;
	mock_lock(os);
	jm->stop = true;
	mock_kick_locked(os);
	mock_unlock(os);
	pthread_join(jm->thr, NULL);
	jm->thread_running = false;
}

void mock_jm_reset_locked(struct pw_os *os)
{
	struct mock_jm *jm = mock_jm_of(os);

	for (unsigned int js = 0; js < MOCK_JM_SLOTS; js++) {
		struct mock_slot *s = &jm->slot[js];

		s->next_valid = false;
		s->active = false;
		s->runnable = false;
		s->hang = s->stall = s->fail = s->mmu_stall = false;
		s->err_pending = false;
		s->status = 0;
		s->head_reg = s->tail_reg = 0;
		s->pos = 0;
		s->st.resets++;
	}
}

void mock_jm_int_clear_locked(struct pw_os *os, uint32_t bits)
{
	struct mock_jm *jm = mock_jm_of(os);

	for (unsigned int js = 0; js < MOCK_JM_SLOTS; js++) {
		struct mock_slot *s = &jm->slot[js];

		if ((bits & (1u << (js + 16))) && s->err_pending) {
			s->err_pending = false;
			slot_promote(os, jm, js);
		}
	}
}

void mock_jm_mmu_clear_locked(struct pw_os *os, uint32_t bits)
{
	for (unsigned int as = 0; as < 16; as++) {
		if (bits & (1u << as))
			mmu_event(os, mock_jm_of(os), as);
	}
}

void mock_jm_as_command_locked(struct pw_os *os, unsigned int as, uint32_t cmd)
{
	(void)cmd;
	mmu_event(os, mock_jm_of(os), as);
}

static void js_command(struct pw_os *os, struct mock_jm *jm, unsigned int js, uint32_t cmd)
{
	struct mock_slot *s = &jm->slot[js];
	bool flag, hard;

	switch (cmd) {
	case 0:		/* NOP */
		return;
	case 2:
	case 3:
	case 4:
	case 5:
	case 6:
	case 7:
		break;
	default:
		mock_model_error(os, "JS%u: JS_COMMAND 0x%x", js, cmd);
		return;
	}
	if (!s->active) {
		s->st.ignored_stops++;
		return;
	}
	flag = (s->cur.config & CFG_JOB_CHAIN_FLAG) != 0;
	if ((cmd == 4 || cmd == 5) && flag) {
		s->st.ignored_stops++;
		return;
	}
	if ((cmd == 6 || cmd == 7) && !flag) {
		s->st.ignored_stops++;
		return;
	}
	hard = cmd == 3 || cmd == 5 || cmd == 7;
	if (!hard && s->hang) {
		s->st.ignored_stops++;	/* a hung job never reaches a job boundary */
		return;
	}
	if (hard)
		s->st.terminated++;
	else
		s->st.stopped++;
	slot_end_err(os, jm, js, hard ? ST_TERMINATED : ST_STOPPED, s->pos);
}

bool mock_jm_reg_write_locked(struct pw_os *os, uint32_t off, uint32_t v)
{
	struct mock_jm *jm = mock_jm_of(os);
	unsigned int js;
	uint32_t r;

	if (off == R_JS_STATE) {
		mock_model_error(os, "write to read-only JS_STATE");
		return true;
	}
	if (off < R_JS_BASE)
		return false;	/* THROTTLE etc.: plain registers */
	js = (off - R_JS_BASE) / 0x80u;
	r = (off - R_JS_BASE) % 0x80u;
	if (!slot_present(os, js)) {
		mock_model_error(os, "write 0x%x to JS%u, which is not in JS_PRESENT", v, js);
		return true;
	}
	switch (r) {
	case JS_HEAD_LO:
	case JS_HEAD_HI:
	case JS_TAIL_LO:
	case JS_TAIL_HI:
	case JS_STATUS:
	case JS_CONFIG:
	case JS_AFFINITY_LO:
	case JS_AFFINITY_HI:
		mock_model_error(os, "JS%u: write to read-only register +0x%x", js, r);
		return true;
	case JS_COMMAND:
		js_command(os, jm, js, v);
		return true;
	case JS_COMMAND_NEXT: {
		struct mock_slot *s = &jm->slot[js];

		if (v == 0) {
			if (s->next_valid) {
				s->next_valid = false;
				s->st.next_cancels++;
			}
		} else if (v == 1) {
			if (s->next_valid)
				mock_model_error(os, "JS%u: START while NEXT is still queued", js);
			s->next.head = (uint64_t)mock_reg_get_locked(os, R_JS(js, JS_HEAD_NEXT)) |
				       ((uint64_t)mock_reg_get_locked(os, R_JS(js, JS_HEAD_NEXT + 4))
					<< 32);
			s->next.affinity =
				(uint64_t)mock_reg_get_locked(os, R_JS(js, JS_AFFINITY_NEXT)) |
				((uint64_t)mock_reg_get_locked(os, R_JS(js, JS_AFFINITY_NEXT + 4))
				 << 32);
			s->next.config = mock_reg_get_locked(os, R_JS(js, JS_CONFIG_NEXT));
			s->next.flush_id = mock_reg_get_locked(os, R_JS(js, JS_FLUSH_ID_NEXT));
			s->next_valid = true;
			s->st.starts++;
			slot_promote(os, jm, js);
		} else {
			mock_model_error(os, "JS%u: JS_COMMAND_NEXT 0x%x", js, v);
		}
		return true;
	}
	default:
		return false;	/* *_NEXT, XAFFINITY: plain registers, latched on START */
	}
}

bool mock_jm_reg_read_locked(struct pw_os *os, uint32_t off, uint32_t *v)
{
	struct mock_jm *jm = mock_jm_of(os);
	struct mock_slot *s;
	unsigned int js;
	uint32_t r;

	if (off == R_JS_STATE) {
		uint32_t st = 0;

		for (js = 0; js < MOCK_JM_SLOTS; js++) {
			if (jm->slot[js].active)
				st |= 1u << js;
			if (jm->slot[js].next_valid)
				st |= 1u << (js + 16);
		}
		*v = st;
		return true;
	}
	if (off < R_JS_BASE)
		return false;
	js = (off - R_JS_BASE) / 0x80u;
	r = (off - R_JS_BASE) % 0x80u;
	if (!slot_present(os, js)) {
		mock_model_error(os, "read of JS%u, which is not in JS_PRESENT", js);
		*v = 0;
		return true;
	}
	s = &jm->slot[js];
	switch (r) {
	case JS_HEAD_LO:
		*v = (uint32_t)s->head_reg;
		return true;
	case JS_HEAD_HI:
		*v = (uint32_t)(s->head_reg >> 32);
		return true;
	case JS_TAIL_LO:
		*v = (uint32_t)s->tail_reg;
		return true;
	case JS_TAIL_HI:
		*v = (uint32_t)(s->tail_reg >> 32);
		return true;
	case JS_AFFINITY_LO:
		*v = (uint32_t)s->cur.affinity;
		return true;
	case JS_AFFINITY_HI:
		*v = (uint32_t)(s->cur.affinity >> 32);
		return true;
	case JS_CONFIG:
		*v = s->cur.config;
		return true;
	case JS_COMMAND:
		*v = 0;
		return true;
	case JS_STATUS:
		*v = s->status;
		return true;
	case JS_COMMAND_NEXT:
		*v = s->next_valid ? 1u : 0u;
		return true;
	default:
		return false;
	}
}

/* ---- test API (mock_gpu.h) ---- */

void mock_jm_js_stats(struct pw_os *os, unsigned int js, struct mock_js_stats *st)
{
	memset(st, 0, sizeof(*st));
	mock_lock(os);
	if (js < MOCK_JM_SLOTS) {
		struct mock_slot *s = &mock_jm_of(os)->slot[js];

		*st = s->st;
		st->running = s->active;
		st->next_valid = s->next_valid;
		st->mmu_stalled = s->active && s->mmu_stall;
		st->hung = s->active && s->hang;
		st->stalled = s->active && s->stall;
		st->err_pending = s->err_pending;
	}
	mock_unlock(os);
}

void mock_jm_inject(struct pw_os *os, unsigned int js, enum mock_jm_inject what, uint32_t status,
		    uint32_t count)
{
	mock_lock(os);
	if (js < MOCK_JM_SLOTS) {
		struct mock_slot *s = &mock_jm_of(os)->slot[js];

		s->inj = what;
		s->inj_status = status;
		s->inj_count = count;
	}
	mock_unlock(os);
}

void mock_jm_kick(struct pw_os *os, unsigned int js)
{
	mock_lock(os);
	if (js < MOCK_JM_SLOTS) {
		struct mock_slot *s = &mock_jm_of(os)->slot[js];

		if (s->active && s->stall) {
			s->stall = false;
			s->runnable = true;
			mock_kick_locked(os);
		}
	}
	mock_unlock(os);
}

unsigned int mock_jm_job_log(struct pw_os *os, unsigned int js, struct mock_jm_job_rec *recs,
			     unsigned int max)
{
	unsigned int n = 0;

	mock_lock(os);
	if (js < MOCK_JM_SLOTS) {
		struct mock_slot *s = &mock_jm_of(os)->slot[js];
		uint32_t avail = s->log_total < MOCK_JM_LOG ? s->log_total : MOCK_JM_LOG;
		uint32_t first;

		if (max > avail)
			max = avail;
		first = s->log_total - max;
		for (n = 0; n < max; n++)
			recs[n] = s->log[(first + n) % MOCK_JM_LOG];
	}
	mock_unlock(os);
	return n;
}

void mock_jm_job_log_clear(struct pw_os *os, unsigned int js)
{
	mock_lock(os);
	if (js < MOCK_JM_SLOTS)
		mock_jm_of(os)->slot[js].log_total = 0;
	mock_unlock(os);
}
