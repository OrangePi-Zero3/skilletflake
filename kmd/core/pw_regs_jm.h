/* SPDX-License-Identifier: MIT */
/*
 * pw_regs_jm.h - Mali job-manager (Midgard/Bifrost) registers that differ from or extend the
 * CSF layout in pw_regs_gpu.h / pw_regs_mmu.h. Offsets are absolute from the GPU MMIO base.
 *
 * Register offsets and bit meanings are hardware facts from the public Mali register map. They
 * were cross-checked against Linux drivers/gpu/drm/panfrost/panfrost_regs.h, which is
 * GPL-2.0-only and was read for the meaning of the registers only: no text was copied.
 * The GPU_CONTROL power/ID/feature registers, the interrupt-block layout (INT_RAWSTAT..STAT),
 * MMU_INT and the AS registers are shared with CSF and stay in pw_regs_gpu.h / pw_regs_mmu.h.
 *
 * Owner: jm-hw (pw_jm_hw.c). The mock (kmd/tests) uses its own copy of the offsets on purpose.
 */
#ifndef PW_REGS_JM_H
#define PW_REGS_JM_H

#include "pw_util.h"

/* ---- GPU_CONTROL, JM-only meanings ---- */
#define GPU_JS_PRESENT				0x1C	/* CSF: GPU_CSF_ID */
#define GPU_LATEST_FLUSH_ID			0x38
#define GPU_AFBC_FEATURES			0x4C	/* Bifrost+ */
#define GPU_PRFCNT_BASE				0x60	/* 64-bit; CSF: GPU_FEATURES */
#define GPU_JS_FEATURES(n)			(0xC0 + ((n) * 4))	/* n < 16 */
#define GPU_THREAD_TLS_ALLOC			0x310
#define GPU_STACK_PRESENT			0xE00	/* 64-bit */
#define GPU_STACK_READY				0xE10
#define GPU_STACK_PWRON				0xE20
#define GPU_STACK_PWROFF			0xE30
#define GPU_STACK_PWRTRANS			0xE40

/* Implementation-specific configuration (quirk) registers. */
#define GPU_JM_CONFIG				0xF00
#define GPU_SHADER_CONFIG			0xF04
#define GPU_TILER_CONFIG			0xF08
#define GPU_L2_MMU_CONFIG			0xF0C

/* (jm-hw) Bits of the quirk registers that the Bifrost rules in pw_jm_hw.c set (meanings from
 * the Mali register map; panfrost_regs.h read for cross-checking only). */
#define   SC_TLS_HASH_ENABLE			PW_BIT(17)	/* SHADER_CONFIG */
#define   SC_VAR_ALGORITHM			PW_BIT(29)
#define   TC_CLOCK_GATE_OVERRIDE		PW_BIT(0)	/* TILER_CONFIG */
#define   JM_FORCE_COHERENCY_FEATURES_SHIFT	2		/* JM_CONFIG */
#define   JM_IDVS_GROUP_SIZE_SHIFT		16
#define   JM_DEFAULT_IDVS_GROUP_SIZE		0xFu

/* GPU_INT bits that only exist on JM (the rest are in pw_regs_gpu.h). Bit 1 (CSF
 * PROTM_FAULT) and bits 18-19 (CSF doorbell mirror, MCU status) are not used on JM. */
#define   GPU_IRQ_MULTIPLE_FAULT		PW_BIT(7)
#define   GPU_IRQ_PERFCNT_SAMPLE_COMPLETED	PW_BIT(16)

/* GPU_CMD on JM is a plain command number (CSF packs type | payload << 8). */
#define   GPU_JM_CMD_NOP			0x00u
#define   GPU_JM_CMD_SOFT_RESET			0x01u
#define   GPU_JM_CMD_HARD_RESET			0x02u
#define   GPU_JM_CMD_PERFCNT_CLEAR		0x03u
#define   GPU_JM_CMD_PERFCNT_SAMPLE		0x04u
#define   GPU_JM_CMD_CYCLE_COUNT_START		0x05u
#define   GPU_JM_CMD_CYCLE_COUNT_STOP		0x06u
#define   GPU_JM_CMD_CLEAN_CACHES		0x07u
#define   GPU_JM_CMD_CLEAN_INV_CACHES		0x08u

/* GPU_STATUS bit set while the cycle counter runs (JM). */
#define   GPU_STATUS_CYCLE_COUNT_ACTIVE		PW_BIT(6)

/* ---- JOB_CONTROL ---- */
#define JOB_CONTROL_BASE			0x1000
/* bit n: slot n has an active (current) job; bit n + 16: slot n has a job waiting in the NEXT
 * registers. Sampled for a slot's bits when JOB_INT_CLEAR is written for that slot. */
#define JOB_INT_JS_STATE			0x1010
#define JOB_INT_THROTTLE			0x1014
/* JOB_INT bits [15:0] = job chain done on slot n, [31:16] = job chain failed on slot n. */
#define   JOB_INT_DONE(js)			PW_BIT(js)
#define   JOB_INT_ERR(js)			PW_BIT((js) + 16)
#define   JOB_INT_SLOT_MASK(js)			(JOB_INT_DONE(js) | JOB_INT_ERR(js))
/* (jm-hw) JS_STATE bits of slot js (current + NEXT): the same positions as its JOB_INT bits. */
#define   JOB_JS_STATE_SLOT_MASK(js)		JOB_INT_SLOT_MASK(js)

#define PW_JM_MAX_HW_SLOTS			16	/* register space; JS_PRESENT says which exist */

#define JS_BASE					0x1800
#define JS_SLOT_STRIDE				0x80
#define JS_REG(js, r)				(JS_BASE + (js) * JS_SLOT_STRIDE + (r))

#define JS_HEAD(js)				JS_REG(js, 0x00)	/* 64-bit, RO */
#define JS_TAIL(js)				JS_REG(js, 0x08)	/* 64-bit, RO */
#define JS_AFFINITY(js)				JS_REG(js, 0x10)	/* 64-bit */
#define JS_CONFIG(js)				JS_REG(js, 0x18)
#define JS_XAFFINITY(js)			JS_REG(js, 0x1C)
#define JS_COMMAND(js)				JS_REG(js, 0x20)
#define JS_STATUS(js)				JS_REG(js, 0x24)
#define JS_HEAD_NEXT(js)			JS_REG(js, 0x40)	/* 64-bit */
#define JS_AFFINITY_NEXT(js)			JS_REG(js, 0x50)	/* 64-bit */
#define JS_CONFIG_NEXT(js)			JS_REG(js, 0x58)
#define JS_XAFFINITY_NEXT(js)			JS_REG(js, 0x5C)
#define JS_COMMAND_NEXT(js)			JS_REG(js, 0x60)
#define JS_FLUSH_ID_NEXT(js)			JS_REG(js, 0x70)

/* JS_CONFIG(_NEXT) */
#define   JS_CONFIG_ADDRESS_SPACE(as)		((uint32_t)(as) & 0xfu)
#define   JS_CONFIG_START_FLUSH_CLEAN		PW_BIT(8)
#define   JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE (3u << 8)
#define   JS_CONFIG_START_MMU			PW_BIT(10)
#define   JS_CONFIG_JOB_CHAIN_FLAG		PW_BIT(11)
#define   JS_CONFIG_END_FLUSH_CLEAN		PW_BIT(12)
#define   JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE	(3u << 12)
#define   JS_CONFIG_ENABLE_FLUSH_REDUCTION	PW_BIT(14)
#define   JS_CONFIG_DISABLE_DESCRIPTOR_WR_BK	PW_BIT(15)
#define   JS_CONFIG_THREAD_PRI(n)		((uint32_t)(n) << 16)

/* JS_COMMAND(_NEXT) */
#define   JS_COMMAND_NOP			0x00u
#define   JS_COMMAND_START			0x01u
#define   JS_COMMAND_SOFT_STOP			0x02u
#define   JS_COMMAND_HARD_STOP			0x03u
#define   JS_COMMAND_SOFT_STOP_0		0x04u	/* only if JOB_CHAIN_FLAG == 0 */
#define   JS_COMMAND_HARD_STOP_0		0x05u
#define   JS_COMMAND_SOFT_STOP_1		0x06u	/* only if JOB_CHAIN_FLAG == 1 */
#define   JS_COMMAND_HARD_STOP_1		0x07u

/* GPU_JS_FEATURES(n): job types slot n accepts. */
#define   JS_FEATURE_NULL_JOB			PW_BIT(1)
#define   JS_FEATURE_SET_VALUE_JOB		PW_BIT(2)
#define   JS_FEATURE_CACHE_FLUSH_JOB		PW_BIT(3)
#define   JS_FEATURE_COMPUTE_JOB		PW_BIT(4)
#define   JS_FEATURE_VERTEX_JOB			PW_BIT(5)
#define   JS_FEATURE_GEOMETRY_JOB		PW_BIT(6)
#define   JS_FEATURE_TILER_JOB			PW_BIT(7)
#define   JS_FEATURE_FUSED_JOB			PW_BIT(8)
#define   JS_FEATURE_FRAGMENT_JOB		PW_BIT(9)

/* AS_TRANSCFG address mode of the pre-AArch64 "Mali LPAE" page-table format (not used by us;
 * see docs/status/jm-arch.md, open question on the page-table format). */
#define   AS_TRANSCFG_ADRMODE_LEGACY		(0ull << 0)

/*
 * JS_STATUS / exception codes that only exist on JM (the shared ones, OK/TERMINATED/KABOOM/
 * ACTIVE, the instruction, data, OOM and MMU faults, are enum pw_exception_type). Codes 0x40-0x4b
 * mean JOB_* faults here but CS_* faults on CSF, so name lookups must be backend aware.
 */
enum pw_jm_exception_type {
	PW_JM_EXC_DONE = 0x01,
	PW_JM_EXC_INTERRUPTED = 0x02,
	PW_JM_EXC_STOPPED = 0x03,
	PW_JM_EXC_JOB_CONFIG_FAULT = 0x40,
	PW_JM_EXC_JOB_POWER_FAULT = 0x41,
	PW_JM_EXC_JOB_READ_FAULT = 0x42,
	PW_JM_EXC_JOB_WRITE_FAULT = 0x43,
	PW_JM_EXC_JOB_AFFINITY_FAULT = 0x44,
	PW_JM_EXC_JOB_BUS_FAULT = 0x48,
	PW_JM_EXC_INSTR_TYPE_MISMATCH = 0x52,
	PW_JM_EXC_INSTR_OPERAND_FAULT = 0x53,
	PW_JM_EXC_INSTR_TLS_FAULT = 0x54,
	PW_JM_EXC_INSTR_ALIGN_FAULT = 0x56,
	PW_JM_EXC_OOM_AFBC = 0x61,
	PW_JM_EXC_UNKNOWN = 0x7f,
	PW_JM_EXC_DELAYED_BUS_FAULT = 0x80,
	PW_JM_EXC_TRANSLATION_FAULT_IDENTITY = 0xc7,
	PW_JM_EXC_TRANSTAB_BUS_FAULT_0 = 0xd0,
	PW_JM_EXC_TRANSTAB_BUS_FAULT_1 = 0xd1,
	PW_JM_EXC_TRANSTAB_BUS_FAULT_2 = 0xd2,
	PW_JM_EXC_TRANSTAB_BUS_FAULT_3 = 0xd3,
	PW_JM_EXC_ACCESS_FLAG_0 = 0xd8,
};

#endif /* PW_REGS_JM_H */
