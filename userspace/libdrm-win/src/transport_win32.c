/* SPDX-License-Identifier: MIT */
/*
 * Real transport: panwin.sys via SetupAPI enumeration + CreateFileW + NtDeviceIoControlFile.
 *
 * NtDeviceIoControlFile (rather than DeviceIoControl) is used so the raw NTSTATUS reaches
 * panwin_ntstatus_to_errno(): kernel32 would squash it through RtlNtStatusToDosError and lose the
 * errno-carrying PANWIN_STATUS_FROM_ERRNO codes (and turn STATUS_TIMEOUT into success).
 *
 * The device is opened FILE_FLAG_OVERLAPPED. On a synchronous handle the I/O manager serializes
 * every request on the file object (it holds the file object lock until the IRP completes), so
 * a DRM_IOCTL_SYNCOBJ_WAIT blocking in one thread would block the SIGNAL/submit that should
 * wake it in another thread: a deadlock, since Mesa shares one DRM fd between threads.
 * panwin.sys completes every request in the caller's context before returning, so
 * STATUS_PENDING is not expected; each call still passes a per-thread event and waits on it if
 * the request does pend.
 *
 * Enumeration: every present interface of PANWIN_INTERFACE_GUID, in SetupAPI order. If there is
 * none, the fixed symbolic link \\.\PanWin0 is used when it exists. The environment variable
 * PANWIN_DEVICE=<win32 path> overrides both (single device).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <winternl.h>
#include <setupapi.h>

#include "panwin/panwin_ioctl.h"
#include "libdrm_win.h"
#include "pw_internal.h"

#ifndef STATUS_PENDING
#define STATUS_PENDING ((NTSTATUS)0x00000103L)
#endif

static const GUID panwin_guid = PANWIN_INTERFACE_GUID_INIT;

/* ---- per-thread completion event (auto-reset), closed by the FLS callback at thread exit ---- */

static INIT_ONCE fls_once = INIT_ONCE_STATIC_INIT;
static DWORD fls_index = FLS_OUT_OF_INDEXES;

static void WINAPI fls_event_free(void *p)
{
	if (p)
		CloseHandle((HANDLE)p);
}

static BOOL CALLBACK fls_init(INIT_ONCE *once, void *param, void **ctx)
{
	(void)once;
	(void)param;
	(void)ctx;
	fls_index = FlsAlloc(fls_event_free);
	return TRUE;
}

/* Returns the calling thread's event, or a new one the caller must close (*owned = true). */
static HANDLE thread_event(bool *owned)
{
	HANDLE ev;

	*owned = false;
	InitOnceExecuteOnce(&fls_once, fls_init, NULL, NULL);
	if (fls_index != FLS_OUT_OF_INDEXES) {
		ev = FlsGetValue(fls_index);
		if (ev)
			return ev;
		ev = CreateEventW(NULL, FALSE, FALSE, NULL);
		if (ev && FlsSetValue(fls_index, ev))
			return ev;
	} else {
		ev = CreateEventW(NULL, FALSE, FALSE, NULL);
	}
	*owned = ev != NULL;
	return ev;
}

static SRWLOCK dev_lock = SRWLOCK_INIT;
static WCHAR *dev_paths[PW_MAX_DEVICES];
static int dev_count;

static void free_paths_locked(void)
{
	for (int i = 0; i < dev_count; i++) {
		free(dev_paths[i]);
		dev_paths[i] = NULL;
	}
	dev_count = 0;
}

static WCHAR *wcsdup_safe(const WCHAR *s)
{
	size_t n = wcslen(s) + 1;
	WCHAR *d = malloc(n * sizeof(WCHAR));
	if (d)
		memcpy(d, s, n * sizeof(WCHAR));
	return d;
}

static int enumerate_locked(void)
{
	WCHAR env[512];
	DWORD n;

	free_paths_locked();

	n = GetEnvironmentVariableW(L"PANWIN_DEVICE", env, PW_ARRAY_SIZE(env));
	if (n > 0 && n < PW_ARRAY_SIZE(env)) {
		dev_paths[0] = wcsdup_safe(env);
		dev_count = dev_paths[0] ? 1 : 0;
		return dev_count ? dev_count : -ENOMEM;
	}

	HDEVINFO set = SetupDiGetClassDevsW(&panwin_guid, NULL, NULL,
					    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set != INVALID_HANDLE_VALUE) {
		SP_DEVICE_INTERFACE_DATA ifd = { .cbSize = sizeof(ifd) };

		for (DWORD i = 0; dev_count < PW_MAX_DEVICES &&
		     SetupDiEnumDeviceInterfaces(set, NULL, &panwin_guid, i, &ifd); i++) {
			DWORD need = 0;
			SetupDiGetDeviceInterfaceDetailW(set, &ifd, NULL, 0, &need, NULL);
			if (!need)
				continue;
			SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = malloc(need);
			if (!det)
				break;
			det->cbSize = sizeof(*det);
			if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, NULL, NULL)) {
				dev_paths[dev_count] = wcsdup_safe(det->DevicePath);
				if (dev_paths[dev_count])
					dev_count++;
			}
			free(det);
		}
		SetupDiDestroyDeviceInfoList(set);
	}

	if (dev_count == 0) {
		/* QueryDosDevice checks the symlink without opening (and thus creating a file on) it. */
		WCHAR target[512];
		if (QueryDosDeviceW(L"PanWin0", target, PW_ARRAY_SIZE(target))) {
			dev_paths[0] = wcsdup_safe(PANWIN_DOS_NAME_W);
			if (dev_paths[0])
				dev_count = 1;
		}
	}
	return dev_count;
}

static int w32_enumerate(void)
{
	AcquireSRWLockExclusive(&dev_lock);
	int r = enumerate_locked();
	ReleaseSRWLockExclusive(&dev_lock);
	return r;
}

static int w32_device_path(int index, char *buf, size_t len)
{
	int r = -ENOENT;

	AcquireSRWLockShared(&dev_lock);
	if (index >= 0 && index < dev_count && len > 0) {
		if (WideCharToMultiByte(CP_UTF8, 0, dev_paths[index], -1, buf, (int)len, NULL, NULL) > 0)
			r = 0;
		else
			r = -EOVERFLOW;
	}
	ReleaseSRWLockShared(&dev_lock);
	return r;
}

static int w32_open(int index, void **file)
{
	HANDLE h = INVALID_HANDLE_VALUE;
	DWORD err = ERROR_FILE_NOT_FOUND;

	AcquireSRWLockExclusive(&dev_lock);
	if (dev_count == 0)
		enumerate_locked();
	if (index >= 0 && index < dev_count) {
		h = CreateFileW(dev_paths[index], GENERIC_READ | GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
		if (h == INVALID_HANDLE_VALUE)
			err = GetLastError();
	}
	ReleaseSRWLockExclusive(&dev_lock);

	if (h == INVALID_HANDLE_VALUE)
		return -panwin_win32_error_to_errno(err);
	*file = h;
	return 0;
}

static void w32_close(void *file)
{
	CloseHandle((HANDLE)file);
}

static int32_t w32_ioctl(void *file, uint32_t code, void *buf, uint32_t in_size,
			 uint32_t out_size, uint32_t *returned)
{
	IO_STATUS_BLOCK iosb;
	NTSTATUS st;
	bool owned;
	HANDLE ev = thread_event(&owned);

	if (!ev)
		return (int32_t)0xC000009Au; /* STATUS_INSUFFICIENT_RESOURCES */
	memset(&iosb, 0, sizeof(iosb));
	st = NtDeviceIoControlFile((HANDLE)file, ev, NULL, NULL, &iosb, code,
				   in_size ? buf : NULL, in_size, out_size ? buf : NULL, out_size);
	if (st == STATUS_PENDING) {
		WaitForSingleObject(ev, INFINITE);
		st = iosb.Status;
	}
	if (owned)
		CloseHandle(ev);
	if (returned)
		*returned = st >= 0 ? (uint32_t)iosb.Information : 0;
	return (int32_t)st;
}

static void *w32_os_handle(void *file)
{
	return file;
}

const struct panwin_transport pw_transport_win32 = {
	.version = PANWIN_TRANSPORT_VERSION,
	.name = "real",
	.enumerate = w32_enumerate,
	.device_path = w32_device_path,
	.open = w32_open,
	.close = w32_close,
	.ioctl = w32_ioctl,
	.os_handle = w32_os_handle,
};
