/* SPDX-License-Identifier: MIT */
/*
 * pw_regs_gpu.h - Mali Valhall/CSF GPU_CONTROL register block (offsets from the GPU MMIO base).
 *
 * Derived from Linux panthor (dual GPL-2.0/MIT, used under MIT):
 *   drivers/gpu/drm/panthor/panthor_gpu_regs.h (Copyright 2026 ARM Limited).
 * Interrupt-block layout (RAWSTAT/CLEAR/MASK/STAT) from panthor_device.h:651-654.
 */
#ifndef PW_REGS_GPU_H
#define PW_REGS_GPU_H

#include "pw_util.h"

/* Generic interrupt block layout, shared by GPU_INT, JOB_INT and MMU_INT. */
#define INT_RAWSTAT				0x0
#define INT_CLEAR				0x4
#define INT_MASK				0x8
#define INT_STAT				0xc

#define GPU_CONTROL_BASE			0x0

#define GPU_ID					0x0
#define   GPU_ARCH_MAJOR(x)			((x) >> 28)
#define   GPU_ARCH_MINOR(x)			(((x) & PW_GENMASK(27, 24)) >> 24)
#define   GPU_ARCH_REV(x)			(((x) & PW_GENMASK(23, 20)) >> 20)
#define   GPU_PROD_MAJOR(x)			(((x) & PW_GENMASK(19, 16)) >> 16)
#define   GPU_VER_MAJOR(x)			(((x) & PW_GENMASK(15, 12)) >> 12)
#define   GPU_VER_MINOR(x)			(((x) & PW_GENMASK(11, 4)) >> 4)
#define   GPU_VER_STATUS(x)			((x) & PW_GENMASK(3, 0))

#define GPU_L2_FEATURES				0x4
#define  GPU_L2_FEATURES_LINE_SIZE(x)		(1u << ((x) & PW_GENMASK(7, 0)))
#define GPU_CORE_FEATURES			0x8
#define GPU_TILER_FEATURES			0xC
#define GPU_MEM_FEATURES			0x10
#define   GROUPS_L2_COHERENT			PW_BIT(0)
#define GPU_MMU_FEATURES			0x14
#define  GPU_MMU_FEATURES_VA_BITS(x)		((x) & PW_GENMASK(7, 0))
#define  GPU_MMU_FEATURES_PA_BITS(x)		(((x) >> 8) & PW_GENMASK(7, 0))
#define GPU_AS_PRESENT				0x18
#define GPU_CSF_ID				0x1C

#define GPU_INT_BASE				0x20
#define   GPU_IRQ_FAULT				PW_BIT(0)
#define   GPU_IRQ_PROTM_FAULT			PW_BIT(1)
#define   GPU_IRQ_RESET_COMPLETED		PW_BIT(8)
#define   GPU_IRQ_POWER_CHANGED			PW_BIT(9)
#define   GPU_IRQ_POWER_CHANGED_ALL		PW_BIT(10)
#define   GPU_IRQ_CLEAN_CACHES_COMPLETED	PW_BIT(17)
#define   GPU_IRQ_DOORBELL_MIRROR		PW_BIT(18)
#define   GPU_IRQ_MCU_STATUS_CHANGED		PW_BIT(19)

#define GPU_CMD					0x30
#define   GPU_CMD_DEF(type, payload)		((type) | ((payload) << 8))
#define   GPU_SOFT_RESET			GPU_CMD_DEF(1u, 1u)
#define   GPU_HARD_RESET			GPU_CMD_DEF(1u, 2u)
#define   CACHE_CLEAN				PW_BIT(0)
#define   CACHE_INV				PW_BIT(1)
#define   GPU_FLUSH_CACHES(l2, lsc, oth)	\
	GPU_CMD_DEF(4u, ((l2) << 0) | ((lsc) << 4) | ((oth) << 8))

#define GPU_STATUS				0x34
#define   GPU_STATUS_ACTIVE			PW_BIT(0)
#define   GPU_STATUS_PWR_ACTIVE			PW_BIT(1)
#define   GPU_STATUS_PAGE_FAULT			PW_BIT(4)
#define   GPU_STATUS_PROTM_ACTIVE		PW_BIT(7)
#define   GPU_STATUS_DBG_ENABLED		PW_BIT(8)

#define GPU_FAULT_STATUS			0x3C
#define GPU_FAULT_ADDR				0x40	/* 64-bit */
#define GPU_L2_CONFIG				0x48
#define   GPU_L2_CONFIG_ASN_HASH_ENABLE		PW_BIT(24)

#define GPU_PWR_KEY				0x50
#define  GPU_PWR_KEY_UNLOCK			0x2968A819
#define GPU_PWR_OVERRIDE0			0x54
#define GPU_PWR_OVERRIDE1			0x58

#define GPU_FEATURES				0x60	/* 64-bit, arch 11+ (reads 0 earlier) */
#define   GPU_FEATURES_RAY_INTERSECTION		PW_BIT64(2)
#define   GPU_FEATURES_RAY_TRAVERSAL		PW_BIT64(5)

#define GPU_TIMESTAMP_OFFSET			0x88	/* 64-bit */
#define GPU_CYCLE_COUNT				0x90	/* 64-bit */
#define GPU_TIMESTAMP				0x98	/* 64-bit */

#define GPU_THREAD_MAX_THREADS			0xA0
#define GPU_THREAD_MAX_WORKGROUP_SIZE		0xA4
#define GPU_THREAD_MAX_BARRIER_SIZE		0xA8
#define GPU_THREAD_FEATURES			0xAC

#define GPU_TEXTURE_FEATURES(n)			(0xB0 + ((n) * 4))

/* 64-bit LO/HI pairs */
#define GPU_SHADER_PRESENT			0x100
#define GPU_TILER_PRESENT			0x110
#define GPU_L2_PRESENT				0x120

#define SHADER_READY				0x140
#define TILER_READY				0x150
#define L2_READY				0x160

#define SHADER_PWRON				0x180
#define TILER_PWRON				0x190
#define L2_PWRON				0x1A0

#define SHADER_PWROFF				0x1C0
#define TILER_PWROFF				0x1D0
#define L2_PWROFF				0x1E0

#define SHADER_PWRTRANS				0x200
#define TILER_PWRTRANS				0x210
#define L2_PWRTRANS				0x220

#define SHADER_PWRACTIVE			0x240
#define TILER_PWRACTIVE				0x250
#define L2_PWRACTIVE				0x260

#define GPU_REVID				0x280

#define GPU_ASN_HASH(n)				(0x2C0 + ((n) * 4))

#define GPU_COHERENCY_FEATURES			0x300
#define GPU_COHERENCY_PROTOCOL			0x304
#define   GPU_COHERENCY_ACE_LITE		0
#define   GPU_COHERENCY_ACE			1
#define   GPU_COHERENCY_NONE			31

/* Exception codes reported in GPU_FAULT_STATUS[7:0], AS_FAULTSTATUS[7:0] and CS fault/fatal
 * (enum drm_panthor_exception_type, panthor_device.h). */
enum pw_exception_type {
	PW_EXC_OK = 0x00,
	PW_EXC_TERMINATED = 0x04,
	PW_EXC_KABOOM = 0x05,
	PW_EXC_EUREKA = 0x06,
	PW_EXC_ACTIVE = 0x08,
	PW_EXC_CS_RES_TERM = 0x0f,
	PW_EXC_MAX_NON_FAULT = 0x3f,
	PW_EXC_CS_CONFIG_FAULT = 0x40,
	PW_EXC_CS_UNRECOVERABLE = 0x41,
	PW_EXC_CS_ENDPOINT_FAULT = 0x44,
	PW_EXC_CS_BUS_FAULT = 0x48,
	PW_EXC_CS_INSTR_INVALID = 0x49,
	PW_EXC_CS_CALL_STACK_OVERFLOW = 0x4a,
	PW_EXC_CS_INHERIT_FAULT = 0x4b,
	PW_EXC_INSTR_INVALID_PC = 0x50,
	PW_EXC_INSTR_INVALID_ENC = 0x51,
	PW_EXC_INSTR_BARRIER_FAULT = 0x55,
	PW_EXC_DATA_INVALID_FAULT = 0x58,
	PW_EXC_TILE_RANGE_FAULT = 0x59,
	PW_EXC_ADDR_RANGE_FAULT = 0x5a,
	PW_EXC_IMPRECISE_FAULT = 0x5b,
	PW_EXC_OOM = 0x60,
	PW_EXC_CSF_FW_INTERNAL_ERROR = 0x68,
	PW_EXC_CSF_RES_EVICTION_TIMEOUT = 0x69,
	PW_EXC_GPU_BUS_FAULT = 0x80,
	PW_EXC_GPU_SHAREABILITY_FAULT = 0x88,
	PW_EXC_SYS_SHAREABILITY_FAULT = 0x89,
	PW_EXC_GPU_CACHEABILITY_FAULT = 0x8a,
	PW_EXC_TRANSLATION_FAULT_0 = 0xc0,
	PW_EXC_TRANSLATION_FAULT_1 = 0xc1,
	PW_EXC_TRANSLATION_FAULT_2 = 0xc2,
	PW_EXC_TRANSLATION_FAULT_3 = 0xc3,
	PW_EXC_TRANSLATION_FAULT_4 = 0xc4,
	PW_EXC_PERM_FAULT_0 = 0xc8,
	PW_EXC_PERM_FAULT_1 = 0xc9,
	PW_EXC_PERM_FAULT_2 = 0xca,
	PW_EXC_PERM_FAULT_3 = 0xcb,
	PW_EXC_ACCESS_FLAG_1 = 0xd9,
	PW_EXC_ACCESS_FLAG_2 = 0xda,
	PW_EXC_ACCESS_FLAG_3 = 0xdb,
	PW_EXC_ADDR_SIZE_FAULT_IN = 0xe0,
	PW_EXC_ADDR_SIZE_FAULT_OUT0 = 0xe4,
	PW_EXC_ADDR_SIZE_FAULT_OUT1 = 0xe5,
	PW_EXC_ADDR_SIZE_FAULT_OUT2 = 0xe6,
	PW_EXC_ADDR_SIZE_FAULT_OUT3 = 0xe7,
	PW_EXC_MEM_ATTR_FAULT_0 = 0xe8,
	PW_EXC_MEM_ATTR_FAULT_1 = 0xe9,
	PW_EXC_MEM_ATTR_FAULT_2 = 0xea,
	PW_EXC_MEM_ATTR_FAULT_3 = 0xeb,
};

#endif /* PW_REGS_GPU_H */
