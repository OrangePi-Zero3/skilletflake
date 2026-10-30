/* SPDX-License-Identifier: MIT */
/*
 * NTSTATUS / Win32 error -> errno mapping.
 *
 * The standard-status table keeps the ABI v1 mapping of kmd/win (STATUS_DEVICE_NOT_READY =
 * ENODEV, STATUS_QUOTA_EXCEEDED = ENOSPC, ...) so an old driver still decodes sensibly; ABI v2
 * panwin.sys sends errno-carrying statuses for every failure it produces itself.
 *
 * panwin.sys completes IRPs with an NTSTATUS. Two forms are understood:
 *  1. ABI v2 (include/panwin/panwin_ioctl.h): a failing DRM / private ioctl completes with
 *     PANWIN_STATUS_FROM_ERRNO(e) = 0xE0000000 | (0xD52 << 16) | e (sev=error, customer bit,
 *     facility 0xD52, e = positive Linux errno from the OS-neutral core). Linux errno numbers
 *     differ from the Windows CRT's (ETIME is 62 on Linux, 137 in UCRT), so they are translated
 *     to CRT errno values here; ETIME and ETIMEDOUT stay distinct.
 *  2. Standard NTSTATUS codes, produced by the I/O manager / KMDF (invalid handle, buffer
 *     checks, unknown ioctl = STATUS_INVALID_DEVICE_REQUEST, ...), mapped with the table below.
 */
#include <errno.h>
#include <stdint.h>
#include <windows.h>

#include "panwin/panwin_ioctl.h"
#include "libdrm_win.h"
#include "pw_internal.h"

/* Linux asm-generic errno numbers -> Windows CRT errno. */
static int linux_errno_to_crt(unsigned e)
{
	switch (e) {
	case 1: return EPERM;
	case 2: return ENOENT;
	case 3: return ESRCH;
	case 4: return EINTR;
	case 5: return EIO;
	case 6: return ENXIO;
	case 7: return E2BIG;
	case 9: return EBADF;
	case 11: return EAGAIN;
	case 12: return ENOMEM;
	case 13: return EACCES;
	case 14: return EFAULT;
	case 16: return EBUSY;
	case 17: return EEXIST;
	case 18: return EXDEV;
	case 19: return ENODEV;
	case 22: return EINVAL;
	case 23: return ENFILE;
	case 24: return EMFILE;
	case 25: return ENOTTY;
	case 27: return EFBIG;
	case 28: return ENOSPC;
	case 32: return EPIPE;
	case 34: return ERANGE;
	case 35: return EDEADLK;
	case 36: return ENAMETOOLONG;
	case 38: return ENOSYS;
	case 61: return ENODATA;
	case 62: return ETIME;
	case 71: return EPROTO;
	case 74: return EBADMSG;
	case 75: return EOVERFLOW;
	case 95: return EOPNOTSUPP;
	case 105: return ENOBUFS;
	case 110: return ETIMEDOUT;
	case 125: return ECANCELED;
	default: return EIO;
	}
}

int panwin_ntstatus_to_errno(int32_t status)
{
	uint32_t s = (uint32_t)status;

	if (PANWIN_STATUS_IS_ERRNO(s))
		return linux_errno_to_crt((unsigned)PANWIN_STATUS_TO_ERRNO(s));

	switch (s) {
	case 0x00000000u: return 0;                 /* STATUS_SUCCESS */
	case 0x00000102u: return ETIME;             /* STATUS_TIMEOUT (success class, still a timeout) */
	case 0x000000C0u:                           /* STATUS_USER_APC */
	case 0x00000101u: return EINTR;             /* STATUS_ALERTED */
	case 0x80000005u: return EOVERFLOW;         /* STATUS_BUFFER_OVERFLOW */
	case 0x8000000Fu: return ENODEV;            /* STATUS_DEVICE_POWERED_OFF (kmd: GPU not up) */
	case 0x80000011u: return EBUSY;             /* STATUS_DEVICE_BUSY */
	case 0xC0000001u: return EIO;               /* STATUS_UNSUCCESSFUL */
	case 0xC0000002u: return ENOSYS;            /* STATUS_NOT_IMPLEMENTED */
	case 0xC0000005u: return EFAULT;            /* STATUS_ACCESS_VIOLATION */
	case 0xC0000008u: return EBADF;             /* STATUS_INVALID_HANDLE */
	case 0xC000000Du: return EINVAL;            /* STATUS_INVALID_PARAMETER */
	case 0xC000000Eu: return ENODEV;            /* STATUS_NO_SUCH_DEVICE */
	case 0xC000000Fu: return ENOENT;            /* STATUS_NO_SUCH_FILE */
	case 0xC0000010u: return ENOTTY;            /* STATUS_INVALID_DEVICE_REQUEST (unknown ioctl) */
	case 0xC0000017u: return ENOMEM;            /* STATUS_NO_MEMORY */
	case 0xC0000022u: return EACCES;            /* STATUS_ACCESS_DENIED */
	case 0xC0000023u: return EINVAL;            /* STATUS_BUFFER_TOO_SMALL */
	case 0xC0000024u: return EINVAL;            /* STATUS_OBJECT_TYPE_MISMATCH */
	case 0xC0000034u: return ENOENT;            /* STATUS_OBJECT_NAME_NOT_FOUND */
	case 0xC0000044u: return ENOSPC;            /* STATUS_QUOTA_EXCEEDED (kmd: -ENOSPC) */
	case 0xC0000095u: return EOVERFLOW;         /* STATUS_INTEGER_OVERFLOW */
	case 0xC000009Au: return ENOMEM;            /* STATUS_INSUFFICIENT_RESOURCES */
	case 0xC00000A3u: return ENODEV;            /* STATUS_DEVICE_NOT_READY (kmd: -ENODEV) */
	case 0xC00000B5u: return ETIME;             /* STATUS_IO_TIMEOUT */
	case 0xC00000BBu: return EOPNOTSUPP;        /* STATUS_NOT_SUPPORTED */
	case 0xC00000C0u: return ENODEV;            /* STATUS_DEVICE_DOES_NOT_EXIST */
	case 0xC00000E8u: return EFAULT;            /* STATUS_INVALID_USER_BUFFER */
	case 0xC0000120u: return ECANCELED;         /* STATUS_CANCELLED */
	case 0xC0000184u: return ENODEV;            /* STATUS_INVALID_DEVICE_STATE */
	case 0xC0000185u: return EIO;               /* STATUS_IO_DEVICE_ERROR (kmd: -EIO) */
	case 0xC000022Du: return EAGAIN;            /* STATUS_RETRY */
	case 0xC0000225u: return ENOENT;            /* STATUS_NOT_FOUND */
	case 0xC00002B6u: return ENODEV;            /* STATUS_DEVICE_REMOVED */
	case 0xC0000483u: return EIO;               /* STATUS_DEVICE_HARDWARE_ERROR */
	default:
		break;
	}
	if (s >= 0xC00000EFu && s <= 0xC00000FAu)   /* STATUS_INVALID_PARAMETER_1..12 */
		return EINVAL;
	return EIO;
}

int pw_ioctl_status_to_errno(int32_t status)
{
	uint32_t s = (uint32_t)status;

	if (s == 0)
		return 0;
	/* STATUS_TIMEOUT, STATUS_USER_APC and STATUS_ALERTED are success-class but mean the
	 * request did not complete (ETIME / EINTR); every other success code is success. */
	if (status > 0 && s != 0x102u && s != 0xC0u && s != 0x101u)
		return 0;
	return panwin_ntstatus_to_errno(status);
}

int panwin_win32_error_to_errno(uint32_t err)
{
	switch (err) {
	case ERROR_SUCCESS: return 0;
	case ERROR_INVALID_FUNCTION: return ENOTTY;
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_NOT_FOUND: return ENOENT;
	case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
	case ERROR_ACCESS_DENIED: return EACCES;
	case ERROR_INVALID_HANDLE: return EBADF;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:
	case ERROR_NO_SYSTEM_RESOURCES: return ENOMEM;
	case ERROR_INVALID_PARAMETER:
	case ERROR_INSUFFICIENT_BUFFER:
	case ERROR_INVALID_DATA: return EINVAL;
	case ERROR_NOT_READY: return EAGAIN;
	case ERROR_SHARING_VIOLATION:
	case ERROR_BUSY: return EBUSY;
	case ERROR_NOT_SUPPORTED: return EOPNOTSUPP;
	case ERROR_CALL_NOT_IMPLEMENTED: return ENOSYS;
	case ERROR_SEM_TIMEOUT:
	case WAIT_TIMEOUT: return ETIME;
	case ERROR_NOACCESS: return EFAULT;
	case ERROR_OPERATION_ABORTED: return ECANCELED;
	case ERROR_DEV_NOT_EXIST:
	case ERROR_DEVICE_NOT_CONNECTED:
	case ERROR_BAD_UNIT: return ENODEV;
	case ERROR_MORE_DATA: return EOVERFLOW;
	default: return EIO;
	}
}
