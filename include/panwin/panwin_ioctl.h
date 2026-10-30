/* SPDX-License-Identifier: MIT */
/*
 * panwin_ioctl.h - user/kernel ABI between libdrm-win.dll (and tools) and panwin.sys.
 *
 * Design: every Linux DRM ioctl the userspace issues (core DRM ioctls such as SYNCOBJ_*, GEM_CLOSE,
 * VERSION, and the panthor DRM_PANTHOR_* driver ioctls) is forwarded unchanged:
 *
 *   Linux:   ioctl(fd, DRM_IOCTL_X, &arg)          where DRM_IOCTL_X = _IOWR('d', nr, struct ...)
 *   Windows: DeviceIoControl(h, PANWIN_IOCTL_DRM(nr), &arg, sizeof arg, &arg, sizeof arg, ...)
 *
 * The in/out buffer is the Linux uAPI struct verbatim (all fields are fixed-width, pointers are
 * __u64, so the layout is identical on aarch64 Linux and ARM64 Windows). Nested user pointers
 * inside the struct (arrays of syncs, query buffers, ...) are user-mode VAs of the calling
 * process; the kernel probes and copies them in the caller's context.
 * The kernel returns Linux errno semantics: the DeviceIoControl fails and GetLastError() is
 * mapped by the shim from the NTSTATUS (see panwin_status_to_errno() in libdrm-win), and the exact
 * negative errno is also reported in the output's trailing panwin_ioctl_status when present.
 *
 * Private (Windows-only) ioctls replace POSIX facilities that don't exist on Windows:
 * mmap of BOs / the user MMIO page, poll-able syncobj events, driver info.
 */
#ifndef PANWIN_IOCTL_H
#define PANWIN_IOCTL_H

#include <stdint.h>

/* {CEF9CF5A-3F71-4AC2-92E3-7F8D91261AC8} - device interface class exposed by panwin.sys */
#define PANWIN_INTERFACE_GUID_INIT \
	{ 0xcef9cf5a, 0x3f71, 0x4ac2, { 0x92, 0xe3, 0x7f, 0x8d, 0x91, 0x26, 0x1a, 0xc8 } }

/* Also exposed as a fixed symbolic link for simple tools: \\.\PanWin0 (\DosDevices\PanWin0). */
#define PANWIN_DOS_NAME_W   L"\\\\.\\PanWin0"

/* ACPI hardware ID of the RK3588 Mali-G610 node added by our edk2-rk3588 patch. */
#define PANWIN_ACPI_HID     "RKCP7410"

#define PANWIN_FILE_DEVICE  0x8D52u   /* custom device type, 0x8000-0xFFFF range */
#define PANWIN_ACCESS_RW    0x3u      /* FILE_READ_ACCESS | FILE_WRITE_ACCESS */
#define PANWIN_METHOD_BUFFERED 0u

/* CTL_CODE(DeviceType, Function, Method, Access) without needing winioctl.h / wdm.h. */
#define PANWIN_CTL_CODE(func) \
	((PANWIN_FILE_DEVICE << 16) | (PANWIN_ACCESS_RW << 14) | ((uint32_t)(func) << 2) | PANWIN_METHOD_BUFFERED)

/* Function code = 0x800 + DRM ioctl nr (_IOC_NR of the Linux ioctl number, 0x00..0xFF).
 * DRM core ioctls use nr 0x00-0x3F and 0xA0-0xCF, driver (panthor) ioctls 0x40 + DRM_PANTHOR_*. */
#define PANWIN_FUNC_DRM_BASE   0x800u
#define PANWIN_IOCTL_DRM(nr)   PANWIN_CTL_CODE(PANWIN_FUNC_DRM_BASE + ((nr) & 0xFFu))
#define PANWIN_IOCTL_TO_DRM_NR(code) ((((code) >> 2) & 0xFFFu) - PANWIN_FUNC_DRM_BASE)

/* Private ioctls: function codes 0x900+ (never collide with forwarded DRM ioctls). */
#define PANWIN_IOCTL_GET_INFO        PANWIN_CTL_CODE(0x900)
#define PANWIN_IOCTL_MMAP            PANWIN_CTL_CODE(0x901)
#define PANWIN_IOCTL_MUNMAP          PANWIN_CTL_CODE(0x902)
#define PANWIN_IOCTL_SYNCOBJ_EVENT   PANWIN_CTL_CODE(0x903)
#define PANWIN_IOCTL_DEBUG_REG_READ  PANWIN_CTL_CODE(0x9F0) /* debug builds of panwin.sys only */

#define PANWIN_ABI_VERSION 1u

struct panwin_info {
	uint32_t abi_version;     /* out: PANWIN_ABI_VERSION */
	uint32_t flags;           /* out: PANWIN_INFO_* */
	uint32_t drm_version_major; /* out: emulated panthor DRM driver version, e.g. 1 */
	uint32_t drm_version_minor; /* out: e.g. 5 (gates panvk features, see docs/research/mesa-panvk.md) */
	uint64_t gpu_phys_base;   /* out: MMIO base (diagnostics only) */
	char     description[96]; /* out: pw_device_describe() */
};
#define PANWIN_INFO_FW_BOOTED   (1u << 0)
#define PANWIN_INFO_DEBUG_BUILD (1u << 1)

/*
 * Replacement for mmap(fd, offset) on a DRM fd. `offset` is the fake offset returned by
 * DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, or DRM_PANTHOR_USER_MMIO_OFFSET (+ page) for the user MMIO
 * (LATEST_FLUSH_ID) page. The kernel maps the pages into the calling process and returns the VA.
 * Mappings are torn down by MUNMAP or when the file handle is closed.
 */
struct panwin_mmap {
	uint64_t offset;      /* in */
	uint64_t size;        /* in: bytes, page multiple */
	uint32_t prot;        /* in: PANWIN_PROT_* */
	uint32_t pad;
	uint64_t user_va;     /* out */
};
#define PANWIN_PROT_READ  1u
#define PANWIN_PROT_WRITE 2u

struct panwin_munmap {
	uint64_t user_va;     /* in */
	uint64_t size;        /* in */
};

/*
 * poll()/eventfd replacement: returns a Win32 event HANDLE (valid in the caller's process) that
 * the kernel signals when syncobj `handle` reaches `point` (0 = binary syncobj signalled). One-shot.
 */
struct panwin_syncobj_event {
	uint32_t handle;      /* in: syncobj handle */
	uint32_t flags;       /* in: DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT etc. */
	uint64_t point;       /* in */
	uint64_t event_handle;/* out: HANDLE */
};

struct panwin_debug_reg_read {
	uint32_t offset;      /* in: GPU register offset */
	uint32_t value;       /* out */
};

#endif /* PANWIN_IOCTL_H */
