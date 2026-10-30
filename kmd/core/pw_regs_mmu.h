/* SPDX-License-Identifier: MIT */
/*
 * pw_regs_mmu.h - Mali MMU_INT and address-space (AS) registers, absolute offsets from the GPU base.
 *
 * Derived from Linux panthor (dual GPL-2.0/MIT, used under MIT):
 *   drivers/gpu/drm/panthor/panthor_mmu_regs.h (Copyright 2026 ARM Limited).
 * NOTE: panthor's AS_*(as) macros are relative to MMU_AS_BASE (mmu.c:3411 sets
 * mmu->iomem = iomem + MMU_AS_BASE); ours include the base so every offset here is absolute.
 */
#ifndef PW_REGS_MMU_H
#define PW_REGS_MMU_H

#include "pw_util.h"

#define MMU_INT_BASE				0x2000
/* MMU_INT bits [15:0] = page fault on AS n, [31:16] = command complete on AS n (mmu.c:658). */
#define   MMU_INT_FAULT_MASK			0x0000ffffu
#define   MMU_INT_AS_FAULT(as)			PW_BIT(as)

#define MMU_AS_BASE				0x2400
#define MMU_AS_SHIFT				6
#define MMU_AS(as)				(MMU_AS_BASE + ((as) << MMU_AS_SHIFT))
#define PW_MAX_AS				16

#define AS_TRANSTAB(as)				(MMU_AS(as) + 0x0)	/* 64-bit */
#define AS_MEMATTR(as)				(MMU_AS(as) + 0x8)	/* 64-bit */
#define   AS_MEMATTR_AARCH64_INNER_ALLOC_IMPL		(2u << 2)
#define   AS_MEMATTR_AARCH64_INNER_ALLOC_EXPL(w, r)	((3u << 2) | \
							 ((w) ? PW_BIT(0) : 0) | \
							 ((r) ? PW_BIT(1) : 0))
#define   AS_MEMATTR_AARCH64_SH_MIDGARD_INNER		(0u << 4)
#define   AS_MEMATTR_AARCH64_SH_CPU_INNER		(1u << 4)
#define   AS_MEMATTR_AARCH64_SH_CPU_INNER_SHADER_COH	(2u << 4)
#define   AS_MEMATTR_AARCH64_SHARED			(0u << 6)
#define   AS_MEMATTR_AARCH64_INNER_OUTER_NC		(1u << 6)
#define   AS_MEMATTR_AARCH64_INNER_OUTER_WB		(2u << 6)
#define   AS_MEMATTR_AARCH64_FAULT			(3u << 6)
#define AS_LOCKADDR(as)				(MMU_AS(as) + 0x10)	/* 64-bit */
#define AS_COMMAND(as)				(MMU_AS(as) + 0x18)
#define   AS_COMMAND_NOP			0
#define   AS_COMMAND_UPDATE			1
#define   AS_COMMAND_LOCK			2
#define   AS_COMMAND_UNLOCK			3
#define   AS_COMMAND_FLUSH_PT			4
#define   AS_COMMAND_FLUSH_MEM			5
#define   AS_LOCK_REGION_MIN_SIZE		(1ull << 15)
#define AS_FAULTSTATUS(as)			(MMU_AS(as) + 0x1C)
#define  AS_FAULTSTATUS_EXCEPTION_TYPE(x)	((x) & 0xffu)
#define  AS_FAULTSTATUS_ACCESS_TYPE_MASK	(0x3u << 8)
#define  AS_FAULTSTATUS_ACCESS_TYPE_ATOMIC	(0x0u << 8)
#define  AS_FAULTSTATUS_ACCESS_TYPE_EX		(0x1u << 8)
#define  AS_FAULTSTATUS_ACCESS_TYPE_READ	(0x2u << 8)
#define  AS_FAULTSTATUS_ACCESS_TYPE_WRITE	(0x3u << 8)
#define  AS_FAULTSTATUS_DECODER_FAULT		PW_BIT(10)	/* mmu.c:1897: set = decoder, clear = slave */
#define  AS_FAULTSTATUS_SOURCE_ID(x)		((x) >> 16)
#define AS_FAULTADDRESS(as)			(MMU_AS(as) + 0x20)	/* 64-bit */
#define AS_STATUS(as)				(MMU_AS(as) + 0x28)
#define   AS_STATUS_AS_ACTIVE			PW_BIT(0)
#define AS_TRANSCFG(as)				(MMU_AS(as) + 0x30)	/* 64-bit */
#define   AS_TRANSCFG_ADRMODE_UNMAPPED		(1ull << 0)
#define   AS_TRANSCFG_ADRMODE_IDENTITY		(2ull << 0)
#define   AS_TRANSCFG_ADRMODE_AARCH64_4K	(6ull << 0)
#define   AS_TRANSCFG_ADRMODE_AARCH64_64K	(8ull << 0)
#define   AS_TRANSCFG_ADRMODE_MASK		(0xfull << 0)
#define   AS_TRANSCFG_INA_BITS(x)		((uint64_t)(x) << 6)
#define   AS_TRANSCFG_INA_BITS_GET(x)		(((x) >> 6) & 0x1f)
#define   AS_TRANSCFG_OUTA_BITS(x)		((uint64_t)(x) << 14)
#define   AS_TRANSCFG_SL_CONCAT			PW_BIT64(22)
#define   AS_TRANSCFG_PTW_MEMATTR_NC		(1ull << 24)
#define   AS_TRANSCFG_PTW_MEMATTR_WB		(2ull << 24)
#define   AS_TRANSCFG_PTW_SH_NS			(0ull << 28)
#define   AS_TRANSCFG_PTW_SH_OS			(2ull << 28)
#define   AS_TRANSCFG_PTW_SH_IS			(3ull << 28)
#define   AS_TRANSCFG_PTW_RA			PW_BIT64(30)
#define   AS_TRANSCFG_DISABLE_HIER_AP		PW_BIT64(33)
#define   AS_TRANSCFG_DISABLE_AF_FAULT		PW_BIT64(34)
#define   AS_TRANSCFG_WXN			PW_BIT64(35)
#define   AS_TRANSCFG_XREADABLE			PW_BIT64(36)
#define AS_FAULTEXTRA(as)			(MMU_AS(as) + 0x38)	/* 64-bit */

#endif /* PW_REGS_MMU_H */
