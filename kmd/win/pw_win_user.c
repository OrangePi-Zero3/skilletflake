// SPDX-License-Identifier: MIT
/*
 * pw_win_user.c - copy_to_user / copy_from_user for the calling process, under SEH.
 *
 * Must run in the context of the process that owns the user VA (EvtIoInCallerContext or a
 * direct dispatch), at PASSIVE_LEVEL/APC_LEVEL. For a KernelMode requestor the pointers are
 * trusted and no probing is done (same rule as the I/O manager).
 *
 * clang note: the copy is an out-of-line call to ntoskrnl's memcpy (we build with
 * -ffreestanding -fno-builtin), so a fault happens inside a call made from the __try body,
 * which clang's SEH lowering catches on every target. Keep it that way: do not replace the
 * memcpy by open-coded loads/stores inside the __try. UNTESTED ON HARDWARE.
 */
#include "pw_win.h"

NTSTATUS pw_win_copy_to_user(KPROCESSOR_MODE mode, void *user_dst, const void *src, size_t len)
{
	NTSTATUS st = STATUS_SUCCESS;

	if (!len)
		return STATUS_SUCCESS;
	if (mode == KernelMode) {
		memcpy(user_dst, src, len);
		return STATUS_SUCCESS;
	}
	__try {
		ProbeForWrite(user_dst, len, 1);
		memcpy(user_dst, src, len);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		st = STATUS_ACCESS_VIOLATION;
	}
	return st;
}

NTSTATUS pw_win_copy_from_user(KPROCESSOR_MODE mode, void *dst, const void *user_src, size_t len)
{
	NTSTATUS st = STATUS_SUCCESS;

	if (!len)
		return STATUS_SUCCESS;
	if (mode == KernelMode) {
		memcpy(dst, user_src, len);
		return STATUS_SUCCESS;
	}
	__try {
		ProbeForRead((void *)user_src, len, 1);
		memcpy(dst, user_src, len);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		st = STATUS_ACCESS_VIOLATION;
	}
	return st;
}
