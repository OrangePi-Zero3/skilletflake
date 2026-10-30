/* SPDX-License-Identifier: MIT */
/*
 * pw_win.h - private header of the KMDF glue (kmd/win). Not seen by the OS-neutral core.
 */
#ifndef PW_WIN_H
#define PW_WIN_H

#include <ntddk.h>
#include <wdf.h>

#include "pw_win_ddk.h"

#include "pw_core.h"
#include "pw_os.h"

#ifndef PANWIN_VERSION_STRING
#define PANWIN_VERSION_STRING "unknown"
#endif
#ifndef PANWIN_DEBUG_IOCTLS
#define PANWIN_DEBUG_IOCTLS 0
#endif

#define PW_POOL_TAG 'niwP' /* "Pwin" in pool dumps */

/* Emulated panthor DRM driver version. 1.0: only DEV_QUERY GPU_INFO/CSIF_INFO are
 * implemented, which is what Mesa's panthor_kmod requires without version gates (see
 * docs/status/kmd-win.md for the gates of later minors). Bump as features land. */
#define PW_DRM_NAME          "panthor"
#define PW_DRM_DESC          "Panthor DRM driver (panfrost-win)"
#define PW_DRM_DATE          "0" /* Linux >= 6.14 reports "0" since drivers lost their date */
#define PW_DRM_VERSION_MAJOR 1
#define PW_DRM_VERSION_MINOR 0
#define PW_DRM_VERSION_PATCH 0

/* Registry parameters (device "Device Parameters" key, set by panwin.inf; the service's
 * Parameters key is used as a fallback). */
#define PW_REG_BOOT_FIRMWARE L"BootFirmware"   /* DWORD, default 0 (M1: no FW boot) */
#define PW_REG_FIRMWARE_ROOT L"FirmwareRoot"   /* REG_SZ, NT path ending in '\' */
#define PW_REG_FIRMWARE_NAME L"FirmwareName"   /* REG_SZ, optional, relative to the root */
#define PW_REG_PMU_CHECK     L"Rk3588PmuCheck" /* DWORD, default 1 */
#define PW_DEFAULT_FIRMWARE_ROOT L"\\SystemRoot\\System32\\drivers\\panwin\\"

#define PW_PATH_MAX 260 /* WCHARs */

/* ---- pw_os (opaque to core) ---- */
struct pw_os {
	WDFDEVICE device;
	volatile UCHAR *mmio;     /* GPU register window (Device-nGnRE via MmMapIoSpace[Ex]) */
	SIZE_T mmio_len;
	PHYSICAL_ADDRESS mmio_pa;
	volatile LONG regs_enabled; /* 0 until the GPU power domain is known to be on */
	WCHAR fw_root[PW_PATH_MAX];
	USHORT fw_root_chars;
};

/* Run-time resolved routines (pw_win_resolve_routines() in DriverEntry). */
struct pw_win_routines {
	PFN_EX_ALLOCATE_POOL2 ExAllocatePool2;
	PFN_MM_MAP_IO_SPACE_EX MmMapIoSpaceEx;
};
extern struct pw_win_routines pw_win_rt;

/* ---- per-device context ---- */
typedef struct _PW_DEVICE_CTX {
	WDFDEVICE device;
	struct pw_os os;
	struct pw_device *volatile pw; /* published to the ISR right after pw_device_alloc() */
	volatile LONG core_ready;      /* pw_device_probe() succeeded: ioctls may use ->pw */
	int core_status;               /* last alloc/probe result, PW_OK if none yet */
	BOOLEAN gpu_powered;           /* PMU check passed (or disabled): registers are safe */
	BOOLEAN core_failed_logged;
	WDFINTERRUPT irq[PW_IRQ_COUNT];
	ULONG irq_spurious[PW_IRQ_COUNT]; /* asserted lines masked by the glue (no core / declined) */
	/* parameters */
	ULONG boot_firmware;
	ULONG pmu_check;
	WCHAR fw_name_w[PW_PATH_MAX];
	char fw_name[PW_PATH_MAX];      /* ASCII copy of FirmwareName, "" = derive from GPU_ID */
} PW_DEVICE_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PW_DEVICE_CTX, PwDeviceCtx)

typedef struct _PW_IRQ_CTX {
	PW_DEVICE_CTX *dev;
	enum pw_irq_line line;
} PW_IRQ_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PW_IRQ_CTX, PwIrqCtx)

/* ---- per-open-handle context (one per DRM "fd") ---- */
typedef struct _PW_FILE_CTX {
	PW_DEVICE_CTX *dev;
	HANDLE pid;
	/* Later: GEM/VM/group/syncobj handle tables, user mappings to tear down at cleanup. */
} PW_FILE_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PW_FILE_CTX, PwFileCtx)

/* ---- per-request context: user buffers locked in the caller's context ---- */
typedef struct _PW_REQ_CTX {
	WDFMEMORY user_out;   /* nested output buffer (e.g. DEV_QUERY pointer), locked for write */
	size_t user_out_len;
} PW_REQ_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PW_REQ_CTX, PwReqCtx)

/* The core device, if it probed successfully (what ioctls may use). */
static inline struct pw_device *pw_win_core(PW_DEVICE_CTX *ctx)
{
	return __atomic_load_n(&ctx->core_ready, __ATOMIC_ACQUIRE) ? ctx->pw : NULL;
}

/* pw_win_driver.c */
void pw_win_resolve_routines(void);
NTSTATUS pw_win_status_from_errno(int err);

/* pw_win_ioctl.c */
NTSTATUS pw_win_queue_init(PW_DEVICE_CTX *ctx);
EVT_WDF_IO_IN_CALLER_CONTEXT pw_win_io_in_caller_context;
EVT_WDF_DEVICE_FILE_CREATE pw_win_file_create;
EVT_WDF_FILE_CLEANUP pw_win_file_cleanup;
EVT_WDF_FILE_CLOSE pw_win_file_close;

/* pw_win_user.c: copies to/from user VAs of the current process, under SEH. */
NTSTATUS pw_win_copy_to_user(KPROCESSOR_MODE mode, void *user_dst, const void *src, size_t len);
NTSTATUS pw_win_copy_from_user(KPROCESSOR_MODE mode, void *dst, const void *user_src, size_t len);

/* pw_os_win.c */
void pw_os_win_init(struct pw_os *os, WDFDEVICE device);
void pw_os_win_set_firmware_root(struct pw_os *os, const WCHAR *root, USHORT chars);

/* Glue logging (same sink as pw_os_log). */
#define PW_WLOG(lvl, ...) pw_os_log(NULL, (lvl), __VA_ARGS__)

#endif /* PW_WIN_H */
