/* SPDX-License-Identifier: MIT */
/*
 * Minimal transport DLL for the PANWIN_TRANSPORT=dll:<path> loader test: one device whose kernel
 * answers GET_INFO (ABI PANWIN_ABI_VERSION) and fails every DRM ioctl with ENOSYS.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "panwin/panwin_ioctl.h"
#include "panwin_transport.h"

static int td_enumerate(void) { return 1; }

static int td_device_path(int index, char *buf, size_t len)
{
	if (index != 0)
		return -ENOENT;
	snprintf(buf, len, "testdll:%d", index);
	return 0;
}

static int td_open(int index, void **file)
{
	if (index != 0)
		return -ENOENT;
	*file = (void *)(uintptr_t)0x5151;
	return 0;
}

static void td_close(void *file) { (void)file; }

static int32_t td_ioctl(void *file, uint32_t code, void *buf, uint32_t in_size, uint32_t out_size,
			uint32_t *returned)
{
	(void)file;
	(void)in_size;
	*returned = 0;
	if (code == PANWIN_IOCTL_GET_INFO && out_size == sizeof(struct panwin_info)) {
		struct panwin_info *i = buf;

		memset(i, 0, sizeof(*i));
		i->abi_version = PANWIN_ABI_VERSION;
		snprintf(i->description, sizeof(i->description), "transport DLL test");
		*returned = sizeof(*i);
		return 0;
	}
	return PANWIN_STATUS_FROM_ERRNO(38); /* Linux ENOSYS */
}

static void *td_os_handle(void *file) { return file; }

static const struct panwin_transport td = {
	.version = PANWIN_TRANSPORT_VERSION, .name = "testdll", .enumerate = td_enumerate,
	.device_path = td_device_path, .open = td_open, .close = td_close, .ioctl = td_ioctl,
	.os_handle = td_os_handle,
};

__declspec(dllexport) const struct panwin_transport *panwin_transport_entry(void);
__declspec(dllexport) const struct panwin_transport *panwin_transport_entry(void)
{
	return &td;
}
