/* SPDX-License-Identifier: MIT */
/*
 * panwin_transport.h - pluggable kernel transports of libdrm-win (libdrm.dll).
 *
 * A transport is what libdrm-win sends its METHOD_BUFFERED ioctls to. Built in:
 *   "real"  panwin.sys: SetupAPI enumeration + CreateFileW(FILE_FLAG_OVERLAPPED) +
 *           NtDeviceIoControlFile
 *   "fake"  in-process fake kernel (only in builds configured with -Dfake_kernel=true)
 * More can be registered at run time (panwin_register_transport), e.g. a test transport that
 * runs the real kmd/core in-process, or loaded from a DLL with
 *   PANWIN_TRANSPORT=dll:<path to dll>
 * whose export PANWIN_TRANSPORT_ENTRY ("panwin_transport_entry", a panwin_transport_entry_fn)
 * returns the table. PANWIN_TRANSPORT=<name> selects a built-in or registered transport by name
 * at first use (PANWIN_FAKE_KERNEL=1 is the legacy spelling of PANWIN_TRANSPORT=fake).
 *
 * The transport must reproduce panwin.sys's ABI (include/panwin/panwin_ioctl.h), including
 * completing failed DRM ioctls with PANWIN_STATUS_FROM_ERRNO(linux_errno): libdrm-win decodes
 * the returned NTSTATUS exactly as it does for the real driver.
 */
#ifndef PANWIN_TRANSPORT_H
#define PANWIN_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PANWIN_TRANSPORT_VERSION 1u

struct panwin_transport {
	uint32_t version;       /* PANWIN_TRANSPORT_VERSION */
	const char *name;       /* unique, e.g. "real", "fake", "core" */
	/* Number of devices present (<= 16), or a negative CRT errno. Re-enumerates. */
	int (*enumerate)(void);
	/* Human-readable kernel-side path of device `index` (UTF-8). 0 or negative CRT errno. */
	int (*device_path)(int index, char *buf, size_t len);
	/* Open device `index` (one kernel file, like open() of a DRM node). 0 or -errno. */
	int (*open)(int index, void **file);
	void (*close)(void *file);
	/*
	 * One DeviceIoControl: `buf` holds in_size input bytes and receives up to out_size output
	 * bytes (METHOD_BUFFERED, in and out share the buffer). Returns the NTSTATUS the request
	 * completed with (0 = STATUS_SUCCESS) and the number of bytes written back in *returned.
	 * Must be callable concurrently from several threads on the same file (a syncobj WAIT on
	 * one thread must not block a SIGNAL on another).
	 */
	int32_t (*ioctl)(void *file, uint32_t code, void *buf, uint32_t in_size, uint32_t out_size,
			 uint32_t *returned);
	/* Value returned by panwin_fd_to_handle() (a HANDLE for "real"). */
	void *(*os_handle)(void *file);
};

#define PANWIN_TRANSPORT_ENTRY "panwin_transport_entry"
typedef const struct panwin_transport *(*panwin_transport_entry_fn)(void);

/* Add a transport (the table must stay valid until the process exits). 0, or -1 with errno =
 * EINVAL (bad table/version), EEXIST (name taken) or ENOSPC (registry full). */
int panwin_register_transport(const struct panwin_transport *tp);

/* Route every device to transport `name` ("real", "fake", a registered name, or
 * "dll:<path>"). 0, or -1 with errno = ENOENT (unknown name / DLL without the entry point) or
 * EBUSY (an fd or mapping is open). */
int panwin_select_transport(const char *name);

/* Name of the transport in use ("real", "fake", ...). */
const char *panwin_transport_name(void);

#ifdef __cplusplus
}
#endif

#endif /* PANWIN_TRANSPORT_H */
