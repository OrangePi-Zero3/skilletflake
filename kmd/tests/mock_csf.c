// SPDX-License-Identifier: MIT
/*
 * mock_csf.c - CSF command-stream executor for the mock MCU (see mock_gpu.h).
 *
 * Instruction encodings are taken from Mesa src/panfrost/genxml/v10.xml (MIT): opcode in bits
 * 63:56, and per instruction the fields below. The interface offsets are written out here on
 * purpose instead of using the core's pw_fw_iface.h, so a layout mistake in the core shows up
 * as a test failure. Everything runs under the mock lock, synchronously in the register write
 * that rang a doorbell.
 */

#include <stdlib.h>
#include <string.h>

#include "mock_internal.h"

#define MCS_REGS		128
#define MCS_CALL_DEPTH		8
#define MCS_BUDGET		(1u << 20)	/* instructions per run, then yield */

/* CSG input/output fields */
#define CSGI_REQ		0x00
#define CSGI_ACK_IRQ_MASK	0x04
#define CSGI_DB_REQ		0x08
#define CSGI_CS_IRQ_ACK		0x0c
#define CSGI_EP_REQ		0x34
#define CSGI_EP_REQ2		0x38
#define CSGI_CONFIG		0x50
#define CSGO_ACK		0x00
#define CSGO_DB_ACK		0x08
#define CSGO_CS_IRQ_REQ		0x0c
/* CS input/output fields */
#define CSI_REQ			0x00
#define CSI_CONFIG		0x04
#define CSI_ACK_IRQ_MASK	0x0c
#define CSI_RB_BASE		0x10
#define CSI_RB_SIZE		0x18
#define CSI_HEAP_START		0x20
#define CSI_HEAP_END		0x28
#define CSI_RB_INPUT		0x30
#define CSI_RB_OUTPUT		0x38
#define CSO_ACK			0x00
#define CSO_STATUS_WAIT		0x48
#define CSO_WAIT_SYNC_PTR	0x50
#define CSO_WAIT_SYNC_VALUE	0x58
#define CSO_BLOCKED_REASON	0x60
#define CSO_WAIT_SYNC_VALUE_HI	0x64
#define CSO_FATAL		0x84
#define CSO_FATAL_INFO		0x90
#define CSO_HEAP_VT_START	0xc0
#define CSO_HEAP_VT_END		0xc4
#define CSO_HEAP_FRAG_END	0xcc
#define CSO_HEAP_ADDRESS	0xd0

/* req/ack bits */
#define CSG_STATE_MASK		0x7u
#define CSG_REQ_TOGGLES		(0x10u | 0x20u)	/* ENDPOINT_CONFIG, STATUS_UPDATE */
#define CSG_SYNC_UPDATE		(1u << 28)
#define CS_STATE_MASK		0x7u
#define CS_REQ_ECHO		(0x7u | 0x10u | 0xf00u)	/* state, EXTRACT_EVENT, IDLE_* */
#define CS_TILER_OOM		(1u << 26)
#define CS_FATAL		(1u << 30)

#define EXC_CS_BUS_FAULT		0x48
#define EXC_CS_INSTR_INVALID		0x49
#define EXC_CS_CALL_STACK_OVERFLOW	0x4a

enum mcs_block { MCS_RUNNING = 0, MCS_SYNC_WAIT, MCS_OOM };

struct mock_cs {
	bool started, fatal;
	int blocked;
	uint32_t regs[MCS_REGS];
	struct {
		uint64_t pc, end;
	} fr[MCS_CALL_DEPTH + 1];	/* fr[0]: ring, linear offsets */
	int depth;
	uint64_t heap_ctx;
	struct mock_cs_stats st;
};

struct mock_csg {
	bool running;
	uint32_t as;
	bool sync_dirty;
	uint64_t ep_req;
	struct mock_cs cs[MOCK_FW_STREAM_NUM];
};

struct mock_csf {
	struct mock_csg csg[MOCK_FW_GROUP_NUM];
	uint32_t oom_inject;
	uint32_t oom_vt_start, oom_vt_end, oom_frag_end;
	uint32_t oom_handled;
	uint64_t oom_heap_start, oom_heap_end;
};

struct mock_csf *mock_csf_create(void)
{
	return calloc(1, sizeof(struct mock_csf));
}

void mock_csf_destroy(struct mock_csf *csf)
{
	free(csf);
}

void mock_csf_reset_locked(struct pw_os *os)
{
	struct mock_csf *csf = mock_csf_of(os);

	memset(csf->csg, 0, sizeof(csf->csg));
}

/* ---- shared-section and GPU memory helpers ---- */

static uint32_t r32(struct pw_os *os, uint32_t off)
{
	return mock_sh_r32_locked(os, off);
}

static void w32(struct pw_os *os, uint32_t off, uint32_t v)
{
	mock_sh_w32_locked(os, off, v);
}

static uint64_t r64(struct pw_os *os, uint32_t off)
{
	return r32(os, off) | ((uint64_t)r32(os, off + 4) << 32);
}

static void w64(struct pw_os *os, uint32_t off, uint64_t v)
{
	w32(os, off, (uint32_t)v);
	w32(os, off + 4, (uint32_t)(v >> 32));
}

static uint64_t reg64(const struct mock_cs *cs, uint32_t r)
{
	return cs->regs[r] | ((uint64_t)cs->regs[r + 1] << 32);
}

/* ---- events ---- */

static void raise_csg_irq(struct pw_os *os, unsigned int i)
{
	mock_raise_job_locked(os, 1u << i);
}

/* Toggle CS ack bits for an event and the CSG's cs_irq_req bit, raise the CSG interrupt. */
static void cs_event(struct pw_os *os, unsigned int i, unsigned int j, uint32_t bits)
{
	w32(os, CS_OUT(i, j) + CSO_ACK, r32(os, CS_OUT(i, j) + CSO_ACK) ^ bits);
	if (r32(os, CS_IN(i, j) + CSI_ACK_IRQ_MASK) & bits) {
		w32(os, CSG_OUT(i) + CSGO_CS_IRQ_REQ,
		    r32(os, CSG_OUT(i) + CSGO_CS_IRQ_REQ) ^ (1u << j));
		raise_csg_irq(os, i);
	}
}

static void cs_fatal(struct pw_os *os, unsigned int i, unsigned int j, uint32_t type,
		     uint64_t info)
{
	struct mock_cs *cs = &mock_csf_of(os)->csg[i].cs[j];

	cs->fatal = true;
	cs->st.fatal = true;
	cs->st.fatal_type = type;
	w32(os, CS_OUT(i, j) + CSO_FATAL, type);
	w64(os, CS_OUT(i, j) + CSO_FATAL_INFO, info);
	cs_event(os, i, j, CS_FATAL);
}

/* ---- the interpreter ---- */

static bool cond_holds(uint32_t cond, int64_t a, int64_t b, bool is_signed)
{
	uint64_t ua = (uint64_t)a, ub = (uint64_t)b;

	switch (cond) {
	case 0: return is_signed ? a <= b : ua <= ub;	/* Lequal */
	case 1: return is_signed ? a > b : ua > ub;	/* Greater */
	case 2: return a == b;				/* Equal */
	case 3: return a != b;				/* Nequal */
	case 4: return is_signed ? a < b : ua < ub;	/* Less */
	case 5: return is_signed ? a >= b : ua >= ub;	/* Gequal */
	case 6: return true;				/* Always */
	default: return false;
	}
}

enum exec_res { EX_NEXT, EX_DONE_PC, EX_BLOCK, EX_FATAL };

/* Execute one instruction of CS (i, j) at @pc. EX_NEXT: advance pc by 8. EX_DONE_PC: the
 * instruction updated the frames itself. */
static enum exec_res exec_one(struct pw_os *os, unsigned int i, unsigned int j, uint64_t ins,
			      uint64_t pc, bool *synced)
{
	struct mock_csf *csf = mock_csf_of(os);
	struct mock_csg *csg = &csf->csg[i];
	struct mock_cs *cs = &csg->cs[j];
	uint32_t op = (uint32_t)(ins >> 56);
	uint32_t r48 = (uint32_t)(ins >> 48) & 0xff;	/* Destination / Base Register */
	uint32_t r40 = (uint32_t)(ins >> 40) & 0xff;	/* Address / Source / Value */
	uint32_t r32_ = (uint32_t)(ins >> 32) & 0xff;	/* Data / Length */

	/* Register operands must be in range (a pair where 64-bit). */
	switch (op) {
	case 1:		/* bits 47:0 are the immediate */
		if (r48 + 1 >= MCS_REGS)
			goto bad;
		break;
	case 16: case 17:
		if (r48 + 1 >= MCS_REGS || r40 + 1 >= MCS_REGS)
			goto bad;
		break;
	case 20: case 21: case 22: case 32: case 33: case 37: case 38: case 39: case 40:
	case 48: case 51: case 52: case 53:
		if (r40 + 1 >= MCS_REGS || r32_ + 1 >= MCS_REGS)
			goto bad;
		break;
	case 2:
		if (r48 >= MCS_REGS)
			goto bad;
		break;
	default:
		break;
	}

	switch (op) {
	case 0:		/* NOP */
	case 3:		/* WAIT: every asynchronous operation completes synchronously here */
	case 9:		/* FINISH_TILING */
	case 11:	/* FINISH_FRAGMENT */
	case 23:	/* SET_SB_ENTRY */
	case 25:	/* SET_EXCEPTION_HANDLER */
	case 34:	/* REQ_RESOURCE */
	case 47:	/* ERROR_BARRIER */
	case 49:	/* HEAP_OPERATION */
	case 50:	/* TRACE_POINT */
		return EX_NEXT;
	case 1:		/* MOVE48 */
		cs->regs[r48] = (uint32_t)ins;
		cs->regs[r48 + 1] = (uint32_t)(ins >> 32) & 0xffff;
		return EX_NEXT;
	case 2:		/* MOVE32 */
		cs->regs[r48] = (uint32_t)ins;
		return EX_NEXT;
	case 4: case 6: case 7: case 8: case 44:	/* RUN_COMPUTE/IDVS/FRAGMENT/FULLSCREEN/
							 * COMPUTE_INDIRECT */
	case 5:					/* RUN_TILING */
		cs->st.runs++;
		if ((op == 5 || op == 6) && csf->oom_inject) {
			csf->oom_inject--;
			w64(os, CS_OUT(i, j) + CSO_HEAP_ADDRESS, cs->heap_ctx);
			w32(os, CS_OUT(i, j) + CSO_HEAP_VT_START, csf->oom_vt_start);
			w32(os, CS_OUT(i, j) + CSO_HEAP_VT_END, csf->oom_vt_end);
			w32(os, CS_OUT(i, j) + CSO_HEAP_FRAG_END, csf->oom_frag_end);
			cs->blocked = MCS_OOM;
			cs_event(os, i, j, CS_TILER_OOM);
			/* Resume after this instruction once the host answered. */
			cs->fr[cs->depth].pc = pc + 8;
			return EX_BLOCK;
		}
		return EX_NEXT;
	case 16:	/* ADD_IMM32 */
		cs->regs[r48] = cs->regs[r40] + (uint32_t)ins;
		return EX_NEXT;
	case 17: {	/* ADD_IMM64 */
		uint64_t v = reg64(cs, r40) + (uint64_t)(int64_t)(int32_t)(uint32_t)ins;

		cs->regs[r48] = (uint32_t)v;
		cs->regs[r48 + 1] = (uint32_t)(v >> 32);
		return EX_NEXT;
	}
	case 20:	/* LOAD_MULTIPLE */
	case 21: {	/* STORE_MULTIPLE */
		uint32_t mask = (uint32_t)(ins >> 16) & 0xffff;
		uint64_t addr = reg64(cs, r40) + (uint64_t)(int64_t)(int16_t)(ins & 0xffff);

		for (uint32_t k = 0; k < 16; k++) {
			if (!(mask & (1u << k)))
				continue;
			if (r48 + k >= MCS_REGS)
				goto bad;
			if (mock_gpu_rw_locked(os, csg->as, addr + 4 * k, &cs->regs[r48 + k], 4,
					       op == 21)) {
				cs_fatal(os, i, j, EXC_CS_BUS_FAULT, addr + 4 * k);
				return EX_FATAL;
			}
		}
		return EX_NEXT;
	}
	case 22: {	/* BRANCH: offset in instructions, relative to the next one */
		int32_t off = (int16_t)(ins & 0xffff);
		uint32_t cond = (uint32_t)(ins >> 28) & 0xf;

		if (cond_holds(cond, (int32_t)cs->regs[r40], 0, true)) {
			cs->fr[cs->depth].pc = pc + 8 + (int64_t)off * 8;
			return EX_DONE_PC;
		}
		return EX_NEXT;
	}
	case 32:	/* CALL */
	case 33: {	/* JUMP */
		uint64_t addr = reg64(cs, r40);
		uint32_t len = cs->regs[r32_];

		if (op == 33) {
			if (!cs->depth)
				goto bad;	/* the kernel ring never jumps */
			cs->fr[cs->depth].pc = addr;
			cs->fr[cs->depth].end = addr + len;
			return EX_DONE_PC;
		}
		cs->st.calls++;
		if (!len)
			return EX_NEXT;
		if (cs->depth == MCS_CALL_DEPTH) {
			cs_fatal(os, i, j, EXC_CS_CALL_STACK_OVERFLOW, pc);
			return EX_FATAL;
		}
		/* The caller's pc moves past the CALL when the callee returns. */
		cs->fr[cs->depth].pc = pc;
		cs->depth++;
		cs->fr[cs->depth].pc = addr;
		cs->fr[cs->depth].end = addr + len;
		return EX_DONE_PC;
	}
	case 36:	/* FLUSH_CACHE2 */
		cs->st.flushes++;
		return EX_NEXT;
	case 37: case 38: case 51: case 52: {	/* SYNC_ADD32/SET32/ADD64/SET64 */
		bool is64 = op >= 51;
		uint64_t addr = reg64(cs, r40), v = 0;
		uint64_t data = is64 ? reg64(cs, r32_) : cs->regs[r32_];

		if (mock_gpu_rw_locked(os, csg->as, addr, &v, is64 ? 8 : 4, false)) {
			cs_fatal(os, i, j, EXC_CS_BUS_FAULT, addr);
			return EX_FATAL;
		}
		v = (op == 37 || op == 51) ? v + data : data;
		if (mock_gpu_rw_locked(os, csg->as, addr, &v, is64 ? 8 : 4, true)) {
			cs_fatal(os, i, j, EXC_CS_BUS_FAULT, addr);
			return EX_FATAL;
		}
		cs->st.syncs++;
		csg->sync_dirty = true;
		*synced = true;
		return EX_NEXT;
	}
	case 39: case 53: {	/* SYNC_WAIT32/64 */
		bool is64 = op == 53;
		uint64_t addr = reg64(cs, r40), v = 0;
		uint64_t ref = is64 ? reg64(cs, r32_) : cs->regs[r32_];
		uint32_t cond = (uint32_t)(ins >> 28) & 0xf;

		if (mock_gpu_rw_locked(os, csg->as, addr, &v, is64 ? 8 : 4, false)) {
			cs_fatal(os, i, j, EXC_CS_BUS_FAULT, addr);
			return EX_FATAL;
		}
		if (cond_holds(cond, (int64_t)v, (int64_t)ref, false)) {
			if (cs->blocked == MCS_SYNC_WAIT) {
				cs->blocked = MCS_RUNNING;
				w32(os, CS_OUT(i, j) + CSO_BLOCKED_REASON, 0);
				w32(os, CS_OUT(i, j) + CSO_STATUS_WAIT, 0);
			}
			return EX_NEXT;
		}
		if (cs->blocked != MCS_SYNC_WAIT)
			cs->st.waits++;
		cs->blocked = MCS_SYNC_WAIT;
		/* CS_STATUS_WAIT: SYNC | 64B | condition, as panthor reads it (sched.c:1213). */
		w32(os, CS_OUT(i, j) + CSO_STATUS_WAIT,
		    (1u << 31) | (is64 ? 1u << 30 : 0) | (cond << 24));
		w64(os, CS_OUT(i, j) + CSO_WAIT_SYNC_PTR, addr);
		w32(os, CS_OUT(i, j) + CSO_WAIT_SYNC_VALUE, (uint32_t)ref);
		w32(os, CS_OUT(i, j) + CSO_WAIT_SYNC_VALUE_HI, (uint32_t)(ref >> 32));
		w32(os, CS_OUT(i, j) + CSO_BLOCKED_REASON, 3);	/* SYNC_WAIT */
		cs->fr[cs->depth].pc = pc;	/* re-executed when re-evaluated */
		return EX_BLOCK;
	}
	case 40: {	/* STORE_STATE: 64-bit counter stand-in for the timestamp/cycles */
		uint64_t addr = reg64(cs, r40) + (uint64_t)(int64_t)(int16_t)(ins & 0xffff);
		uint64_t v = cs->st.instrs;

		if (mock_gpu_rw_locked(os, csg->as, addr, &v, 8, true)) {
			cs_fatal(os, i, j, EXC_CS_BUS_FAULT, addr);
			return EX_FATAL;
		}
		return EX_NEXT;
	}
	case 48:	/* HEAP_SET */
		cs->heap_ctx = reg64(cs, r40);
		return EX_NEXT;
	default:
		break;
	}
bad:
	cs_fatal(os, i, j, EXC_CS_INSTR_INVALID, pc);
	return EX_FATAL;
}

/* Run CS (i, j) until it is idle, blocked or faulted. Returns true if it made progress. */
static bool cs_run(struct pw_os *os, unsigned int i, unsigned int j)
{
	struct mock_csg *csg = &mock_csf_of(os)->csg[i];
	struct mock_cs *cs = &csg->cs[j];
	uint64_t base, in_va, out_va, insert = 0;
	uint32_t size, budget = MCS_BUDGET;
	bool progress = false, synced = false;

	if (!csg->running || !cs->started || cs->fatal || cs->blocked == MCS_OOM)
		return false;

	base = r64(os, CS_IN(i, j) + CSI_RB_BASE);
	size = r32(os, CS_IN(i, j) + CSI_RB_SIZE);
	in_va = r64(os, CS_IN(i, j) + CSI_RB_INPUT);
	out_va = r64(os, CS_IN(i, j) + CSI_RB_OUTPUT);
	cs->st.ringbuf_base = base;
	cs->st.ringbuf_size = size;
	if (!size || (size & (size - 1))) {
		mock_model_error(os, "CS %u/%u: ring buffer size 0x%x", i, j, size);
		cs_fatal(os, i, j, EXC_CS_BUS_FAULT, 0);
		return false;
	}
	if (mock_gpu_rw_locked(os, 0, in_va, &insert, 8, false)) {
		mock_model_error(os, "CS %u/%u: ring input iface 0x%llx not mapped in AS0", i, j,
				 (unsigned long long)in_va);
		cs_fatal(os, i, j, EXC_CS_BUS_FAULT, in_va);
		return false;
	}
	if (insert < cs->fr[0].pc || insert - cs->fr[0].pc > size) {
		mock_model_error(os, "CS %u/%u: insert 0x%llx vs extract 0x%llx", i, j,
				 (unsigned long long)insert, (unsigned long long)cs->fr[0].pc);
		cs_fatal(os, i, j, EXC_CS_BUS_FAULT, insert);
		return false;
	}
	cs->fr[0].end = insert;

	while (budget--) {
		uint64_t pc, ins, va;
		enum exec_res r;

		if (cs->depth && cs->fr[cs->depth].pc >= cs->fr[cs->depth].end) {
			cs->depth--;		/* return from CALL */
			cs->fr[cs->depth].pc += 8;
			continue;
		}
		pc = cs->fr[cs->depth].pc;
		if (!cs->depth && pc >= insert)
			break;			/* idle */
		va = cs->depth ? pc : base + (pc & (size - 1));
		if (mock_gpu_rw_locked(os, csg->as, va, &ins, 8, false)) {
			cs_fatal(os, i, j, EXC_CS_BUS_FAULT, va);
			break;
		}
		r = exec_one(os, i, j, ins, pc, &synced);
		if (r == EX_BLOCK || r == EX_FATAL)
			break;
		progress = true;
		cs->st.instrs++;
		if (r == EX_NEXT)
			cs->fr[cs->depth].pc = pc + 8;
	}
	/* The extract pointer stays on a CALL until the callee returns. */
	if (mock_gpu_rw_locked(os, 0, out_va, &cs->fr[0].pc, 8, true))
		mock_model_error(os, "CS %u/%u: ring output iface not writable", i, j);
	cs->st.extract = cs->fr[0].pc;
	(void)synced;
	return progress;
}

/* Re-run blocked CSs until nothing moves, then report SYNC_UPDATE for CSGs that wrote syncs. */
static void settle(struct pw_os *os)
{
	struct mock_csf *csf = mock_csf_of(os);

	for (unsigned int round = 0; round < 64; round++) {
		bool progress = false;

		for (unsigned int i = 0; i < MOCK_FW_GROUP_NUM; i++) {
			for (unsigned int j = 0; j < MOCK_FW_STREAM_NUM; j++) {
				if (csf->csg[i].cs[j].blocked == MCS_SYNC_WAIT &&
				    cs_run(os, i, j))
					progress = true;
			}
		}
		if (!progress)
			break;
	}
	for (unsigned int i = 0; i < MOCK_FW_GROUP_NUM; i++) {
		if (!csf->csg[i].sync_dirty)
			continue;
		csf->csg[i].sync_dirty = false;
		w32(os, CSG_OUT(i) + CSGO_ACK, r32(os, CSG_OUT(i) + CSGO_ACK) ^ CSG_SYNC_UPDATE);
		if (r32(os, CSG_IN(i) + CSGI_ACK_IRQ_MASK) & CSG_SYNC_UPDATE)
			raise_csg_irq(os, i);
	}
}

/* A CS request (doorbell_req toggled for it): state changes, OOM resume, then run. */
static void cs_request(struct pw_os *os, unsigned int i, unsigned int j)
{
	struct mock_csf *csf = mock_csf_of(os);
	struct mock_cs *cs = &csf->csg[i].cs[j];
	uint32_t req = r32(os, CS_IN(i, j) + CSI_REQ);
	uint32_t ack = r32(os, CS_OUT(i, j) + CSO_ACK);
	uint32_t cfg = r32(os, CS_IN(i, j) + CSI_CONFIG);

	cs->st.doorbell = (cfg >> 8) & 0xff;
	cs->st.priority = cfg & 0xf;
	if ((req ^ ack) & CS_STATE_MASK) {
		if ((req & CS_STATE_MASK) == 1) {	/* START */
			uint64_t out_va = r64(os, CS_IN(i, j) + CSI_RB_OUTPUT), extract = 0;

			if (mock_gpu_rw_locked(os, 0, out_va, &extract, 8, false))
				mock_model_error(os, "CS %u/%u: START without a ring output iface",
						 i, j);
			memset(cs->fr, 0, sizeof(cs->fr));
			cs->fr[0].pc = extract;
			cs->depth = 0;
			cs->started = true;
			cs->fatal = false;
			cs->blocked = MCS_RUNNING;
		} else {				/* STOP */
			cs->started = false;
		}
	}
	w32(os, CS_OUT(i, j) + CSO_ACK, (ack & ~CS_REQ_ECHO) | (req & CS_REQ_ECHO));
	cs->st.started = cs->started;

	/* The host answered TILER_OOM: req bit == ack bit again (sched.c:1569). */
	if (cs->blocked == MCS_OOM && !((req ^ ack) & CS_TILER_OOM)) {
		csf->oom_handled++;
		csf->oom_heap_start = r64(os, CS_IN(i, j) + CSI_HEAP_START);
		csf->oom_heap_end = r64(os, CS_IN(i, j) + CSI_HEAP_END);
		cs->blocked = MCS_RUNNING;
	}
	cs_run(os, i, j);
}

/* A CSG request: state, ENDPOINT_CONFIG/STATUS_UPDATE toggles, then CS doorbells. */
static void csg_request(struct pw_os *os, unsigned int i)
{
	struct mock_csg *csg = &mock_csf_of(os)->csg[i];
	uint32_t req = r32(os, CSG_IN(i) + CSGI_REQ);
	uint32_t ack = r32(os, CSG_OUT(i) + CSGO_ACK);
	uint32_t new_ack = ack, db_req, db_toggle;

	if ((req ^ ack) & CSG_STATE_MASK) {
		uint32_t state = req & CSG_STATE_MASK;

		if (state == 1 || state == 3) {		/* START / RESUME */
			csg->running = true;
			csg->as = r32(os, CSG_IN(i) + CSGI_CONFIG) & 0xf;
			csg->ep_req = r32(os, CSG_IN(i) + CSGI_EP_REQ);
			if (!csg->as)
				mock_model_error(os, "CSG %u started on AS0 (the MCU's)", i);
		} else {				/* TERMINATE / SUSPEND */
			csg->running = false;
			/* Every CS of the group stops: its ack state reads STOP, so the host's
			 * next CS START (at the next CSG START, sched.c:1324) is a request. */
			for (unsigned int j = 0; j < MOCK_FW_STREAM_NUM; j++) {
				uint32_t a = r32(os, CS_OUT(i, j) + CSO_ACK);

				w32(os, CS_OUT(i, j) + CSO_ACK, a & ~CS_STATE_MASK);
				csg->cs[j].started = false;
				csg->cs[j].st.started = false;
				csg->cs[j].blocked = MCS_RUNNING;
			}
		}
		new_ack = (new_ack & ~CSG_STATE_MASK) | state;
	}
	new_ack = (new_ack & ~CSG_REQ_TOGGLES) | (req & CSG_REQ_TOGGLES);
	if (new_ack != ack) {
		w32(os, CSG_OUT(i) + CSGO_ACK, new_ack);
		if ((new_ack ^ ack) & r32(os, CSG_IN(i) + CSGI_ACK_IRQ_MASK))
			raise_csg_irq(os, i);
	}

	db_req = r32(os, CSG_IN(i) + CSGI_DB_REQ);
	db_toggle = db_req ^ r32(os, CSG_OUT(i) + CSGO_DB_ACK);
	w32(os, CSG_OUT(i) + CSGO_DB_ACK, db_req);
	for (unsigned int j = 0; j < MOCK_FW_STREAM_NUM; j++) {
		if (db_toggle & (1u << j)) {
			csg->cs[j].st.doorbells++;
			if (csg->running)
				cs_request(os, i, j);
		}
	}
}

void mock_csf_glb_doorbell_locked(struct pw_os *os, uint32_t csg_mask)
{
	for (unsigned int i = 0; i < MOCK_FW_GROUP_NUM; i++) {
		if (csg_mask & (1u << i))
			csg_request(os, i);
	}
	settle(os);
}

void mock_csf_doorbell_locked(struct pw_os *os, uint32_t id)
{
	struct mock_csf *csf = mock_csf_of(os);

	for (unsigned int i = 0; i < MOCK_FW_GROUP_NUM; i++) {
		if (!csf->csg[i].running)
			continue;
		for (unsigned int j = 0; j < MOCK_FW_STREAM_NUM; j++) {
			struct mock_cs *cs = &csf->csg[i].cs[j];

			if (!cs->started ||
			    ((r32(os, CS_IN(i, j) + CSI_CONFIG) >> 8) & 0xff) != id)
				continue;
			cs->st.doorbells++;
			cs_run(os, i, j);
		}
	}
	settle(os);
}

/* ---- test API ---- */

void mock_csf_cs_stats(struct pw_os *os, unsigned int csg, unsigned int cs,
		       struct mock_cs_stats *st)
{
	struct mock_csf *csf;

	mock_lock(os);
	csf = mock_csf_of(os);
	memset(st, 0, sizeof(*st));
	if (csg < MOCK_FW_GROUP_NUM && cs < MOCK_FW_STREAM_NUM) {
		const struct mock_cs *c = &csf->csg[csg].cs[cs];

		*st = c->st;
		st->as = csf->csg[csg].as;
		st->blocked_sync = c->blocked == MCS_SYNC_WAIT;
		st->blocked_oom = c->blocked == MCS_OOM;
		st->extract = c->fr[0].pc;
	}
	mock_unlock(os);
}

bool mock_csf_csg_running(struct pw_os *os, unsigned int csg, uint64_t *ep_req)
{
	bool r = false;

	mock_lock(os);
	if (csg < MOCK_FW_GROUP_NUM) {
		r = mock_csf_of(os)->csg[csg].running;
		if (ep_req)
			*ep_req = mock_csf_of(os)->csg[csg].ep_req;
	}
	mock_unlock(os);
	return r;
}

void mock_csf_kick(struct pw_os *os)
{
	mock_lock(os);
	settle(os);
	mock_unlock(os);
}

void mock_csf_inject_tiler_oom(struct pw_os *os, uint32_t count, uint32_t vt_start,
			       uint32_t vt_end, uint32_t frag_end)
{
	struct mock_csf *csf;

	mock_lock(os);
	csf = mock_csf_of(os);
	csf->oom_inject = count;
	csf->oom_vt_start = vt_start;
	csf->oom_vt_end = vt_end;
	csf->oom_frag_end = frag_end;
	mock_unlock(os);
}

uint32_t mock_csf_oom_handled(struct pw_os *os, uint64_t *heap_start, uint64_t *heap_end)
{
	uint32_t n;

	mock_lock(os);
	n = mock_csf_of(os)->oom_handled;
	if (heap_start)
		*heap_start = mock_csf_of(os)->oom_heap_start;
	if (heap_end)
		*heap_end = mock_csf_of(os)->oom_heap_end;
	mock_unlock(os);
	return n;
}
