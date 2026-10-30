/* SPDX-License-Identifier: MIT */
/*
 * Linux DRM ioctl number -> PANWIN_IOCTL_DRM(nr) mapping checks.
 * Most checks are compile-time. The expected Linux values were printed by a host program built
 * against references/linux/include/uapi (and are identical in the Windows build), so if include/compat/sys/ioccom.h or the uAPI
 * struct layouts drift on Windows, this file stops compiling.
 */
#include <errno.h>
#include <stddef.h>
#include <stdio.h>

#include <xf86drm.h>
#include "drm-uapi/panthor_drm.h"
#include "panwin/panwin_ioctl.h"
#include "libdrm_win.h"

#define SA(x) _Static_assert(x, #x)

/* Linux ioctl numbers, exactly as on aarch64 Linux. */
SA(DRM_IOCTL_VERSION == 0xC0406400u);
SA(DRM_IOCTL_GEM_CLOSE == 0x40086409u);
SA(DRM_IOCTL_GET_CAP == 0xC010640Cu);
SA(DRM_IOCTL_SET_CLIENT_CAP == 0x4010640Du);
SA(DRM_IOCTL_SYNCOBJ_CREATE == 0xC00864BFu);
SA(DRM_IOCTL_SYNCOBJ_DESTROY == 0xC00864C0u);
SA(DRM_IOCTL_SYNCOBJ_WAIT == 0xC02864C3u); /* includes deadline_nsec */
SA(DRM_IOCTL_SYNCOBJ_RESET == 0xC01064C4u);
SA(DRM_IOCTL_SYNCOBJ_SIGNAL == 0xC01064C5u);
SA(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT == 0xC03064CAu);
SA(DRM_IOCTL_SYNCOBJ_QUERY == 0xC01864CBu);
SA(DRM_IOCTL_SYNCOBJ_TRANSFER == 0xC02064CCu);
SA(DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL == 0xC01864CDu);
SA(DRM_IOCTL_PANTHOR_DEV_QUERY == 0xC0106440u);
SA(DRM_IOCTL_PANTHOR_VM_CREATE == 0xC0106441u);
SA(DRM_IOCTL_PANTHOR_VM_BIND == 0xC0186443u);
SA(DRM_IOCTL_PANTHOR_BO_CREATE == 0xC0186445u);
SA(DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET == 0xC0106446u);
SA(DRM_IOCTL_PANTHOR_GROUP_SUBMIT == 0xC0186449u);

/* nr extraction and the panwin function-code mapping. */
SA(_IOC_NR(DRM_IOCTL_PANTHOR_DEV_QUERY) == DRM_COMMAND_BASE + DRM_PANTHOR_DEV_QUERY);
SA(_IOC_NR(DRM_IOCTL_PANTHOR_BO_QUERY_INFO) == 0x40 + 16);
SA(_IOC_SIZE(DRM_IOCTL_PANTHOR_DEV_QUERY) == sizeof(struct drm_panthor_dev_query));
SA(_IOC_TYPE(DRM_IOCTL_SYNCOBJ_WAIT) == DRM_IOCTL_BASE);
/* CTL_CODE(0x8D52, 0x800 + nr, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS) */
SA(PANWIN_IOCTL_DRM(_IOC_NR(DRM_IOCTL_VERSION)) == 0x8D52E000u);
SA(PANWIN_IOCTL_DRM(_IOC_NR(DRM_IOCTL_PANTHOR_DEV_QUERY)) == 0x8D52E100u);
SA(PANWIN_IOCTL_DRM(_IOC_NR(DRM_IOCTL_PANTHOR_GROUP_SUBMIT)) == 0x8D52E124u);
SA(PANWIN_IOCTL_DRM(_IOC_NR(DRM_IOCTL_SYNCOBJ_CREATE)) == 0x8D52E2FCu);
SA(PANWIN_IOCTL_DRM(_IOC_NR(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT)) == 0x8D52E328u);
SA(PANWIN_IOCTL_TO_DRM_NR(PANWIN_IOCTL_DRM(0xCD)) == 0xCD);
SA(PANWIN_IOCTL_GET_INFO == 0x8D52E400u);
SA(PANWIN_IOCTL_DEBUG_REG_READ == 0x8D52E7C0u);

/* uAPI struct layouts: identical to aarch64 Linux (all fixed-width / LP64-compatible). */
SA(sizeof(struct drm_version) == 64);
SA(sizeof(struct drm_panthor_gpu_info) == 112);
SA(offsetof(struct drm_panthor_gpu_info, shader_present) == 72);
SA(sizeof(struct drm_panthor_csif_info) == 24);
SA(sizeof(struct drm_panthor_group_create) == 56);
SA(sizeof(struct drm_panthor_queue_submit) == 40);
SA(sizeof(struct drm_panthor_vm_bind_op) == 48);
SA(sizeof(struct panwin_info) == 120);
SA(sizeof(struct panwin_mmap) == 32);
SA(sizeof(struct panwin_syncobj_event) == 24);

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
	/* Round trip for every nr, and private codes never fall into the DRM window. */
	for (unsigned nr = 0; nr < 256; nr++) {
		unsigned code = PANWIN_IOCTL_DRM(nr);
		CHECK(PANWIN_IOCTL_TO_DRM_NR(code) == nr);
		CHECK((code >> 16) == PANWIN_FILE_DEVICE);
		CHECK((code & 3) == PANWIN_METHOD_BUFFERED);
		CHECK(code != PANWIN_IOCTL_GET_INFO && code != PANWIN_IOCTL_MMAP &&
		      code != PANWIN_IOCTL_MUNMAP && code != PANWIN_IOCTL_SYNCOBJ_EVENT &&
		      code != PANWIN_IOCTL_DEBUG_REG_READ);
	}

	/* Behaviour that needs no device. */
	errno = 0;
	CHECK(drmIoctl(0x4000, DRM_IOCTL_VERSION, &(struct drm_version){ 0 }) == -1 && errno == EBADF);
	errno = 0;
	CHECK(drmIoctl(0x4000, _IOWR('x', 1, int), &(int){ 0 }) == -1 && errno == ENOTTY);
	CHECK(!panwin_is_fd(0) && !panwin_is_fd(1) && !panwin_is_fd(PANWIN_FD_BASE));

	printf("DRM_IOCTL_PANTHOR_DEV_QUERY 0x%08lx -> PANWIN 0x%08x\n",
	       (unsigned long)DRM_IOCTL_PANTHOR_DEV_QUERY,
	       PANWIN_IOCTL_DRM(_IOC_NR(DRM_IOCTL_PANTHOR_DEV_QUERY)));
	printf("DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT 0x%08lx -> PANWIN 0x%08x\n",
	       (unsigned long)DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT,
	       PANWIN_IOCTL_DRM(_IOC_NR(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT)));
	printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
	return failures ? 1 : 0;
}
