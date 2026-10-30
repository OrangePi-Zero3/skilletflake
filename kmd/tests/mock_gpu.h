/* SPDX-License-Identifier: MIT */
/*
 * mock_gpu.h - host test double for pw_os.h: a mock MMIO register file with a small behavioural
 * model of an RK3588 Mali-G610 (CSF) or (jm-arch) an Orange Pi Zero3 Mali-G31 MP2 (job manager),
 * fake physical memory with a separate "DRAM" and "CPU cache" view for cached mappings (so
 * missing pw_os_mem_flush() calls are visible to the emulated GPU), an independent AArch64
 * page-table walker, an emulated MCU (G610) and a job-chain executor (G31, mock_jm.c).
 */
#ifndef MOCK_GPU_H
#define MOCK_GPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pw_os.h"

struct pw_device;

/* G610 values. GPU_ID 0xa867 << 16 (Mesa bin/drm-shim.py:93, "g610": "a867"); the feature
 * registers are "Dumped from a G610" in Mesa src/panfrost/drm-shim/panfrost_noop.c:178-194
 * (MIT). Revision/status nibbles of GPU_ID are not known (left 0). */
#define MOCK_G610_GPU_ID		0xa8670000u
#define MOCK_G610_CSF_ID		0x40a0412u
#define MOCK_G610_L2_FEATURES		0x7120306u
#define MOCK_G610_TILER_FEATURES	0x809u
#define MOCK_G610_MEM_FEATURES		0x301u
#define MOCK_G610_MMU_FEATURES		0x2830u		/* 48 VA bits, 40 PA bits */
#define MOCK_G610_THREAD_FEATURES	0x4010000u
#define MOCK_G610_MAX_THREADS		2048u
#define MOCK_G610_MAX_WG_SIZE		1024u
#define MOCK_G610_MAX_BARRIER		1024u
#define MOCK_G610_TEXTURE_FEATURES0	0xc1ffff9eu
#define MOCK_G610_AS_PRESENT		0xffu
#define MOCK_G610_SHADER_PRESENT	0x50005ull
#define MOCK_G610_L2_PRESENT		1ull
#define MOCK_G610_TILER_PRESENT		1ull

/*
 * (jm-arch) G31 MP2 values (Orange Pi Zero3, Allwinner H618). Sources and confidence, to be
 * confirmed with panwin-info / a Linux panfrost boot log on the board (docs/status/jm-arch.md):
 *  GPU_ID 0x7093 << 16: arch 7.0 rev 9, product 3 = G31 (Mesa pan_model.c PAN_PROD_ID(7, 0, 3);
 *    Linux panfrost matches 0x7003 ignoring the arch-rev nibble). The 0x7093 / r0p0 / status 0
 *    values are what public panfrost boot logs of G31 boards print: RECALLED, not dumped here.
 *  L2 0x07110206, core 0x2, tiler 0x209, mem 0x1, AS 0xff, JS 0x7, shader_present 0x3,
 *    l2_present 0x1: same recalled boot-log line ("Features: L2:... AS:0xff JS:0x7"). GUESSES
 *    until read on the board.
 *  MMU 0x2830 (48-bit VA, 40-bit PA): GUESS (Mesa's drm-shim default for most hardware is
 *    0x00280030; a 0x2823 / 35-bit VA G31 would also work with the core, which needs >= 33).
 *  Threads: 512 max threads / workgroup / barrier (Mesa: "G31 is 512"), THREAD_FEATURES with
 *    16384 registers (512 * 32) and 2 tasks, TLS_ALLOC 512: GUESSES.
 *  JS_FEATURES 0x20e (JS0: null, set-value, cache-flush, fragment), 0x1fe (JS1: everything but
 *    fragment), 0x7e (JS2: compute-type jobs): the common kbase/panfrost layout, GUESSES.
 *  Texture features: the G610 dump (Mesa only uses them for format support): GUESS.
 *  Coherency features 0x1 (ACE-Lite only; the H616 integration is non-coherent anyway): GUESS.
 *  STACK_PRESENT 0x1, AFBC_FEATURES 0: GUESSES.
 */
#define MOCK_G31_GPU_ID			0x70930000u
#define MOCK_G31_L2_FEATURES		0x07110206u
#define MOCK_G31_CORE_FEATURES		0x2u
#define MOCK_G31_TILER_FEATURES		0x209u
#define MOCK_G31_MEM_FEATURES		0x1u
#define MOCK_G31_MMU_FEATURES		0x2830u
#define MOCK_G31_AS_PRESENT		0xffu
#define MOCK_G31_JS_PRESENT		0x7u
#define MOCK_G31_JS0_FEATURES		0x20eu
#define MOCK_G31_JS1_FEATURES		0x1feu
#define MOCK_G31_JS2_FEATURES		0x7eu
#define MOCK_G31_THREAD_FEATURES	0x02004000u
#define MOCK_G31_MAX_THREADS		512u
#define MOCK_G31_MAX_WG_SIZE		512u
#define MOCK_G31_MAX_BARRIER		512u
#define MOCK_G31_THREAD_TLS_ALLOC	512u
#define MOCK_G31_TEXTURE_FEATURES0	MOCK_G610_TEXTURE_FEATURES0
#define MOCK_G31_COHERENCY_FEATURES	0x1u
#define MOCK_G31_AFBC_FEATURES		0x0u
#define MOCK_G31_SHADER_PRESENT		0x3ull
#define MOCK_G31_L2_PRESENT		1ull
#define MOCK_G31_TILER_PRESENT		1ull
#define MOCK_G31_STACK_PRESENT		1ull

/* Which GPU the mock models. G610 is the default (all pre-JM tests). */
enum mock_model {
	MOCK_MODEL_G610 = 0,		/* RK3588, CSF: MCU + command-stream executor (mock_csf.c) */
	MOCK_MODEL_G31,			/* H618, job manager: job slots + executor (mock_jm.c) */
};

/* Emulated FW interface (plausible, not dumped from hardware). */
#define MOCK_FW_IFACE_VERSION		((1u << 24) | (1u << 16) | 0u)	/* v1.1.0 */
#define MOCK_FW_GROUP_NUM		8u
#define MOCK_FW_STREAM_NUM		8u
#define MOCK_FW_CS_FEATURES		(0x5fu | (8u << 8) | (7u << 16))	/* 96 regs, 8 SB */

#define MOCK_PA_BASE			0x100000000ull	/* fake DRAM above 4 GiB */

enum mock_fw_variant {
	MOCK_FW_OK = 0,
	MOCK_FW_NEVER_BOOTS,		/* MCU_CONTROL=AUTO does nothing */
	MOCK_FW_VERSION_ZERO,
	MOCK_FW_BAD_GROUP_NUM,		/* group_num = 2 */
	MOCK_FW_BAD_INPUT_VA,		/* glb input_va outside the shared section */
	MOCK_FW_BAD_STREAM_NUM,		/* stream_num = 40 */
	MOCK_FW_MISMATCHED_CSG,		/* CSG 3 differs from CSG 0 */
	MOCK_FW_IGNORE_DOORBELL,	/* never acks requests */
};

struct mock_config {
	enum mock_model model;		/* (jm-arch) register set and command model */
	uint32_t gpu_id;		/* 0 = the model's GPU_ID */
	uint32_t as_present;		/* (jm-arch) 0 = the model's AS_PRESENT (LRU tests) */
	bool gpu_id_zero;		/* GPU_ID reads 0 */
	bool no_reset_complete;		/* soft reset never raises RESET_COMPLETED */
	bool no_l2_ready;		/* L2_READY never sets */
	enum mock_fw_variant fw_variant;
	const char *fw_path;		/* file served for any firmware name; NULL = ENOENT */
	const void *fw_data;		/* alternative: in-memory image (not copied) */
	size_t fw_size;
	uint32_t as_busy_reads;		/* AS_STATUS reads ACTIVE this many times per command */
	uint64_t pa_base;		/* 0 = MOCK_PA_BASE */
	bool mcu_lenient;		/* fuzzing: boot even if the AS0 mappings look wrong */
	/* Reset/flush/MCU-boot completions appear in RAWSTAT this long after the command
	 * (0 = default 100 us), like real hardware; instant_completion makes them synchronous. */
	uint32_t completion_delay_us;
	bool instant_completion;
	/* GPU RAWSTAT reads from non-IRQ threads don't show RESET_COMPLETED/CLEAN_CACHES_COMPLETED,
	 * forcing completion through the GPU bottom half. */
	bool hide_completions_from_waiters;
	/* (e2e) Honour PW_MEM_ZEROED (pw_os.h: such pages must read as zero). Off by default so
	 * the host tests keep their 0xa5 poison everywhere; libdrm-win's core transport turns it
	 * on because userspace (Mesa) relies on zeroed BOs, as on Linux. */
	bool honor_zeroed;
	/* (jm-hw) G31 only: SHADER_READY / TILER_READY never set (power-up timeout tests). */
	bool no_shader_ready;
	bool no_tiler_ready;
};

struct mock_as_state {
	bool enabled;			/* last UPDATE latched a non-UNMAPPED config */
	uint64_t transtab, memattr, transcfg;
	bool locked;
	uint64_t lockaddr;
	uint32_t updates, locks, unlocks;
	/* (added by jm-mem) AS_COMMAND_FLUSH_PT / FLUSH_MEM seen. On the G31 model they end a
	 * pending LOCK (the JM TLB-maintenance sequence: LOCK, update, FLUSH_PT/FLUSH_MEM); on the
	 * G610 model they are counted and otherwise ignored. */
	uint32_t flush_pts, flush_mems;
};

struct mock_glb_snapshot {
	bool valid;
	uint32_t req, ack_irq_mask, poweroff_timer, progress_timer, idle_timer;
	uint64_t core_en_mask;
	uint32_t doorbells;
};

struct pw_os *mock_create(const struct mock_config *cfg);
void mock_destroy(struct pw_os *os);

/* Interrupt delivery: when a device is attached, three threads (one per line, like panthor's
 * threaded IRQs) call pw_irq_top() whenever RAWSTAT & MASK != 0, then pw_irq_bottom(). */
void mock_attach_irq(struct pw_os *os, struct pw_device *dev);
void mock_detach_irq(struct pw_os *os);
/* Number of times a line was asserted but pw_irq_top() did not claim it (should be 0). */
uint32_t mock_irq_unclaimed(struct pw_os *os);
uint32_t mock_irq_delivered(struct pw_os *os, unsigned int line);

/* Register file access from tests (bypassing the model's side effects for reads). */
uint32_t mock_reg(struct pw_os *os, uint32_t offset);
uint32_t mock_rawstat(struct pw_os *os, unsigned int line);	/* 0 job, 1 mmu, 2 gpu */
uint32_t mock_int_mask(struct pw_os *os, unsigned int line);	/* live INT_MASK value */
void mock_get_as(struct pw_os *os, unsigned int as, struct mock_as_state *st);
/* (added by jm-mem) The next @count pw_os_mem_alloc() calls fail with PW_ENOMEM (0 = off). */
void mock_fail_mem_allocs(struct pw_os *os, uint32_t count);
uint32_t mock_flush_count(struct pw_os *os);
uint32_t mock_last_flush_cmd(struct pw_os *os);
uint32_t mock_soft_resets(struct pw_os *os);
uint32_t mock_model_errors(struct pw_os *os);	/* protocol violations seen by the model */
uint32_t mock_mcu_status(struct pw_os *os);
void mock_glb_snapshot(struct pw_os *os, struct mock_glb_snapshot *s);

/* Fault / event injection. */
void mock_inject_mmu_fault(struct pw_os *os, unsigned int as, uint32_t fault_status, uint64_t va);
void mock_inject_gpu_fault(struct pw_os *os, uint32_t fault_status, uint64_t addr);
void mock_raise_mmu_irq_bits(struct pw_os *os, uint32_t bits);
void mock_mcu_raise_glb_event(struct pw_os *os, uint32_t bits);	/* toggles ack bits */
uint32_t mock_mcu_glb_req(struct pw_os *os);
uint32_t mock_mcu_glb_ack(struct pw_os *os);

/* Emulated GPU MMU walk using the AS configuration latched by the last UPDATE, reading the
 * tables from "DRAM" (what the GPU sees, i.e. only flushed CPU writes). (jm-mem) Both the
 * AArch64 4K format (TRANSCFG ADRMODE 6) and, on the G31 model, the legacy Mali LPAE format
 * (ADRMODE_LEGACY 0 with TRANSTAB[1:0] = 3: 4 levels, leaf type 0b01 at every level, bit 6 =
 * read, bit 7 = write, XN bit 54, no access flag) are walked; descriptors of the other format
 * fault, so a format mix-up is visible to tests.
 * access: 'r', 'w' or 'x'. Returns 0 or a Mali exception code (0xc0+lvl translation,
 * 0xc8+lvl permission, 0xd8+lvl access flag, 0x80 bus fault). */
int mock_gpu_translate(struct pw_os *os, unsigned int as, uint64_t va, char access, uint64_t *pa,
		       int *level, uint64_t *pte);
/* Read GPU-visible bytes at a VA through AS @as. Returns 0 or the fault code. */
int mock_gpu_read(struct pw_os *os, unsigned int as, uint64_t va, void *buf, size_t len);
/* Write GPU-visible bytes at a VA through AS @as. Returns 0 or the fault code. */
int mock_gpu_write(struct pw_os *os, unsigned int as, uint64_t va, const void *buf, size_t len);
/* Host pointer to the "DRAM" byte at @pa (NULL if not allocated). */
uint8_t *mock_pa_to_host(struct pw_os *os, uint64_t pa);
/* (e2e) CPU view backing a user mapping (mmap) of @mem: the GPU ("DRAM") bytes themselves for
 * PW_MEM_UNCACHED memory, a CPU-cache view kept coherent only by pw_os_mem_flush()/
 * pw_os_mem_invalidate() otherwise. Page aligned, valid until pw_os_mem_free(). */
void *mock_mem_user_view(struct pw_os *os, struct pw_os_mem *mem);
/* (e2e) One user mapping of the whole of @mem, released with mock_mem_user_unmap(). On Windows
 * uncached memory gets a distinct view of the same pages per call (read-only unless @writable),
 * like a real user mapping; otherwise this is mock_mem_user_view() and unmap is a no-op. */
void *mock_mem_user_map(struct pw_os *os, struct pw_os_mem *mem, bool writable);
void mock_mem_user_unmap(struct pw_os *os, struct pw_os_mem *mem, void *view);

/*
 * CSF command-stream executor (mock_csf.c). The emulated MCU acts on CSG/CS requests (START,
 * TERMINATE/SUSPEND, ENDPOINT_CONFIG, STATUS_UPDATE, doorbell_req) and, on a CS doorbell,
 * executes the ring buffer from extract to insert through the group's AS: MOVE48/32, ADD_IMM,
 * LOAD/STORE_MULTIPLE, BRANCH, CALL/JUMP (depth 8), SYNC_ADD/SET 32/64, SYNC_WAIT32/64
 * (blocks the CS until satisfied, re-evaluated whenever a CS writes memory or on
 * mock_csf_kick()), HEAP_SET, STORE_STATE, FLUSH_CACHE2/WAIT/ERROR_BARRIER/NOP and the RUN_xxx and
 * FINISH_xxx work instructions (no-ops, counted). Anything else raises CS_FATAL
 * (CS_INSTR_INVALID); GPU faults raise CS_FATAL (CS_BUS_FAULT). Sync operations raise CSG
 * SYNC_UPDATE. Execution is synchronous inside the doorbell register write.
 */
struct mock_cs_stats {
	bool started;			/* CS_STATE_START acked */
	bool fatal;
	bool blocked_sync;		/* waiting in SYNC_WAIT */
	bool blocked_oom;		/* waiting for the host to handle TILER_OOM */
	uint32_t as;			/* AS of the CSG (CSG config) */
	uint32_t doorbell;		/* CS_CONFIG doorbell id */
	uint32_t priority;		/* CS_CONFIG priority */
	uint64_t extract;		/* ring-buffer extract (linear) */
	uint64_t instrs, runs, flushes, calls, syncs, waits, doorbells;
	uint32_t fatal_type;		/* CS_FATAL exception type */
	uint64_t ringbuf_base;
	uint32_t ringbuf_size;
};
void mock_csf_cs_stats(struct pw_os *os, unsigned int csg, unsigned int cs,
		       struct mock_cs_stats *st);
/* CSG slot state: 0 idle/terminated, 1 running; *ep_req = endpoint_req as programmed. */
bool mock_csf_csg_running(struct pw_os *os, unsigned int csg, uint64_t *ep_req);
/* Re-evaluate blocked SYNC_WAITs (e.g. after the test wrote GPU memory with the CPU). */
void mock_csf_kick(struct pw_os *os);
/* The next @count RUN_TILING/RUN_IDVS instructions raise TILER_OOM with the HEAP_SET context
 * address and these counters; the CS resumes once the host acked it and rang the CSG doorbell. */
void mock_csf_inject_tiler_oom(struct pw_os *os, uint32_t count, uint32_t vt_start,
			       uint32_t vt_end, uint32_t frag_end);
/* Number of TILER_OOM events the host handled; heap_start/heap_end it gave for the last one. */
uint32_t mock_csf_oom_handled(struct pw_os *os, uint64_t *heap_start, uint64_t *heap_end);

/*
 * (jm-arch, jm-hw) Job-manager executor (mock_jm.c, G31 model only; the full model is in the
 * header of mock_jm.c). JS_COMMAND_NEXT = START latches HEAD/AFFINITY/CONFIG/FLUSH_ID_NEXT; the
 * chain moves to the current slot when it is idle and no ERR of the slot is pending; an
 * executor thread walks it through the AS in JS_CONFIG (Mesa genxml v7.xml Job Header:
 * WRITE_VALUE executed, NULL/CACHE_FLUSH and the shader job types completed without shaders,
 * exception status DONE written back), then JOB_INT DONE(js), or ERR(js) with JS_STATUS on a
 * fault/stop. GPU page faults raise an MMU fault on the AS and stall the job until the fault
 * is cleared with the page mapped (retry) or the AS disabled (job fails).
 */
struct mock_js_stats {
	uint32_t starts;		/* JS_COMMAND_NEXT = START latched */
	uint32_t done, failed, stopped;	/* DONE IRQs, faults/injected failures, soft stops */
	uint64_t last_head;		/* HEAD of the last started chain */
	uint32_t last_config;		/* CONFIG of the last started chain */
	uint32_t last_status;		/* JS_STATUS after it */
	uint64_t jobs;			/* job descriptors completed */
	/* (jm-hw) */
	uint32_t runs;			/* chains that moved from NEXT to current */
	uint32_t terminated;		/* hard stops */
	uint32_t ignored_stops;		/* stop commands that matched no running chain */
	uint32_t next_cancels;		/* JS_COMMAND_NEXT = NOP that dropped a queued chain */
	uint64_t last_affinity;		/* AFFINITY of the last started chain */
	uint32_t last_flush_id;		/* FLUSH_ID of the last started chain */
	uint64_t last_tail;		/* JS_TAIL when the last chain ended */
	uint32_t null_jobs, write_values, cache_flushes, noop_jobs;	/* by job type */
	uint32_t page_faults, fault_retries, config_faults, dep_violations;
	uint64_t last_fault_va;
	uint32_t start_flushes, start_flushes_skipped, end_flushes;	/* JS_CONFIG bookkeeping */
	uint32_t resets;		/* GPU resets seen */
	/* live state */
	bool running, next_valid, mmu_stalled, hung, stalled, err_pending;
};
void mock_jm_js_stats(struct pw_os *os, unsigned int js, struct mock_js_stats *st);

/* (jm-hw) Fault injection for the next @count chains that start on @js (0 = every chain until
 * changed; MOCK_JM_INJ_NONE clears): HANG never completes and ignores soft stops (hard stop or
 * reset ends it), STALL waits for mock_jm_kick() and then runs normally (soft stops work),
 * FAIL ends the chain at its first job with JS_STATUS = @status (header status written). */
enum mock_jm_inject {
	MOCK_JM_INJ_NONE = 0,
	MOCK_JM_INJ_HANG,
	MOCK_JM_INJ_STALL,
	MOCK_JM_INJ_FAIL,
};
void mock_jm_inject(struct pw_os *os, unsigned int js, enum mock_jm_inject what, uint32_t status,
		    uint32_t count);
void mock_jm_kick(struct pw_os *os, unsigned int js);	/* release a STALLed chain */

/* (jm-hw) The last jobs that ended on @js, oldest first (ring of 64): completed ones with their
 * type/index and status DONE; for a failed chain one record with type 0xff and the status. */
struct mock_jm_job_rec {
	uint64_t va;
	uint32_t type, index, status;
};
unsigned int mock_jm_job_log(struct pw_os *os, unsigned int js, struct mock_jm_job_rec *recs,
			     unsigned int max);
void mock_jm_job_log_clear(struct pw_os *os, unsigned int js);

/* Logging captured from pw_os_log (all levels). */
bool mock_log_contains(struct pw_os *os, const char *needle);
void mock_log_clear(struct pw_os *os);
const char *mock_last_fw_name(struct pw_os *os);

/* Resource accounting (leak checks on top of LSan). */
long mock_live_allocs(struct pw_os *os);
long mock_live_mems(struct pw_os *os);
long mock_live_sync(struct pw_os *os);	/* mutexes + events */

#endif /* MOCK_GPU_H */
