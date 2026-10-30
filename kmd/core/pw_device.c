// SPDX-License-Identifier: MIT
/*
 * pw_device.c - device lifetime, IRQ dispatch and DEV_QUERY payloads (pw_core.h).
 *
 * Bring-up order follows panthor_device_init() (panthor_device.c:166-300) restricted to M1-M2:
 *   GPU_ID/features -> GPU IRQ -> soft reset -> coherency NONE + L2 on
 *   -> MMU (IRQ) -> FW (job IRQ, MCU VM, sections, AS0, MCU boot, ifaces, global init, ping).
 * Deviation: panthor doesn't soft-reset at probe (only in its reset work, device.c:142); we do,
 * because the state left by UEFI on Windows is unknown.
 *
 * (jm-arch) A job-manager GPU (enum pw_backend) shares GPU_ID -> reset -> L2 -> MMU, then runs
 * the JM modules instead of the firmware (panfrost_device_init order: gpu, mmu, job):
 *   pw_jm_hw_init (features, quirks, shader/tiler power, job IRQ, AS slots)
 *   -> pw_jm_mem_init -> pw_jm_sched_init.
 */

#include "pw_file.h"
#include "pw_jm_hw.h"
#include "pw_jm_mem.h"
#include "pw_jm_sched.h"
#include "pw_mmu.h"
#include "pw_priv.h"
#include "pw_sched.h"

int pw_device_alloc(const struct pw_device_params *params, struct pw_device **out)
{
	struct pw_device *dev;

	if (!params || !params->os || !out)
		return PW_EINVAL;
	dev = pw_os_zalloc(params->os, sizeof(*dev));
	if (!dev)
		return PW_ENOMEM;
	dev->os = params->os;
	dev->boot_firmware = params->boot_firmware;
	if (params->fw_name)
		pw_strlcpy(dev->fw_name, params->fw_name, sizeof(dev->fw_name));
	*out = dev;
	return PW_OK;
}

/* Undo pw_device_probe(): stop the MCU, release the AS slots, power the L2 down and mask
 * every line. Sync objects stay alive, since bottom halves may still be running. */
static void device_quiesce(struct pw_device *dev)
{
	/* (jm-arch) JM: reverse of device_probe_jm(); every fini is a no-op if its init never ran,
	 * and the CSF finis below are no-ops on JM (nothing was started). */
	if (dev->backend == PW_BACKEND_JM) {
		pw_jm_sched_fini(dev);
		pw_jm_mem_fini(dev);
		pw_jm_hw_fini(dev);
	}
	/* The OS layer closed every file first, so no group is left; the scheduler's work item
	 * goes before the MCU (sched.c:4050 panthor_sched_unplug runs before panthor_fw_unplug). */
	pw_sched_fini(dev);
	pw_fw_fini(dev);
	pw_mmu_fini(dev);
	if (dev->gpu.reqs_lock && dev->gpu.l2_on)
		pw_gpu_l2_power_off(dev);
	pw_gpu_fini(dev);
	for (unsigned int i = 0; i < PW_IRQ_COUNT; i++)
		pw_irq_suspend(dev, (enum pw_irq_line)i);
}

/* (jm-arch) JM: no firmware; boot_firmware is ignored (pw_core.h, enum pw_backend). */
static int device_probe_jm(struct pw_device *dev)
{
	int ret;

	ret = pw_mmu_init(dev);
	if (ret)
		return ret;
	ret = pw_jm_hw_init(dev);
	if (ret)
		return ret;
	ret = pw_jm_mem_init(dev);
	if (ret)
		return ret;
	ret = pw_jm_sched_init(dev);
	if (ret)
		return ret;
	pw_info(dev, "job manager initialised");
	return PW_OK;
}

static int device_probe(struct pw_device *dev)
{
	int ret;

	ret = pw_gpu_init(dev);
	if (ret)
		return ret;

	ret = pw_gpu_soft_reset(dev);
	if (ret)
		return ret;

	ret = pw_gpu_l2_power_on(dev);
	if (ret)
		return ret;

	if (dev->backend == PW_BACKEND_JM)
		return device_probe_jm(dev);

	if (!dev->fw_name[0]) {
		/* fw.c:811 */
		pw_snprintf(dev->fw_name, sizeof(dev->fw_name), "arm/mali/arch%u.%u/mali_csffw.bin",
			    GPU_ARCH_MAJOR(dev->gpu_info.gpu_id), GPU_ARCH_MINOR(dev->gpu_info.gpu_id));
	}

	if (!dev->boot_firmware) {
		pw_info(dev, "M1 mode: GPU initialised, MMU/firmware skipped");
		return PW_OK;
	}

	ret = pw_mmu_init(dev);
	if (ret)
		return ret;

	ret = pw_fw_init(dev);
	if (ret)
		return ret;

	/* device.c:286: panthor_sched_init() right after panthor_fw_init(). */
	ret = pw_sched_init(dev);
	if (ret)
		return ret;

	pw_info(dev, "firmware booted");
	return PW_OK;
}

int pw_device_probe(struct pw_device *dev)
{
	int ret;

	if (!dev || dev->probed)
		return PW_EINVAL;
	dev->probed = true;

	ret = device_probe(dev);
	if (ret) {
		pw_err(dev, "probe failed (%d)", ret);
		device_quiesce(dev);
	}
	return ret;
}

int pw_device_create(const struct pw_device_params *params, struct pw_device **out)
{
	struct pw_device *dev;
	int ret;

	ret = pw_device_alloc(params, &dev);
	if (ret)
		return ret;
	ret = pw_device_probe(dev);
	if (ret) {
		pw_device_destroy(dev);
		return ret;
	}
	*out = dev;
	return PW_OK;
}

/* Pre-condition: the OS layer has disconnected the ISR and flushed queued bottom halves. */
void pw_device_destroy(struct pw_device *dev)
{
	if (!dev)
		return;
	device_quiesce(dev);
	if (dev->backend == PW_BACKEND_JM) {
		pw_jm_sched_release(dev);
		pw_jm_mem_release(dev);
		pw_jm_hw_release(dev);
	}
	pw_fw_release(dev);
	pw_mmu_release(dev);
	pw_gpu_release(dev);
	for (unsigned int i = 0; i < PW_IRQ_COUNT; i++)
		pw_irq_fini(dev, (enum pw_irq_line)i);
	pw_os_free(dev->os, dev);
}

void pw_device_get_gpu_info(struct pw_device *dev, struct drm_panthor_gpu_info *info)
{
	*info = dev->gpu_info;
}

int pw_device_get_csif_info(struct pw_device *dev, struct drm_panthor_csif_info *info)
{
	if (!pw_device_fw_booted(dev))
		return PW_ENOENT;
	*info = dev->csif_info;
	return PW_OK;
}

void pw_device_get_mmu_info(struct pw_device *dev, struct drm_panthor_mmu_info *info)
{
	(void)dev;
	pw_memset(info, 0, sizeof(*info));
	info->page_size_bitmap = PW_SZ_4K | PW_SZ_2M;	/* mmu.c:3379 */
}

bool pw_device_fw_booted(struct pw_device *dev)
{
	return dev && __atomic_load_n(&dev->fw.booted, __ATOMIC_ACQUIRE) &&
	       __atomic_load_n(&dev->fw.ifaces_ready, __ATOMIC_ACQUIRE);
}

int pw_device_fw_ping(struct pw_device *dev, uint32_t timeout_ms)
{
	if (!pw_device_fw_booted(dev))
		return PW_EINVAL;
	return pw_fw_ping(dev, timeout_ms);
}

void pw_device_describe(struct pw_device *dev, char *buf, size_t len)
{
	const struct drm_panthor_gpu_info *gi;
	uint32_t id;
	int n;

	if (!buf || !len)
		return;
	if (!dev || !dev->gpu_info.gpu_id) {
		pw_snprintf(buf, len, "Mali (not probed)");
		return;
	}
	gi = &dev->gpu_info;
	id = gi->gpu_id;
	n = pw_snprintf(buf, len, "%s r%up%u arch%u.%u (id 0x%04x), %u cores",
			pw_gpu_model_name(gi), GPU_VER_MAJOR(id), GPU_VER_MINOR(id),
			GPU_ARCH_MAJOR(id), GPU_ARCH_MINOR(id), id >> 16,
			pw_hweight64(gi->shader_present));
	if (n < 0 || (size_t)n >= len)
		return;

	if (dev->backend == PW_BACKEND_JM) {
		pw_snprintf(buf + n, len - (size_t)n, ", job manager (no firmware)");
	} else if (pw_device_fw_booted(dev)) {
		uint32_t v = PW_READ_ONCE(dev->fw.glb.control->version);

		pw_snprintf(buf + n, len - (size_t)n, ", fw booted (iface v%u.%u.%u), %u CSGs",
			    CSF_IFACE_VERSION_MAJOR(v), CSF_IFACE_VERSION_MINOR(v),
			    CSF_IFACE_VERSION_PATCH(v), dev->csif_info.csg_slot_count);
	} else if (!dev->boot_firmware && dev->gpu.l2_on) {
		pw_snprintf(buf + n, len - (size_t)n, ", L2 on, fw not loaded (M1 mode)");
	} else {
		pw_snprintf(buf + n, len - (size_t)n, ", fw not booted");
	}
}

/* ---- (jm-arch) backend queries ---- */

enum pw_backend pw_device_backend(const struct pw_device *dev)
{
	return dev ? dev->backend : PW_BACKEND_NONE;
}

void pw_device_drm_version(const struct pw_device *dev, uint32_t *major, uint32_t *minor,
			   uint32_t *patch, const char **name)
{
	if (dev && dev->backend == PW_BACKEND_JM) {
		pw_jm_drm_version(major, minor, patch);
		if (name)
			*name = PW_JM_DRM_NAME;
		return;
	}
	pw_core_drm_version(major, minor, patch);
	if (name)
		*name = "panthor";
}
