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
	/* Arch timer frequency (CNTFRQ_EL0) for FW timeouts; 0 = 24 MHz (RK3588). */
	uint32_t timer_hz;
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

/* ---- additions (kmd-core, M1-M2) ---- */

struct drm_panthor_mmu_info;

/*
 * Two-phase creation. pw_device_alloc() touches no hardware; it lets the OS layer publish the
 * device pointer to its ISR before pw_device_probe() unmasks any GPU interrupt (pw_irq_top()
 * returns false for a NULL device, which would leave a level-triggered line asserted).
 * pw_device_create() == alloc + probe (+ destroy on failure). All waits in probe also poll the
 * RAWSTAT registers, so probe completes (more slowly) even if interrupts are not delivered.
 * After a failed probe the device must still be released with pw_device_destroy().
 * Bottom halves of different lines must be able to run concurrently: the MMU bottom half can
 * wait (up to 100 ms, then falls back to polling) for a GPU cache flush to complete.
 */
int  pw_device_alloc(const struct pw_device_params *params, struct pw_device **out);
int  pw_device_probe(struct pw_device *dev);

/* True once the MCU raised GLOBAL_IF and the interfaces were parsed (PANWIN_INFO_FW_BOOTED). */
bool pw_device_fw_booted(struct pw_device *dev);

/* Toggle GLB_PING and wait for the FW to ack it. PW_OK, PW_ETIMEDOUT, or PW_EINVAL if no FW. */
int  pw_device_fw_ping(struct pw_device *dev, uint32_t timeout_ms);

/* DRM_PANTHOR_DEV_QUERY_MMU_INFO payload. */
void pw_device_get_mmu_info(struct pw_device *dev, struct drm_panthor_mmu_info *info);

/* ---- per-open-file API (M3-M5): the OS layer forwards every DRM ioctl here ---- */

struct pw_file;
struct pw_bo;
struct pw_os_mem;

/* One per open handle of the device (Linux: per DRM file). */
int  pw_file_open(struct pw_device *dev, struct pw_file **out);
/* Releases every object the file still owns (BOs, VMs, groups, heaps, syncobjs). The OS layer
 * must have torn down all user mappings and pending notifies of this file first. */
void pw_file_close(struct pw_file *file);

/*
 * Execute DRM ioctl number @nr (_IOC_NR of the Linux DRM_IOCTL_* value: core ioctls 0x00-0x3f and
 * 0xa0-0xcf, panthor 0x40 + DRM_PANTHOR_*) on the Linux uAPI struct in @arg (kernel copy, in/out,
 * @size == _IOC_SIZE of the Linux ioctl). Nested user pointers inside @arg are read/written with
 * pw_os_copy_{from,to}_user, so this must run in the calling thread's context at passive level.
 * Returns PW_OK or a negative Linux errno (PW_ENOTTY for unknown ioctls). @arg is copied back to
 * the caller even on error (DRM semantics: only on success; the OS layer decides).
 */
int  pw_file_ioctl(struct pw_file *file, uint32_t nr, void *arg, uint32_t size);

/* Emulated panthor DRM driver version (DRM_IOCTL_VERSION / PANWIN_IOCTL_GET_INFO). */
void pw_core_drm_version(uint32_t *major, uint32_t *minor, uint32_t *patch);

/*
 * mmap(): resolve a fake mmap offset (from DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, or the user MMIO
 * offset: both DRM_PANTHOR_USER_MMIO_OFFSET_32BIT and _64BIT are accepted, whatever
 * SET_USER_MMIO_OFFSET chose) to what must be mapped into the caller.
 */
enum pw_mmap_kind {
	PW_MMAP_BO = 0,		/* map bo pages [offset_in_obj, +size) */
	PW_MMAP_MMIO = 1,	/* map GPU register page(s) at mmio_offset (read-only) */
};
struct pw_mmap_target {
	enum pw_mmap_kind kind;
	struct pw_bo *bo;	/* PW_MMAP_BO: a reference the OS layer drops with pw_bo_put() */
	uint64_t offset_in_obj;	/* bytes, page aligned */
	uint32_t mmio_offset;	/* PW_MMAP_MMIO: offset in the GPU register window */
	bool read_only;		/* write mappings must be refused */
	bool cached;		/* false: map write-combined/Normal-NC (the M4 default) */
};
int  pw_file_mmap_lookup(struct pw_file *file, uint64_t offset, uint64_t size,
			 struct pw_mmap_target *t);
struct pw_os_mem *pw_bo_os_mem(struct pw_bo *bo);
void pw_bo_put(struct pw_bo *bo);

/*
 * PANWIN_IOCTL_SYNCOBJ_EVENT: call @fn(@ctx) once when syncobj @handle has a fence for @point
 * (0 = binary) and it is signalled (immediately, from this call, if it already is). @flags are
 * DRM_SYNCOBJ_WAIT_FLAGS_*; without WAIT_FOR_SUBMIT a missing fence is PW_EINVAL. *cookie
 * identifies the registration so the OS layer can cancel it with pw_file_syncobj_notify_cancel()
 * (returns true if it was cancelled before firing; after cancel/fire @fn is never called again).
 * @fn runs at passive level, possibly from an IRQ bottom half: it must not block.
 */
typedef void (*pw_notify_fn)(void *ctx);
int  pw_file_syncobj_notify(struct pw_file *file, uint32_t handle, uint64_t point, uint32_t flags,
			    pw_notify_fn fn, void *ctx, void **cookie);
bool pw_file_syncobj_notify_cancel(struct pw_file *file, void *cookie);

#endif /* PW_CORE_H */
