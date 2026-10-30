/* SPDX-License-Identifier: MIT */
/*
 * pw_core.h - public API of the OS-neutral Mali CSF core, called by the OS layer (kmd/win).
 *
 * Milestone M0-M2 surface: device bring-up, IRQ handling, GPU info query, firmware boot.
 * Later milestones extend this with VM/BO/group/syncobj entry points (one per panthor ioctl).
 */
#ifndef PW_CORE_H
#define PW_CORE_H

#include "pw_os.h"

struct drm_panthor_gpu_info;
struct drm_panthor_csif_info;

struct pw_device; /* opaque, allocated by core */

/* Which of the three Mali interrupt lines (ACPI _CRS order on RK3588: job, mmu, gpu). */
enum pw_irq_line { PW_IRQ_JOB = 0, PW_IRQ_MMU = 1, PW_IRQ_GPU = 2, PW_IRQ_COUNT = 3 };

struct pw_device_params {
	struct pw_os *os;
	/* Firmware path relative to the firmware root; NULL = derive from GPU_ID
	 * ("arm/mali/arch%u.%u/mali_csffw.bin"). */
	const char *fw_name;
	/* If false, stop after GPU init (GPU_ID, reset, L2) and skip MMU/FW: M1 bring-up mode. */
	bool boot_firmware;
};

/* Probe + init. Pre-condition: the GPU power domain, clocks and supply are already on
 * (firmware/ACPI responsibility). Returns PW_OK and *out on success. */
int  pw_device_create(const struct pw_device_params *params, struct pw_device **out);
void pw_device_destroy(struct pw_device *dev);

/*
 * Interrupts. The OS layer calls pw_irq_top() from the ISR (high IRQL, no locks, no allocation):
 * it reads+masks the line's INT_STAT and returns true if the interrupt was ours. If it returns true
 * the OS layer must later call pw_irq_bottom() for the same line at passive level, which handles the
 * events and re-enables the mask.
 */
bool pw_irq_top(struct pw_device *dev, enum pw_irq_line line);
void pw_irq_bottom(struct pw_device *dev, enum pw_irq_line line);

/* DRM_PANTHOR_DEV_QUERY_GPU_INFO / _CSIF_INFO payloads. */
void pw_device_get_gpu_info(struct pw_device *dev, struct drm_panthor_gpu_info *info);
int  pw_device_get_csif_info(struct pw_device *dev, struct drm_panthor_csif_info *info);

/* Human-readable one-line state for debugging (e.g. "G610 r0p0 arch10.8, fw booted, 8 CSGs"). */
void pw_device_describe(struct pw_device *dev, char *buf, size_t len);

#endif /* PW_CORE_H */
