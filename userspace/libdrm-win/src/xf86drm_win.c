/* SPDX-License-Identifier: MIT */
/*
 * The subset of the libdrm (xf86drm.h) API used by Mesa panfrost/panvk, the Vulkan runtime and
 * util, implemented over panwin.sys. Wrapper semantics (return conventions, struct setup) follow
 * upstream libdrm xf86drm.c (MIT, https://gitlab.freedesktop.org/mesa/drm).
 *
 * Return conventions match libdrm exactly: functions that return drmIoctl()'s result return -1
 * with errno set; drmSyncobjWait/TimelineWait return -errno; drmGetDevice* return -errno.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include <xf86drm.h>
#include "panwin/panwin_ioctl.h"
#include "libdrm_win.h"
#include "pw_internal.h"

#define memclear(s) memset(&(s), 0, sizeof(s))
/* From libdrm_macros.h (private upstream header), needed by DRM_NODE_NAME_MAX. */
#ifndef MAX3
#define MAX3(a, b, c) ((a) > (b) ? ((a) > (c) ? (a) : (c)) : ((b) > (c) ? (b) : (c)))
#endif

/* The DRM structs with pointer / size_t members must have their LP64 layout on LLP64 too. */
_Static_assert(sizeof(void *) == 8, "libdrm-win supports 64-bit Windows only");
_Static_assert(sizeof(struct drm_version) == 64, "drm_version layout differs from aarch64 Linux");

/* ---- core ioctl ---- */

int drmIoctl(int fd, unsigned long request, void *arg)
{
	uint32_t size = _IOC_SIZE(request);

	if (_IOC_TYPE(request) != DRM_IOCTL_BASE) {
		errno = ENOTTY;
		return -1;
	}
	if (size && !arg) {
		errno = EFAULT;
		return -1;
	}
	/* The Linux struct goes both ways verbatim, see panwin_ioctl.h. Like upstream libdrm,
	 * restart on EINTR (an alerted wait); EAGAIN is not retried (no busy loop on Windows). */
	int ret;
	do {
		ret = pw_fd_ioctl(fd, PANWIN_IOCTL_DRM(_IOC_NR(request)), size ? arg : NULL, size, size);
	} while (ret == -1 && errno == EINTR);
	return ret;
}

/* ---- version ---- */

static char *dup_n(const char *s, size_t n)
{
	char *d = malloc(n + 1);
	if (d) {
		memcpy(d, s ? s : "", s ? n : 0);
		d[s ? n : 0] = '\0';
	}
	return d;
}

drmVersionPtr drmGetVersion(int fd)
{
	struct drm_version v;
	drmVersionPtr r;
	char *name = NULL, *date = NULL, *desc = NULL;

	memclear(v);
	if (drmIoctl(fd, DRM_IOCTL_VERSION, &v))
		return NULL;

	/* Second pass: the kernel copies up to *_len bytes and reports the full lengths. */
	if (v.name_len)
		name = malloc(v.name_len + 1);
	if (v.date_len)
		date = malloc(v.date_len + 1);
	if (v.desc_len)
		desc = malloc(v.desc_len + 1);
	if ((v.name_len && !name) || (v.date_len && !date) || (v.desc_len && !desc))
		goto nomem;
	v.name = name;
	v.date = date;
	v.desc = desc;
	if (drmIoctl(fd, DRM_IOCTL_VERSION, &v)) {
		free(name);
		free(date);
		free(desc);
		return NULL;
	}
	if (name)
		name[v.name_len] = '\0';
	if (date)
		date[v.date_len] = '\0';
	if (desc)
		desc[v.desc_len] = '\0';

	r = calloc(1, sizeof(*r));
	if (!r)
		goto nomem;
	r->version_major = v.version_major;
	r->version_minor = v.version_minor;
	r->version_patchlevel = v.version_patchlevel;
	r->name_len = (int)v.name_len;
	r->name = name ? name : dup_n(NULL, 0);
	r->date_len = (int)v.date_len;
	r->date = date ? date : dup_n(NULL, 0);
	r->desc_len = (int)v.desc_len;
	r->desc = desc ? desc : dup_n(NULL, 0);
	if (!r->name || !r->date || !r->desc) {
		drmFreeVersion(r);
		errno = ENOMEM;
		return NULL;
	}
	return r;

nomem:
	free(name);
	free(date);
	free(desc);
	errno = ENOMEM;
	return NULL;
}

void drmFreeVersion(drmVersionPtr v)
{
	if (!v)
		return;
	free(v->name);
	free(v->date);
	free(v->desc);
	free(v);
}

drmVersionPtr drmGetLibVersion(int fd)
{
	drmVersionPtr v = calloc(1, sizeof(*v));
	(void)fd;
	if (!v)
		return NULL;
	v->version_major = 1;
	v->version_minor = 3;
	v->version_patchlevel = 0;
	v->name = dup_n(NULL, 0);
	v->date = dup_n(NULL, 0);
	v->desc = dup_n(NULL, 0);
	return v;
}

/* ---- caps ---- */

int drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	struct drm_get_cap cap;
	int ret;

	memclear(cap);
	cap.capability = capability;
	ret = drmIoctl(fd, DRM_IOCTL_GET_CAP, &cap);
	if (ret)
		return ret;
	*value = cap.value;
	return 0;
}

int drmSetClientCap(int fd, uint64_t capability, uint64_t value)
{
	struct drm_set_client_cap cap;

	memclear(cap);
	cap.capability = capability;
	cap.value = value;
	return drmIoctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
}

int drmIsMaster(int fd)
{
	(void)fd;
	return 0; /* render-only device, no master concept */
}

/* ---- driver commands ---- */

int drmCommandNone(int fd, unsigned long drmCommandIndex)
{
	unsigned long request = DRM_IO(DRM_COMMAND_BASE + drmCommandIndex);
	return drmIoctl(fd, request, NULL) ? -errno : 0;
}

int drmCommandRead(int fd, unsigned long drmCommandIndex, void *data, unsigned long size)
{
	unsigned long request = DRM_IOC(DRM_IOC_READ, DRM_IOCTL_BASE,
					DRM_COMMAND_BASE + drmCommandIndex, size);
	return drmIoctl(fd, request, data) ? -errno : 0;
}

int drmCommandWrite(int fd, unsigned long drmCommandIndex, void *data, unsigned long size)
{
	unsigned long request = DRM_IOC(DRM_IOC_WRITE, DRM_IOCTL_BASE,
					DRM_COMMAND_BASE + drmCommandIndex, size);
	return drmIoctl(fd, request, data) ? -errno : 0;
}

int drmCommandWriteRead(int fd, unsigned long drmCommandIndex, void *data, unsigned long size)
{
	unsigned long request = DRM_IOC(DRM_IOC_READ | DRM_IOC_WRITE, DRM_IOCTL_BASE,
					DRM_COMMAND_BASE + drmCommandIndex, size);
	return drmIoctl(fd, request, data) ? -errno : 0;
}

/* ---- open / close ---- */

int drmAvailable(void)
{
	return pw_transport()->enumerate() > 0;
}

int drmOpenWithType(const char *name, const char *busid, int type)
{
	char node[32];
	int n = pw_transport()->enumerate();

	(void)busid;
	if (name && strcmp(name, "panthor")) {
		errno = ENODEV;
		return -1;
	}
	if (n <= 0) {
		errno = n < 0 ? -n : ENODEV;
		return -1;
	}
	pw_format_node(node, sizeof(node), 0, type == DRM_NODE_RENDER ? DRM_NODE_RENDER : DRM_NODE_PRIMARY);
	return panwin_open(node, 0);
}

int drmOpen(const char *name, const char *busid)
{
	return drmOpenWithType(name, busid, DRM_NODE_PRIMARY);
}

int drmOpenRender(int minor)
{
	char node[32];
	snprintf(node, sizeof(node), "/dev/dri/renderD%d", minor);
	return panwin_open(node, 0);
}

int drmClose(int fd)
{
	return panwin_close(fd);
}

/* ---- devices ---- */

/* Matches the Linux DT node of the RK3588 GPU so existing matching logic keeps working. */
static const char platform_fullname[] = "/gpu@fb000000";
static const char *const platform_compatible[] = { "rockchip,rk3588-mali", "arm,mali-valhall-csf" };

/* One allocation per device (freed by drmFreeDevice with a single free()), like libdrm. */
static drmDevicePtr make_device(int index)
{
	size_t compat_strs = 0;
	for (size_t i = 0; i < PW_ARRAY_SIZE(platform_compatible); i++)
		compat_strs += strlen(platform_compatible[i]) + 1;

	size_t sz = sizeof(drmDevice) + DRM_NODE_MAX * sizeof(char *) + DRM_NODE_MAX * DRM_NODE_NAME_MAX +
		    sizeof(drmPlatformBusInfo) + sizeof(drmPlatformDeviceInfo) +
		    (PW_ARRAY_SIZE(platform_compatible) + 1) * sizeof(char *) + compat_strs + 16;
	char *mem = calloc(1, sz);
	if (!mem)
		return NULL;

	char *p = mem;
	drmDevicePtr d = (drmDevicePtr)p;
	p += sizeof(drmDevice);
	d->nodes = (char **)p;
	p += DRM_NODE_MAX * sizeof(char *);
	d->bustype = DRM_BUS_PLATFORM;
	d->businfo.platform = (drmPlatformBusInfoPtr)p;
	p += sizeof(drmPlatformBusInfo);
	d->deviceinfo.platform = (drmPlatformDeviceInfoPtr)p;
	p += sizeof(drmPlatformDeviceInfo);
	d->deviceinfo.platform->compatible = (char **)p;
	p += (PW_ARRAY_SIZE(platform_compatible) + 1) * sizeof(char *);
	for (size_t i = 0; i < PW_ARRAY_SIZE(platform_compatible); i++) {
		size_t l = strlen(platform_compatible[i]) + 1;
		memcpy(p, platform_compatible[i], l);
		d->deviceinfo.platform->compatible[i] = p;
		p += l;
	}
	snprintf(d->businfo.platform->fullname, sizeof(d->businfo.platform->fullname), "%s",
		 platform_fullname);

	/* Only a render node exists: panwin.sys has no KMS / master concept. */
	d->available_nodes = 1 << DRM_NODE_RENDER;
	for (int t = 0; t < DRM_NODE_MAX; t++) {
		d->nodes[t] = p;
		if (t == DRM_NODE_RENDER)
			pw_format_node(p, DRM_NODE_NAME_MAX, index, DRM_NODE_RENDER);
		p += DRM_NODE_NAME_MAX;
	}
	return d;
}

int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	int n = pw_transport()->enumerate();

	(void)flags;
	if (n < 0)
		return n;
	if (!devices)
		return n;
	if (max_devices < 0)
		return -EINVAL;
	int count = n < max_devices ? n : max_devices;
	for (int i = 0; i < count; i++) {
		devices[i] = make_device(i);
		if (!devices[i]) {
			drmFreeDevices(devices, i);
			return -ENOMEM;
		}
	}
	return count;
}

int drmGetDevices(drmDevicePtr devices[], int max_devices)
{
	return drmGetDevices2(DRM_DEVICE_GET_PCI_REVISION, devices, max_devices);
}

int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	struct pw_file *f;
	int index;

	(void)flags;
	if (!device)
		return -EINVAL;
	f = pw_file_get(fd);
	if (!f)
		return -EBADF;
	index = f->index;
	pw_file_put(f);
	*device = make_device(index);
	return *device ? 0 : -ENOMEM;
}

int drmGetDevice(int fd, drmDevicePtr *device)
{
	return drmGetDevice2(fd, DRM_DEVICE_GET_PCI_REVISION, device);
}

static int devid_to_index(dev_t devid, int *type)
{
	uint64_t d = (uint64_t)devid;
	unsigned maj = (unsigned)((d >> 8) & 0xfff), min = (unsigned)(d & 0xff);

	if (maj != PANWIN_DRM_MAJOR)
		return -ENODEV;
	if (min >= PW_RENDER_MINOR_BASE && min < PW_RENDER_MINOR_BASE + PW_MAX_DEVICES) {
		*type = DRM_NODE_RENDER;
		return (int)(min - PW_RENDER_MINOR_BASE);
	}
	if (min < PW_PRIMARY_MINOR_BASE + PW_MAX_DEVICES) {
		*type = DRM_NODE_PRIMARY;
		return (int)(min - PW_PRIMARY_MINOR_BASE);
	}
	return -ENODEV;
}

int drmGetDeviceFromDevId(dev_t dev_id, uint32_t flags, drmDevicePtr *device)
{
	int type, index = devid_to_index(dev_id, &type);

	(void)flags;
	if (!device)
		return -EINVAL;
	if (index < 0)
		return index;
	if (index >= pw_transport()->enumerate())
		return -ENODEV;
	*device = make_device(index);
	return *device ? 0 : -ENOMEM;
}

int drmGetNodeTypeFromDevId(dev_t devid)
{
	int type, index = devid_to_index(devid, &type);
	return index < 0 ? index : type;
}

void drmFreeDevice(drmDevicePtr *device)
{
	if (!device)
		return;
	free(*device);
	*device = NULL;
}

void drmFreeDevices(drmDevicePtr devices[], int count)
{
	if (!devices)
		return;
	for (int i = 0; i < count; i++)
		drmFreeDevice(&devices[i]);
}

int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
	if (a == NULL || b == NULL)
		return 0;
	if (a->bustype != b->bustype)
		return 0;
	if (a->bustype != DRM_BUS_PLATFORM)
		return 0;
	/* Same synthetic bus info for every device: tell them apart by render node. */
	if (strcmp(a->businfo.platform->fullname, b->businfo.platform->fullname))
		return 0;
	if ((a->available_nodes & (1 << DRM_NODE_RENDER)) && (b->available_nodes & (1 << DRM_NODE_RENDER)))
		return !strcmp(a->nodes[DRM_NODE_RENDER], b->nodes[DRM_NODE_RENDER]);
	return 1;
}

int drmGetNodeTypeFromFd(int fd)
{
	if (!panwin_is_fd(fd)) {
		errno = EBADF;
		return -1;
	}
	return DRM_NODE_RENDER;
}

char *drmGetDeviceNameFromFd2(int fd)
{
	struct pw_file *f = pw_file_get(fd);
	char node[32];

	if (!f)
		return NULL;
	pw_format_node(node, sizeof(node), f->index, DRM_NODE_RENDER);
	pw_file_put(f);
	return _strdup(node);
}

char *drmGetRenderDeviceNameFromFd(int fd)
{
	return drmGetDeviceNameFromFd2(fd);
}

char *drmGetPrimaryDeviceNameFromFd(int fd)
{
	(void)fd;
	return NULL; /* no primary node */
}

/* ---- GEM / PRIME ---- */

int drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close args;

	memclear(args);
	args.handle = handle;
	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &args);
}

/* dma-buf fds do not exist on Windows (yet): NT-handle based sharing is a TODO. */
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)handle;
	(void)flags;
	(void)prime_fd;
	if (!panwin_is_fd(fd)) {
		errno = EBADF;
		return -1;
	}
	errno = ENOSYS;
	return -1;
}

int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	(void)prime_fd;
	(void)handle;
	if (!panwin_is_fd(fd)) {
		errno = EBADF;
		return -1;
	}
	errno = ENOSYS;
	return -1;
}

/* ---- syncobj ---- */

int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	struct drm_syncobj_create args;
	int ret;

	memclear(args);
	args.flags = flags;
	ret = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &args);
	if (ret)
		return ret;
	*handle = args.handle;
	return 0;
}

int drmSyncobjDestroy(int fd, uint32_t handle)
{
	struct drm_syncobj_destroy args;

	memclear(args);
	args.handle = handle;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &args);
}

/* Syncobj/sync-file fds need kernel-created fds, which Windows does not have. */
static int no_fd_support(int fd)
{
	errno = panwin_is_fd(fd) ? ENOSYS : EBADF;
	return -1;
}

int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	(void)handle;
	(void)obj_fd;
	return no_fd_support(fd);
}

int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	(void)obj_fd;
	(void)handle;
	return no_fd_support(fd);
}

int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	(void)handle;
	(void)sync_file_fd;
	return no_fd_support(fd);
}

int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	(void)handle;
	(void)sync_file_fd;
	return no_fd_support(fd);
}

int drmSyncobjEventfd(int fd, uint32_t handle, uint64_t point, int ev_fd, uint32_t flags)
{
	(void)handle;
	(void)point;
	(void)ev_fd;
	(void)flags;
	return no_fd_support(fd); /* use panwin_syncobj_event() instead */
}

int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec,
		   unsigned flags, uint32_t *first_signaled)
{
	struct drm_syncobj_wait args;
	int ret;

	memclear(args);
	args.handles = (uintptr_t)handles;
	args.timeout_nsec = timeout_nsec;
	args.count_handles = num_handles;
	args.flags = flags;

	ret = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &args);
	if (ret < 0)
		return -errno;
	if (first_signaled)
		*first_signaled = args.first_signaled;
	return ret;
}

int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
	struct drm_syncobj_array args;

	memclear(args);
	args.handles = (uintptr_t)handles;
	args.count_handles = handle_count;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &args);
}

int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
	struct drm_syncobj_array args;

	memclear(args);
	args.handles = (uintptr_t)handles;
	args.count_handles = handle_count;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &args);
}

int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points,
			     uint32_t handle_count)
{
	struct drm_syncobj_timeline_array args;

	memclear(args);
	args.handles = (uintptr_t)handles;
	args.points = (uintptr_t)points;
	args.count_handles = handle_count;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &args);
}

int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
			   int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	struct drm_syncobj_timeline_wait args;
	int ret;

	memclear(args);
	args.handles = (uintptr_t)handles;
	args.points = (uintptr_t)points;
	args.timeout_nsec = timeout_nsec;
	args.count_handles = num_handles;
	args.flags = flags;

	ret = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &args);
	if (ret < 0)
		return -errno;
	if (first_signaled)
		*first_signaled = args.first_signaled;
	return ret;
}

int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count,
		     uint32_t flags)
{
	struct drm_syncobj_timeline_array args;

	memclear(args);
	args.handles = (uintptr_t)handles;
	args.points = (uintptr_t)points;
	args.count_handles = handle_count;
	args.flags = flags;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_QUERY, &args);
}

int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count)
{
	return drmSyncobjQuery2(fd, handles, points, handle_count, 0);
}

int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
		       uint64_t src_point, uint32_t flags)
{
	struct drm_syncobj_transfer args;

	memclear(args);
	args.src_handle = src_handle;
	args.dst_handle = dst_handle;
	args.src_point = src_point;
	args.dst_point = dst_point;
	args.flags = flags;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &args);
}
