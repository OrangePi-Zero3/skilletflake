/* SPDX-License-Identifier: MIT */
/*
 * pw_regs_fw.h - MCU_CONTROL, JOB_INT, LATEST_FLUSH_ID and CSF doorbells (absolute offsets).
 *
 * Derived from Linux panthor (dual GPL-2.0/MIT, used under MIT):
 *   drivers/gpu/drm/panthor/panthor_fw_regs.h (Copyright 2026 ARM Limited).
 * panthor's MCU_CONTROL/MCU_STATUS are relative to MCU_CONTROL_BASE (fw.c:1500); ours are absolute.
 */
#ifndef PW_REGS_FW_H
#define PW_REGS_FW_H

#include "pw_util.h"

#define MCU_CONTROL_BASE			0x700
#define MCU_CONTROL				(MCU_CONTROL_BASE + 0x0)
#define   MCU_CONTROL_DISABLE			0
#define   MCU_CONTROL_ENABLE			1
#define   MCU_CONTROL_AUTO			2
#define MCU_STATUS				(MCU_CONTROL_BASE + 0x4)
#define   MCU_STATUS_DISABLED			0
#define   MCU_STATUS_ENABLED			1
#define   MCU_STATUS_HALT			2
#define   MCU_STATUS_FATAL			3

#define JOB_INT_BASE				0x1000
#define   JOB_INT_GLOBAL_IF			PW_BIT(31)
#define   JOB_INT_CSG_IF(x)			PW_BIT(x)

/* One read-only page holding the latest cache-flush ID, mapped to userspace by panthor. */
#define CSF_GPU_LATEST_FLUSH_ID			0x10000

#define CSF_DOORBELL(i)				(0x80000 + ((i) * 0x10000))
#define CSF_GLB_DOORBELL_ID			0

#endif /* PW_REGS_FW_H */
