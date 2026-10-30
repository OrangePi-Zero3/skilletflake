/* SPDX-License-Identifier: MIT */
/*
 * Linux-compatible ioctl number encoding for Windows builds, so that Mesa's drm-uapi headers
 * (drm.h takes this path on non-Linux) produce the exact same DRM_IOCTL_* values as on Linux:
 *   bits 0-7 nr, 8-15 type, 16-29 size, 30-31 direction.
 * libdrm-win extracts nr/size from these values to build PANWIN_IOCTL_DRM(nr) codes.
 */
#ifndef PANWIN_COMPAT_SYS_IOCCOM_H
#define PANWIN_COMPAT_SYS_IOCCOM_H

#define _IOC_NRBITS    8
#define _IOC_TYPEBITS  8
#define _IOC_SIZEBITS  14
#define _IOC_DIRBITS   2

#define _IOC_NRSHIFT   0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT  (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE  0U
#define _IOC_WRITE 1U
#define _IOC_READ  2U

#define _IOC(dir, type, nr, size) \
	((unsigned long)(((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | \
			 ((nr) << _IOC_NRSHIFT) | ((unsigned long)(size) << _IOC_SIZESHIFT)))

#define _IO(type, nr)         _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, t)     _IOC(_IOC_READ, (type), (nr), sizeof(t))
#define _IOW(type, nr, t)     _IOC(_IOC_WRITE, (type), (nr), sizeof(t))
#define _IOWR(type, nr, t)    _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(t))

#define _IOC_DIR(n)  (((n) >> _IOC_DIRSHIFT) & ((1U << _IOC_DIRBITS) - 1))
#define _IOC_TYPE(n) (((n) >> _IOC_TYPESHIFT) & ((1U << _IOC_TYPEBITS) - 1))
#define _IOC_NR(n)   (((n) >> _IOC_NRSHIFT) & ((1U << _IOC_NRBITS) - 1))
#define _IOC_SIZE(n) (((n) >> _IOC_SIZESHIFT) & ((1U << _IOC_SIZEBITS) - 1))

#endif
