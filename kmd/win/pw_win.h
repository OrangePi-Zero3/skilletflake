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
#include "pw_win_plat.h"

#ifndef PANWIN_VERSION_STRING
#define PANWIN_VERSION_STRING "unknown"
#endif
#ifndef PANWIN_DEBUG_IOCTLS
#define PANWIN_DEBUG_IOCTLS 0
#endif

#define PW_POOL_TAG 'niwP' /* "Pwin" in pool dumps */

/* Registry parameters (device "Device Parameters" key, set by panwin.inf; the service's
 * Parameters key is used as a fallback). */
#define PW_REG_BOOT_FIRMWARE L"BootFirmware"   /* DWORD, default 0 (M1: no FW boot) */
#define PW_REG_FIRMWARE_ROOT L"FirmwareRoot"   /* REG_SZ, NT path ending in '\' */
#define PW_REG_FIRMWARE_NAME L"FirmwareName"   /* REG_SZ, optional, relative to the root */
#define PW_REG_PMU_CHECK     L"Rk3588PmuCheck" /* DWORD, default 1; RK3588 (RKCP7410) only */
#define PW_REG_PSC_CHECK     L"AcpiPscCheck"   /* DWORD, default 1; boards whose node has _PSC */
#define PW_DEFAULT_FIRMWARE_ROOT L"\\SystemRoot\\System32\\drivers\\panwin\\"

#define PW_PATH_MAX 260 /* WCHARs */

/* Linux ENODEV: not part of the core's PW_E* set, used by the glue for "no core device". */
#define PW_WIN_ENODEV (-19)

/* ---- pw_os (opaque to core) ---- */
struct pw_os {
	WDFDEVICE device;
	volatile UCHAR *mmio;     /* GPU register window (Device-nGnRE via MmMapIoSpace[Ex]) */
	SIZE_T mmio_len;
	PHYSICAL_ADDRESS mmio_pa;
	volatile LONG regs_enabled; /* 0 until the GPU power domain is known to be on */
	WCHAR fw_root[PW_PATH_MAX];
	USHORT fw_root_chars;
	/*
	 * In-flight ioctls (struct pw_win_active, embedded in the request context): lets
	 * pw_os_event_wait_user() find the abort event of the file the current thread is
	 * executing an ioctl on, so cleanup / device teardown can interrupt user waits.
	 */
	KSPIN_LOCK active_lock;
	LIST_ENTRY active_list;
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
	BOOLEAN gpu_powered;           /* PMU/_PSC check passed (or disabled): registers are safe */
	BOOLEAN core_failed_logged;
	WDFINTERRUPT irq[PW_IRQ_COUNT];
	ULONG irq_spurious[PW_IRQ_COUNT]; /* asserted lines masked by the glue (no core / declined) */
	/*
	 * Device-level users of ->pw / ->os registers that are not tied to a file (GET_INFO,
	 * DEBUG_REG_READ) hold core_rundown; D0Exit waits for them before destroying the core.
	 * Every open file is on `files` (files_lock): D0Exit revokes them (pw_file_close) first.
	 */
	EX_RUNDOWN_REF core_rundown;
	FAST_MUTEX files_lock;         /* KeEnterCriticalRegion + ExAcquireFastMutexUnsafe */
	LIST_ENTRY files;
	/*
	 * (jm-glue) Board, from the ACPI hardware IDs (EvtDeviceAdd); NULL if not recognised, in
	 * which case no SoC-specific check runs. `backend` is pw_device_backend() of the last probe,
	 * published with `probed` (release) once pw_device_probe() returned, success or not; both
	 * are cleared in D0Exit after the device is destroyed. GET_INFO may use ->pw while holding
	 * core_rundown and seeing probed != 0 (D0Exit waits for the rundown before destroying it).
	 */
	const struct pw_win_platform *plat;
	volatile LONG probed;
	enum pw_backend backend;
	/* parameters */
	ULONG boot_firmware;
	ULONG pmu_check;
	ULONG psc_check;
	uint32_t timer_hz;              /* CNTFRQ_EL0 (24 MHz on RK3588 and H616) */
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
	LIST_ENTRY link;          /* dev->files, under dev->files_lock */
	PEPROCESS process;        /* creator (referenced): the only process allowed to mmap */
	struct pw_file *pwf;      /* core file; NULL if there was no core device, or revoked */
	/* In-flight ioctls of this file hold `rundown`; teardown waits for them after setting
	 * `abort`, which interrupts their user waits (pw_os_event_wait_user -> PW_EINTR). */
	EX_RUNDOWN_REF rundown;
	KEVENT abort;             /* notification event, set once at teardown */
	BOOLEAN torn_down;        /* under dev->files_lock */
	FAST_MUTEX lock;          /* maps + notifies */
	LIST_ENTRY maps;          /* struct pw_win_map */
	LIST_ENTRY notifies;      /* struct pw_win_notify */
	ULONG nmaps, nnotifies;
} PW_FILE_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PW_FILE_CTX, PwFileCtx)

/* An ioctl being executed in the caller's thread (registered in pw_os.active_list). */
struct pw_win_active {
	LIST_ENTRY link;
	HANDLE thread_id;
	PKEVENT abort;            /* the file's abort event */
};

/* ---- per-request context (nonpaged, lives as long as the request) ---- */
typedef struct _PW_REQ_CTX {
	struct pw_win_active active;
} PW_REQ_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PW_REQ_CTX, PwReqCtx)

/* The core device, if it probed successfully (what ioctls may use). */
static inline struct pw_device *pw_win_core(PW_DEVICE_CTX *ctx)
{
	return __atomic_load_n(&ctx->core_ready, __ATOMIC_ACQUIRE) ? ctx->pw : NULL;
}

/* Passive-level lock helpers (same pattern as pw_os_mutex: holder stays at PASSIVE_LEVEL). */
static inline void pw_win_lock(FAST_MUTEX *m)
{
	KeEnterCriticalRegion();
	ExAcquireFastMutexUnsafe(m);
}

static inline void pw_win_unlock(FAST_MUTEX *m)
{
	ExReleaseFastMutexUnsafe(m);
	KeLeaveCriticalRegion();
}

/* pw_win_driver.c */
void pw_win_resolve_routines(void);

/* pw_win_ioctl.c */
NTSTATUS pw_win_status_from_errno(int err);
NTSTATUS pw_win_queue_init(PW_DEVICE_CTX *ctx);
void pw_win_files_init(PW_DEVICE_CTX *ctx);
/* D0Exit: tear down every open file (user mappings, notifies, pw_file_close) so the core
 * device can be destroyed; the files stay open but every later ioctl fails with ENODEV. */
void pw_win_revoke_files(PW_DEVICE_CTX *ctx);
EVT_WDF_IO_IN_CALLER_CONTEXT pw_win_io_in_caller_context;
EVT_WDF_DEVICE_FILE_CREATE pw_win_file_create;
EVT_WDF_FILE_CLEANUP pw_win_file_cleanup;
EVT_WDF_FILE_CLOSE pw_win_file_close;

/* pw_win_user.c: copies to/from user VAs of the current process, under SEH. */
NTSTATUS pw_win_copy_to_user(KPROCESSOR_MODE mode, void *user_dst, const void *src, size_t len);
NTSTATUS pw_win_copy_from_user(KPROCESSOR_MODE mode, void *dst, const void *user_src, size_t len);
/* MmMapLockedPagesSpecifyCache(UserMode) under SEH; NULL on failure. */
void *pw_win_map_user(PMDL mdl, MEMORY_CACHING_TYPE cache, ULONG priority);

/* pw_os_win.c */
void pw_os_win_init(struct pw_os *os, WDFDEVICE device);
void pw_os_win_set_firmware_root(struct pw_os *os, const WCHAR *root, USHORT chars);
/* The MDL describing all pages of @mem (owned by mem) and the cache type every CPU mapping of
 * them must use (ARM64 forbids mismatched attributes between aliases). */
PMDL pw_os_win_mem_mdl(const struct pw_os_mem *mem);
MEMORY_CACHING_TYPE pw_os_win_mem_cache(const struct pw_os_mem *mem);
void pw_os_win_active_enter(struct pw_os *os, struct pw_win_active *a, PKEVENT abort);
void pw_os_win_active_leave(struct pw_os *os, struct pw_win_active *a);

/* Glue logging (same sink as pw_os_log). */
#define PW_WLOG(lvl, ...) pw_os_log(NULL, (lvl), __VA_ARGS__)

#endif /* PW_WIN_H */
