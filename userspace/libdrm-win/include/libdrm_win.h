/* SPDX-License-Identifier: MIT */
/*
 * libdrm_win.h - Windows-only extensions exported by libdrm-win (libdrm.dll).
 *
 * Windows has no DRM file descriptors, mmap() or /dev/dri. libdrm-win emulates them:
 *
 *  - "DRM fds" are small ints >= PANWIN_FD_BASE (0x4000) handed out by panwin_open(); they never
 *    collide with CRT fds and are only meaningful to this DLL (drm*() and panwin_*()).
 *    Mesa's open()/close()/dup()/mmap() call sites must be routed here (see docs/status/libdrm-win.md).
 *  - Device nodes are synthesized: device i (i-th panwin.sys interface) is "/dev/dri/renderD<128+i>".
 *  - Several fds can share one kernel file (panwin_dup), like dup() on Linux: GEM/syncobj handles
 *    stay valid across them. The kernel file is closed when the last fd AND the last mapping made
 *    through it are gone (mappings keep the file alive, as on Linux).
 *
 * All functions are thread-safe. Unless stated otherwise they return 0 / a value >= 0 on success
 * and -1 with errno set on failure (POSIX style), like the calls they replace.
 */
#ifndef LIBDRM_WIN_H
#define LIBDRM_WIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PANWIN_FD_BASE 0x4000
#define PANWIN_FD_MAX  1024 /* max simultaneously open emulated fds */

/* Linux-style device numbers for the synthetic nodes: makedev(226, minor), glibc encoding for
 * minor < 256, i.e. (226 << 8) | minor. Render minors are 128 + device index. */
#define PANWIN_DRM_MAJOR 226
#define PANWIN_MAKEDEV(maj, min) ((((uint64_t)(maj) & 0xfffu) << 8) | ((uint64_t)(min) & 0xffu))

/* flags for panwin_open: O_RDWR/O_CLOEXEC etc. are accepted and ignored. */
int panwin_open(const char *path, int flags);
int panwin_close(int fd);
int panwin_dup(int fd);                  /* os_dupfd_cloexec() replacement */
int panwin_is_fd(int fd);                /* 1 if fd is a libdrm-win fd, else 0 (never sets errno) */
void *panwin_fd_to_handle(int fd);       /* underlying HANDLE (NULL + errno=EBADF if invalid; the
					  * fake-kernel transport returns a non-HANDLE cookie) */

/* stat() replacement for the synthetic node paths: *rdev = PANWIN_MAKEDEV(226, minor).
 * Returns -1/ENOENT for any other path. */
int panwin_stat_rdev(const char *path, uint64_t *rdev);

/* mmap()/munmap() replacement on a DRM fd (PANWIN_IOCTL_MMAP/MUNMAP). prot is PROT_READ(1) |
 * PROT_WRITE(2), the same bit values as PANWIN_PROT_*. Returns NULL (not MAP_FAILED) + errno. */
void *panwin_mmap(int fd, uint64_t offset, size_t size, int prot);
/* Returns 0, or -1 with errno = EINVAL if ptr was not returned by panwin_mmap (so a Mesa os_munmap
 * wrapper can fall back to VirtualFree for its own anonymous mappings). */
int panwin_munmap(void *ptr, size_t size);
int panwin_is_mapping(const void *ptr);  /* 1 if ptr is the start of a live panwin_mmap mapping */

/* PANWIN_IOCTL_SYNCOBJ_EVENT: one-shot Win32 event (returned as void * = HANDLE, caller
 * CloseHandle()s it) signalled when syncobj `handle` reaches `point` (0 = binary). */
int panwin_syncobj_event(int fd, uint32_t handle, uint64_t point, uint32_t flags, void **event);

/* Raw private-ioctl access (PANWIN_IOCTL_GET_INFO, DEBUG_REG_READ, ...): buf is in+out. */
int panwin_private_ioctl(int fd, uint32_t code, void *buf, uint32_t size);

/* Map a raw NTSTATUS / Win32 error code to a Linux errno value (positive). Exposed for tests. */
int panwin_ntstatus_to_errno(int32_t status);
int panwin_win32_error_to_errno(uint32_t err);

/* Test hook: route all devices to the in-process fake kernel (only in builds configured with
 * -Dfake_kernel=true; otherwise returns -1/ENOSYS). Fails with EBUSY while any fd or mapping is
 * open. Also enabled at first use by the environment variable PANWIN_FAKE_KERNEL=1. */
int panwin_use_fake_kernel(int enable);

/* Kernel-side (Win32) path of device `index` as UTF-8, e.g. \\?\ACPI#RKCP7410#..., for
 * diagnostics. Returns 0, or -1 with errno = ENOENT/EOVERFLOW. */
int panwin_device_path(int index, char *buf, size_t len);

/* "real" (DeviceIoControl) or "fake". */
const char *panwin_transport_name(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBDRM_WIN_H */
