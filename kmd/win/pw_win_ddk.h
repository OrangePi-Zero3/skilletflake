/* SPDX-License-Identifier: MIT */
/*
 * pw_win_ddk.h - kernel declarations that the mingw-w64 DDK headers lack (or that we resolve at
 * run time). Prototypes are written from the public Microsoft documentation.
 *
 * Import policy (see docs/status/kmd-win.md): APIs that are in our approximate ARM64 .def
 * files are imported statically; Windows 10+ APIs that mingw-w64's def lists do not contain
 * (ExAllocatePool2, MmMapIoSpaceEx) are looked up with MmGetSystemRoutineAddress so that a
 * missing export degrades to a fallback instead of failing the driver load.
 */
#ifndef PW_WIN_DDK_H
#define PW_WIN_DDK_H

#include <ntddk.h>

/* ExAllocatePool2 (Windows 10 2004+). */
typedef ULONG64 POOL_FLAGS;
#ifndef POOL_FLAG_NON_PAGED
#define POOL_FLAG_UNINITIALIZED 0x0000000000000002ULL
#define POOL_FLAG_NON_PAGED     0x0000000000000040ULL
#define POOL_FLAG_PAGED         0x0000000000000100ULL
#endif
typedef PVOID (NTAPI *PFN_EX_ALLOCATE_POOL2)(POOL_FLAGS Flags, SIZE_T NumberOfBytes, ULONG Tag);

/* MmMapIoSpaceEx (Windows 10+): Protect is PAGE_* | PAGE_NOCACHE / PAGE_WRITECOMBINE. */
typedef PVOID (NTAPI *PFN_MM_MAP_IO_SPACE_EX)(PHYSICAL_ADDRESS PhysicalAddress,
					      SIZE_T NumberOfBytes, ULONG Protect);

/* MmAllocateContiguousNodeMemory (Windows 8+; present in the ARM32 export list). */
#ifndef MM_ANY_NODE_OK
#define MM_ANY_NODE_OK 0x80000000
#endif
typedef ULONG NODE_REQUIREMENT;
NTKERNELAPI PVOID NTAPI MmAllocateContiguousNodeMemory(SIZE_T NumberOfBytes,
						       PHYSICAL_ADDRESS LowestAcceptableAddress,
						       PHYSICAL_ADDRESS HighestAcceptableAddress,
						       PHYSICAL_ADDRESS BoundaryAddressMultiple,
						       ULONG Protect, NODE_REQUIREMENT PreferredNode);

/* MM_PAGE_PRIORITY modifiers (Windows 8+). */
#ifndef MdlMappingNoExecute
#define MdlMappingNoExecute 0x40000000
#endif

#endif /* PW_WIN_DDK_H */
