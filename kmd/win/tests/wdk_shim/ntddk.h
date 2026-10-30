/* SPDX-License-Identifier: MIT */
/*
 * Host-test stand-in for the WDK's ntddk.h (kmd/win/tests only, never used by panwin.sys).
 *
 * Just enough of the NT kernel API for pw_os_win.c, pw_win_ioctl.c and pw_win_user.c to compile
 * and run on Linux, implemented on pthreads in fake_wdk.c. Semantics follow the public Microsoft
 * documentation of each routine as far as the glue relies on it; nothing here is derived from
 * Windows code. Dispatcher objects (KEVENT) share one global lock + condition variable, like the
 * NT dispatcher lock, which makes WaitAny over several objects trivial.
 */
#ifndef FAKE_NTDDK_H
#define FAKE_NTDDK_H

/* Every system header the glue (and drm.h on Linux) needs, before wdf.h neuters __asm__. */
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <linux/types.h>
#include <asm/ioctl.h>
#include <sys/types.h>

#define NTAPI
#define NTKERNELAPI
#define FASTCALL

typedef void VOID;
typedef void *PVOID;
typedef void *HANDLE;
typedef unsigned char UCHAR, BOOLEAN, *PBOOLEAN;
typedef char CHAR;
typedef unsigned short USHORT;
typedef wchar_t WCHAR;
typedef int LONG;
typedef unsigned int ULONG, *PULONG, DWORD;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG, ULONG64;
typedef uintptr_t ULONG_PTR, *PULONG_PTR;
typedef size_t SIZE_T;
typedef int NTSTATUS;
typedef UCHAR KIRQL;
typedef CHAR KPROCESSOR_MODE;
typedef ULONG_PTR PFN_NUMBER, *PPFN_NUMBER;

#define TRUE 1
#define FALSE 0

typedef union _LARGE_INTEGER {
	struct {
		ULONG LowPart;
		LONG HighPart;
	};
	LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER, PHYSICAL_ADDRESS;

#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define STATUS_SUCCESS                 ((NTSTATUS)0x00000000)
#define STATUS_WAIT_0                  ((NTSTATUS)0x00000000)
#define STATUS_WAIT_1                  ((NTSTATUS)0x00000001)
#define STATUS_USER_APC                ((NTSTATUS)0x000000C0)
#define STATUS_ALERTED                 ((NTSTATUS)0x00000101)
#define STATUS_TIMEOUT                 ((NTSTATUS)0x00000102)
#define STATUS_PENDING                 ((NTSTATUS)0x00000103)
#define STATUS_ACCESS_VIOLATION        ((NTSTATUS)0xC0000005)
#define STATUS_INVALID_HANDLE          ((NTSTATUS)0xC0000008)
#define STATUS_INVALID_PARAMETER       ((NTSTATUS)0xC000000D)
#define STATUS_INVALID_DEVICE_REQUEST  ((NTSTATUS)0xC0000010)
#define STATUS_ACCESS_DENIED           ((NTSTATUS)0xC0000022)
#define STATUS_BUFFER_TOO_SMALL        ((NTSTATUS)0xC0000023)
#define STATUS_OBJECT_TYPE_MISMATCH    ((NTSTATUS)0xC0000024)
#define STATUS_OBJECT_NAME_NOT_FOUND   ((NTSTATUS)0xC0000034)
#define STATUS_OBJECT_PATH_NOT_FOUND   ((NTSTATUS)0xC000003A)
#define STATUS_INSUFFICIENT_RESOURCES  ((NTSTATUS)0xC000009A)

#define PAGE_SIZE 4096u
#define PAGE_SHIFT 12

/* ---- lists ---- */
typedef struct _LIST_ENTRY {
	struct _LIST_ENTRY *Flink, *Blink;
} LIST_ENTRY, *PLIST_ENTRY;

#define CONTAINING_RECORD(addr, type, field) ((type *)((char *)(addr) - offsetof(type, field)))

static inline void InitializeListHead(PLIST_ENTRY h)
{
	h->Flink = h->Blink = h;
}

static inline BOOLEAN IsListEmpty(const LIST_ENTRY *h)
{
	return h->Flink == h;
}

static inline BOOLEAN RemoveEntryList(PLIST_ENTRY e)
{
	PLIST_ENTRY f = e->Flink, b = e->Blink;

	b->Flink = f;
	f->Blink = b;
	return f == b;
}

static inline PLIST_ENTRY RemoveHeadList(PLIST_ENTRY h)
{
	PLIST_ENTRY e = h->Flink;

	RemoveEntryList(e);
	return e;
}

static inline void InsertTailList(PLIST_ENTRY h, PLIST_ENTRY e)
{
	PLIST_ENTRY b = h->Blink;

	e->Flink = h;
	e->Blink = b;
	b->Flink = e;
	h->Blink = e;
}

/* ---- objects with a reference count (ObReference/ObDereference) ---- */
struct fake_obhdr {
	uint32_t magic;
	int refs;	/* __atomic */
};
#define FAKE_OB_EVENT   0x45564e54u
#define FAKE_OB_PROCESS 0x50524f43u

typedef struct fake_process {
	struct fake_obhdr hdr;
	const char *name;
} *PEPROCESS;

typedef struct fake_object_type *POBJECT_TYPE;
extern POBJECT_TYPE *ExEventObjectType;

/* ---- IRQL / modes / misc constants ---- */
#define PASSIVE_LEVEL 0
#define APC_LEVEL 1
#define DISPATCH_LEVEL 2
#define KernelMode 0
#define UserMode 1
#define IO_NO_INCREMENT 0
#define EVENT_MODIFY_STATE 0x0002
#define SYNCHRONIZE 0x00100000L
#define GENERIC_READ 0x80000000L
#define THREAD_WAIT_OBJECTS 3
#define EXCEPTION_EXECUTE_HANDLER 1

typedef enum _EVENT_TYPE { NotificationEvent, SynchronizationEvent } EVENT_TYPE;
typedef enum _TIMER_TYPE { NotificationTimer, SynchronizationTimer } TIMER_TYPE;
typedef enum _KWAIT_REASON { Executive = 0, UserRequest = 6 } KWAIT_REASON;
typedef enum _WAIT_TYPE { WaitAll, WaitAny } WAIT_TYPE;
typedef enum _WORK_QUEUE_TYPE { CriticalWorkQueue, DelayedWorkQueue } WORK_QUEUE_TYPE;
typedef enum _MEMORY_CACHING_TYPE { MmNonCached = 0, MmCached = 1, MmWriteCombined = 2 }
	MEMORY_CACHING_TYPE;
typedef enum _POOL_TYPE { NonPagedPool = 0, NonPagedPoolNx = 512 } POOL_TYPE;
#define NormalPagePriority 16
#define MM_ALLOCATE_FULLY_REQUIRED 0x4
#define PAGE_READWRITE 0x04
#define PAGE_NOCACHE 0x200
#define PAGE_WRITECOMBINE 0x400

#define DPFLTR_IHVDRIVER_ID 77
#define DPFLTR_ERROR_LEVEL 0
#define DPFLTR_WARNING_LEVEL 1
#define DPFLTR_TRACE_LEVEL 2
#define DPFLTR_INFO_LEVEL 3
ULONG DbgPrintEx(ULONG component, ULONG level, const char *fmt, ...);

/* ---- dispatcher objects ---- */
typedef struct _KEVENT {
	struct fake_obhdr hdr;	/* only used for "user" events handed out as handles */
	EVENT_TYPE type;
	int signalled;		/* under the dispatcher lock */
} KEVENT, *PKEVENT, *PRKEVENT;

void KeInitializeEvent(PRKEVENT e, EVENT_TYPE type, BOOLEAN state);
LONG KeSetEvent(PRKEVENT e, LONG incr, BOOLEAN wait);
void KeClearEvent(PRKEVENT e);
NTSTATUS KeWaitForSingleObject(PVOID obj, KWAIT_REASON reason, KPROCESSOR_MODE mode,
			       BOOLEAN alertable, PLARGE_INTEGER timeout);
NTSTATUS KeWaitForMultipleObjects(ULONG count, PVOID objs[], WAIT_TYPE type, KWAIT_REASON reason,
				  KPROCESSOR_MODE mode, BOOLEAN alertable, PLARGE_INTEGER timeout,
				  PVOID wait_blocks);
NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE mode, BOOLEAN alertable, PLARGE_INTEGER interval);
void KeStallExecutionProcessor(ULONG us);
LARGE_INTEGER KeQueryPerformanceCounter(PLARGE_INTEGER freq);

/* ---- spin locks / fast mutexes / critical regions / rundown ---- */
typedef pthread_mutex_t KSPIN_LOCK, *PKSPIN_LOCK;
void KeInitializeSpinLock(PKSPIN_LOCK l);
void KeAcquireSpinLock(PKSPIN_LOCK l, KIRQL *old);
void KeReleaseSpinLock(PKSPIN_LOCK l, KIRQL old);
void KeAcquireSpinLockAtDpcLevel(PKSPIN_LOCK l);
void KeReleaseSpinLockFromDpcLevel(PKSPIN_LOCK l);

typedef struct _FAST_MUTEX {
	pthread_mutex_t m;
} FAST_MUTEX, *PFAST_MUTEX;
void ExInitializeFastMutex(PFAST_MUTEX m);
void ExAcquireFastMutexUnsafe(PFAST_MUTEX m);
void ExReleaseFastMutexUnsafe(PFAST_MUTEX m);
void KeEnterCriticalRegion(void);
void KeLeaveCriticalRegion(void);

typedef struct _EX_RUNDOWN_REF {
	pthread_mutex_t m;
	pthread_cond_t c;
	long count;
	int active;	/* run-down started: acquires fail */
} EX_RUNDOWN_REF, *PEX_RUNDOWN_REF;
void ExInitializeRundownProtection(PEX_RUNDOWN_REF r);
void ExReInitializeRundownProtection(PEX_RUNDOWN_REF r);
BOOLEAN ExAcquireRundownProtection(PEX_RUNDOWN_REF r);
void ExReleaseRundownProtection(PEX_RUNDOWN_REF r);
void ExWaitForRundownProtectionRelease(PEX_RUNDOWN_REF r);

/* ---- timers and DPCs ---- */
typedef struct _KDPC *PKDPC, *PRKDPC;
typedef VOID KDEFERRED_ROUTINE(PKDPC dpc, PVOID ctx, PVOID a1, PVOID a2);
typedef KDEFERRED_ROUTINE *PKDEFERRED_ROUTINE;
typedef struct _KDPC {
	PKDEFERRED_ROUTINE routine;
	PVOID ctx;
} KDPC;
typedef struct _KTIMER {
	struct _KTIMER *next;	/* armed list */
	int armed;
	uint64_t due_ns;
	PKDPC dpc;
} KTIMER, *PKTIMER;
void KeInitializeTimerEx(PKTIMER t, TIMER_TYPE type);
BOOLEAN KeSetTimer(PKTIMER t, LARGE_INTEGER due, PKDPC dpc);
BOOLEAN KeCancelTimer(PKTIMER t);
void KeInitializeDpc(PRKDPC dpc, PKDEFERRED_ROUTINE routine, PVOID ctx);
void KeFlushQueuedDpcs(void);

/* ---- work items ---- */
typedef struct fake_device_object *PDEVICE_OBJECT;
typedef VOID IO_WORKITEM_ROUTINE(PDEVICE_OBJECT dev, PVOID ctx);
typedef IO_WORKITEM_ROUTINE *PIO_WORKITEM_ROUTINE;
typedef struct _IO_WORKITEM *PIO_WORKITEM;
PIO_WORKITEM IoAllocateWorkItem(PDEVICE_OBJECT dev);
void IoQueueWorkItem(PIO_WORKITEM item, PIO_WORKITEM_ROUTINE routine, WORK_QUEUE_TYPE q,
		     PVOID ctx);
void IoFreeWorkItem(PIO_WORKITEM item);

/* ---- processes, threads, objects ---- */
PEPROCESS IoGetCurrentProcess(void);
HANDLE PsGetCurrentThreadId(void);
KPROCESSOR_MODE ExGetPreviousMode(void);
#define ObReferenceObject(o) ObfReferenceObject(o)
#define ObDereferenceObject(o) ObfDereferenceObject(o)
void ObfReferenceObject(PVOID o);
void ObfDereferenceObject(PVOID o);
NTSTATUS ObReferenceObjectByHandle(HANDLE h, ULONG access, POBJECT_TYPE type,
				   KPROCESSOR_MODE mode, PVOID *obj, PVOID info);

/* ---- pool ---- */
PVOID ExAllocatePoolWithTag(POOL_TYPE type, SIZE_T size, ULONG tag);
void ExFreePoolWithTag(PVOID p, ULONG tag);
void ExFreePool(PVOID p);
#define RtlZeroMemory(p, n) memset((p), 0, (n))
#define RtlCopyMemory(d, s, n) memcpy((d), (s), (n))

/* ---- user memory: SEH emulated with setjmp/longjmp (one level, per thread) ----
 * __try { body } __except (f) { handler }  ->  if (setjmp() == 0) { body } else { handler }
 * ProbeForRead/Write longjmp to the innermost __try for addresses in the fake "bad" range. */
extern __thread jmp_buf fake_seh_jb;
#define __try if (setjmp(fake_seh_jb) == 0)
#define __except(filter) else
void ProbeForRead(const volatile void *addr, SIZE_T len, ULONG align);
void ProbeForWrite(volatile void *addr, SIZE_T len, ULONG align);
#define FAKE_BAD_USER_PTR 0xdead0000u	/* [this, +64 KiB) faults in ProbeFor* */

/* ---- MDLs and mappings ---- */
typedef struct _MDL {
	PVOID start_va;		/* MmGetMdlVirtualAddress */
	ULONG byte_count;
	char *base;		/* host pointer of the first described byte */
	int pages_owned;	/* MmAllocatePagesForMdlEx: base is ours to free */
	PFN_NUMBER pfns[];	/* only for MmAllocatePagesForMdlEx MDLs */
} MDL, *PMDL;
#define MmGetMdlVirtualAddress(m) ((m)->start_va)
#define MmGetMdlByteCount(m) ((m)->byte_count)
#define MmGetMdlPfnArray(m) ((m)->pfns)
PMDL IoAllocateMdl(PVOID va, ULONG len, BOOLEAN secondary, BOOLEAN charge, PVOID irp);
void IoFreeMdl(PMDL m);
void IoBuildPartialMdl(PMDL src, PMDL dst, PVOID va, ULONG len);
void MmBuildMdlForNonPagedPool(PMDL m);
PMDL MmAllocatePagesForMdlEx(PHYSICAL_ADDRESS lo, PHYSICAL_ADDRESS hi, PHYSICAL_ADDRESS skip,
			     SIZE_T bytes, MEMORY_CACHING_TYPE cache, ULONG flags);
void MmFreePagesFromMdl(PMDL m);
PVOID MmMapLockedPagesSpecifyCache(PMDL m, KPROCESSOR_MODE mode, MEMORY_CACHING_TYPE cache,
				   PVOID req_va, ULONG bugcheck, ULONG prio);
void MmUnmapLockedPages(PVOID va, PMDL m);
void MmFreeContiguousMemory(PVOID p);
PHYSICAL_ADDRESS MmGetPhysicalAddress(PVOID va);

#define READ_REGISTER_ULONG(p) (*(p))
#define WRITE_REGISTER_ULONG(p, v) (*(p) = (v))

/* ---- files (firmware loader; always "not found" here) ---- */
typedef struct _UNICODE_STRING {
	USHORT Length, MaximumLength;
	WCHAR *Buffer;
} UNICODE_STRING, *PUNICODE_STRING;
typedef struct _OBJECT_ATTRIBUTES {
	PUNICODE_STRING name;
	ULONG attr;
} OBJECT_ATTRIBUTES;
#define OBJ_KERNEL_HANDLE 0x200
#define OBJ_CASE_INSENSITIVE 0x40
#define InitializeObjectAttributes(p, n, a, r, s) ((p)->name = (n), (p)->attr = (a))
typedef struct _IO_STATUS_BLOCK {
	NTSTATUS Status;
	ULONG_PTR Information;
} IO_STATUS_BLOCK;
typedef struct _FILE_STANDARD_INFORMATION {
	LARGE_INTEGER AllocationSize, EndOfFile;
	ULONG NumberOfLinks;
	BOOLEAN DeletePending, Directory;
} FILE_STANDARD_INFORMATION;
#define FileStandardInformation 5
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_SHARE_READ 1
#define FILE_OPEN 1
#define FILE_SYNCHRONOUS_IO_NONALERT 0x20
#define FILE_NON_DIRECTORY_FILE 0x40
NTSTATUS ZwCreateFile(HANDLE *h, ULONG access, OBJECT_ATTRIBUTES *oa, IO_STATUS_BLOCK *iosb,
		      PLARGE_INTEGER alloc, ULONG attrs, ULONG share, ULONG disp, ULONG opts,
		      PVOID ea, ULONG ealen);
NTSTATUS ZwQueryInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, PVOID info, ULONG len, int cls);
NTSTATUS ZwReadFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, IO_STATUS_BLOCK *iosb, PVOID buf,
		    ULONG len, PLARGE_INTEGER pos, PULONG key);
NTSTATUS ZwClose(HANDLE h);

#endif /* FAKE_NTDDK_H */
