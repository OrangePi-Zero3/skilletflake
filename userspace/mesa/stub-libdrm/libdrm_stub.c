/* SPDX-License-Identifier: MIT */
/*
 * Temporary stand-in for libdrm-win's libdrm.dll, used by build.sh only when the real
 * libdrm-win cannot be built. It exports the symbols Mesa's panvk imports, with the
 * libdrm-win prototypes (include/xf86drm.h + libdrm_win.h), and reports "no device"
 * everywhere: drmGetDevices2() finds nothing, every fd operation fails with ENODEV.
 * That is enough to link vulkan_panfrost.dll and to load it in a loader-less smoke test.
 */
#include <errno.h>
#include <stdlib.h>

#include <xf86drm.h>
#include <libdrm_win.h>

#define STUB_FAIL(err) do { errno = (err); return -1; } while (0)

int drmIoctl(int fd, unsigned long request, void *arg) { STUB_FAIL(ENODEV); }
drmVersionPtr drmGetVersion(int fd) { errno = ENODEV; return NULL; }
void drmFreeVersion(drmVersionPtr v) { free(v); }
int drmGetCap(int fd, uint64_t capability, uint64_t *value) { STUB_FAIL(ENODEV); }

int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices) { return 0; }
void drmFreeDevices(drmDevicePtr devices[], int count) { }
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device) { STUB_FAIL(ENODEV); }
void drmFreeDevice(drmDevicePtr *device) { }

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd) { STUB_FAIL(ENOSYS); }
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle) { STUB_FAIL(ENOSYS); }
int drmCloseBufferHandle(int fd, uint32_t handle) { STUB_FAIL(ENODEV); }

int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle) { STUB_FAIL(ENODEV); }
int drmSyncobjDestroy(int fd, uint32_t handle) { STUB_FAIL(ENODEV); }
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd) { STUB_FAIL(ENOSYS); }
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle) { STUB_FAIL(ENOSYS); }
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd) { STUB_FAIL(ENOSYS); }
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd) { STUB_FAIL(ENOSYS); }
int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec,
		   unsigned flags, uint32_t *first_signaled) { STUB_FAIL(ENODEV); }
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count) { STUB_FAIL(ENODEV); }
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count) { STUB_FAIL(ENODEV); }
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points,
			     uint32_t handle_count) { STUB_FAIL(ENODEV); }
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
			   int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{ STUB_FAIL(ENODEV); }
int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count)
{ STUB_FAIL(ENODEV); }
int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count,
		     uint32_t flags) { STUB_FAIL(ENODEV); }
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
		       uint64_t src_point, uint32_t flags) { STUB_FAIL(ENODEV); }
int drmSyncobjEventfd(int fd, uint32_t handle, uint64_t point, int ev_fd, uint32_t flags)
{ STUB_FAIL(ENOSYS); }

int panwin_open(const char *path, int flags) { STUB_FAIL(ENOENT); }
int panwin_close(int fd) { STUB_FAIL(EBADF); }
int panwin_dup(int fd) { STUB_FAIL(EBADF); }
int panwin_stat_rdev(const char *path, uint64_t *rdev) { STUB_FAIL(ENOENT); }
void *panwin_mmap(int fd, uint64_t offset, size_t size, int prot) { errno = ENODEV; return NULL; }
int panwin_munmap(void *ptr, size_t size) { STUB_FAIL(EINVAL); }
