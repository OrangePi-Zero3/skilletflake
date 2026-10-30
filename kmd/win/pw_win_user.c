// SPDX-License-Identifier: MIT
/*
 * pw_win_user.c - copy_to_user / copy_from_user for the calling process, under SEH.
 *
 * Must run in the context of the process that owns the user VA (EvtIoInCallerContext or a
 * direct dispatch), at PASSIVE_LEVEL/APC_LEVEL. For a KernelMode requestor the pointers are
 * trusted and no probing is done (same rule as the I/O manager).
 *
 * Why SEH (and nothing else): a user VA can be unmapped or re-protected by another thread of
 * the process at any moment, so the only correct way to touch it from kernel mode is to
 * probe it (ProbeForRead/Write: range below MmUserProbeAddress, raises on violation) and copy
 * under an exception handler, which is what the I/O manager and every NT system service do.
 * The alternatives do not work: MmIsAddressValid is racy (and a page-in check, not a
 * protection check), MmCopyMemory(MM_COPY_MEMORY_VIRTUAL) only reads and does not probe, and
 * an MDL (IoAllocateMdl + MmProbeAndLockPages) raises on a bad buffer too, so it needs the
 * same __try; it would only add a page-lock round trip for these small copies.
 *
 * clang/llvm-mingw note: aarch64-w64-mingw32 clang implements __try/__except as table-based
 * SEH (.pdata/.xdata, handler __C_specific_handler from ntoskrnl, scope table of call-site
 * ranges). clang catches faults only in *calls* made from the __try body (no
 * -fasync-exceptions on aarch64), so the copy is an out-of-line call to ntoskrnl's memcpy (we
 * build with -ffreestanding -fno-builtin): do not replace it by open-coded loads/stores
 * inside the __try. The generated scope table is checked by docs/status/glue.md "Verified".
 * UNTESTED ON HARDWARE.
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

/*
 * pw_os.h user copies. They run in the requesting thread (pw_file_ioctl is only called from
 * EvtIoInCallerContext), so ExGetPreviousMode() is the requestor's mode: UserMode for
 * DeviceIoControl callers (pointers probed), KernelMode for in-kernel callers (trusted, as the
 * I/O manager does). A copy from any other context would see KernelMode, which is why the core
 * contract restricts these to pw_file_ioctl().
 */
int pw_os_copy_from_user(struct pw_os *os, void *dst, uint64_t user_ptr, size_t size)
{
	(void)os;
	if (!size)
		return PW_OK;
	if (!user_ptr || user_ptr != (uint64_t)(ULONG_PTR)user_ptr)
		return PW_EFAULT;
	return NT_SUCCESS(pw_win_copy_from_user(ExGetPreviousMode(), dst,
						(const void *)(ULONG_PTR)user_ptr, size)) ?
	       PW_OK : PW_EFAULT;
}

int pw_os_copy_to_user(struct pw_os *os, uint64_t user_ptr, const void *src, size_t size)
{
	(void)os;
	if (!size)
		return PW_OK;
	if (!user_ptr || user_ptr != (uint64_t)(ULONG_PTR)user_ptr)
		return PW_EFAULT;
	return NT_SUCCESS(pw_win_copy_to_user(ExGetPreviousMode(), (void *)(ULONG_PTR)user_ptr,
					      src, size)) ? PW_OK : PW_EFAULT;
}

/* MmMapLockedPagesSpecifyCache(UserMode) raises an exception instead of returning NULL. Must
 * run in the target process (caller context), at <= APC_LEVEL. */
void *pw_win_map_user(PMDL mdl, MEMORY_CACHING_TYPE cache, ULONG priority)
{
	void *va = NULL;

	__try {
		va = MmMapLockedPagesSpecifyCache(mdl, UserMode, cache, NULL, FALSE, priority);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		va = NULL;
	}
	return va;
}
