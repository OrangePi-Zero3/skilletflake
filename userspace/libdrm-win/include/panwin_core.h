/* SPDX-License-Identifier: MIT */
/*
 * panwin_core.h - libdrm-win's in-process "core" transport (test builds only).
 *
 * libdrm.dll configured with -Dcore_transport=true (x86_64, tests/e2e) links the real OS-neutral
 * kernel core (kmd/core) on top of the host-test mock G610 (kmd/tests/pw_os_mock.c + the mock
 * CSF command-stream executor) and registers it as transport "core":
 *
 *   PANWIN_TRANSPORT=core   route every device to it (or panwin_select_transport("core"))
 *   PANWIN_CORE_MODEL=g31   (jm-glue) emulate the Orange Pi Zero3's Mali-G31 MP2 instead (job
 *                           manager, no firmware: the core reports DRM "panfrost" and the
 *                           panfrost ioctls; the mock's job-chain executor is kmd/tests/mock_jm.c).
 *                           Default / "g610": the RK3588 Mali-G610 (CSF, "panthor"). Read at
 *                           each device bring-up (first open after the last close).
 *   PANWIN_CORE_FW=<path>   mali_csffw.bin to boot (default: the build-time -Dcore_fw path;
 *                           not used with PANWIN_CORE_MODEL=g31)
 *   PANWIN_CORE_REPORT=1    print a one-line leak/health report when a device is torn down
 *                           (and unexpected DRM ioctl failures)
 *   PANWIN_CORE_TRACE=1     print every DRM ioctl (nr, size, result)
 *   PANWIN_CORE_POISON=1    don't zero PW_MEM_ZEROED memory (the host-test mock's default)
 *   PW_TEST_VERBOSE=1       print every core log line (pw_os_log)
 *
 * It reproduces panwin.sys's ABI v2 (kmd/win/pw_win_ioctl.c): one core pw_file per open,
 * METHOD_BUFFERED copies of the DRM struct (written back on success only), errors as
 * PANWIN_STATUS_FROM_ERRNO, MMAP of BOs (mock "DRAM" pages: uncached BOs are distinct views of
 * the very pages the emulated GPU accesses) and of the LATEST_FLUSH_ID page (a static read-only
 * page), and SYNCOBJ_EVENT on caller-created Win32 events. The device (mock + core + firmware
 * boot) is created by the first open and destroyed, with a leak check, by the last close.
 *
 * NOT a driver: no hardware is involved and the mock's GPU only executes the CS instructions
 * listed in kmd/tests/mock_gpu.h (RUN_* work is a counted no-op).
 */
#ifndef PANWIN_CORE_H
#define PANWIN_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PANWIN_CORE_REPORT_VERSION 1u

struct panwin_core_report {
	uint32_t version;		/* in: PANWIN_CORE_REPORT_VERSION */
	uint32_t device_live;		/* a core device currently exists */
	uint32_t devices_created;	/* successful bring-ups (G610: firmware booted; G31:
					 * probed as a job-manager GPU) */
	uint32_t devices_destroyed;
	uint32_t bringup_failures;
	uint32_t open_files;
	/* Accumulated over every torn-down device: must all be 0 for a clean run. */
	int64_t leaked_allocs;		/* pw_os_zalloc without pw_os_free */
	int64_t leaked_mems;		/* pw_os_mem_alloc without pw_os_mem_free */
	int64_t leaked_sync;		/* mutexes + events + work items */
	uint32_t model_errors;		/* protocol violations seen by the mock */
	uint32_t irq_unclaimed;		/* asserted IRQ lines nobody claimed */
	uint32_t leaked_maps;		/* user mappings still present at file close */
	uint32_t leaked_notifies;	/* SYNCOBJ_EVENT registrations cancelled at close */
	/* Traffic (all devices). */
	uint64_t drm_ioctls, drm_ioctl_errors;
	uint64_t mmaps, munmaps, syncobj_events;
	/* Mock CSF executor, summed over every CSG/CS slot (live device, else the last one
	 * sampled before teardown): what the emulated MCU actually executed. Always 0 on G31. */
	uint64_t cs_instrs, cs_runs, cs_syncs, cs_waits, cs_doorbells;
	uint32_t cs_fatal;		/* slots that raised CS_FATAL */
	uint32_t cs_fatal_type;		/* exception type of the last one */
};

/* Fill @r (set r->version first). 0, or -1 if the version is unknown. Exported by libdrm.dll
 * builds with the core transport only: look it up with GetProcAddress. */
typedef int (*panwin_core_get_report_fn)(struct panwin_core_report *r);
#define PANWIN_CORE_GET_REPORT "panwin_core_get_report"

/*
 * (jm-e2e) State of the fence in binary syncobj @handle of DRM fd @fd, encoded like a Linux
 * sync_file status (SYNC_IOC_FILE_INFO): 1 = signalled without error, 0 = not signalled yet,
 * < 0 = signalled with that negative Linux errno (a failed job: -EINVAL for a GPU fault,
 * -ECANCELED for a killed or timed-out job, -EIO after a failed reset). Returns 0, or -1 with
 * errno EBADF (not a DRM fd), EINVAL (not a core-transport fd, no such syncobj, or no fence).
 * Windows has no sync_file export and SYNCOBJ_WAIT ignores fence errors (as Linux does), so
 * this is the only way to observe them; test builds only. Look it up with GetProcAddress.
 */
typedef int (*panwin_core_syncobj_status_fn)(int fd, uint32_t handle, int32_t *status);
#define PANWIN_CORE_SYNCOBJ_STATUS "panwin_core_syncobj_status"

#ifdef __cplusplus
}
#endif

#endif /* PANWIN_CORE_H */
