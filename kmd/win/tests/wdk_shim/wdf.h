/* SPDX-License-Identifier: MIT */
/*
 * Host-test stand-in for KMDF's wdf.h (kmd/win/tests only). Every WDF handle is a
 * struct fake_wdf_object whose `context` points at the typed context the driver declared;
 * requests carry a METHOD_BUFFERED system buffer and record how they were completed.
 */
#ifndef FAKE_WDF_H
#define FAKE_WDF_H

#include <ntddk.h>

struct fake_wdf_object {
	void *context;
	/* requests */
	struct fake_wdf_object *file;
	ULONG code;
	SIZE_T in_len, out_len;
	void *sysbuf;		/* max(in_len, out_len) bytes */
	int completed;
	NTSTATUS status;
	ULONG_PTR info;
	int enqueued;
};

typedef struct fake_wdf_object *WDFOBJECT, *WDFDEVICE, *WDFREQUEST, *WDFFILEOBJECT, *WDFQUEUE,
	*WDFINTERRUPT;

#define WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(type, name)                         \
	static inline type *name(struct fake_wdf_object *o)                    \
	{                                                                      \
		return (type *)o->context;                                     \
	}

typedef enum { WdfRequestTypeCreate = 0, WdfRequestTypeDeviceControl = 14 } WDF_REQUEST_TYPE;
typedef struct _WDF_REQUEST_PARAMETERS {
	ULONG Size;
	WDF_REQUEST_TYPE Type;
	union {
		struct {
			SIZE_T OutputBufferLength;
			SIZE_T InputBufferLength;
			ULONG IoControlCode;
		} DeviceIoControl;
	} Parameters;
} WDF_REQUEST_PARAMETERS;
#define WDF_REQUEST_PARAMETERS_INIT(p) (memset((p), 0, sizeof(*(p))), (p)->Size = sizeof(*(p)))
void WdfRequestGetParameters(WDFREQUEST r, WDF_REQUEST_PARAMETERS *p);
WDFFILEOBJECT WdfRequestGetFileObject(WDFREQUEST r);
NTSTATUS WdfRequestRetrieveInputBuffer(WDFREQUEST r, SIZE_T min, PVOID *buf, SIZE_T *len);
NTSTATUS WdfRequestRetrieveOutputBuffer(WDFREQUEST r, SIZE_T min, PVOID *buf, SIZE_T *len);
void WdfRequestComplete(WDFREQUEST r, NTSTATUS st);
void WdfRequestCompleteWithInformation(WDFREQUEST r, NTSTATUS st, ULONG_PTR info);
KPROCESSOR_MODE WdfRequestGetRequestorMode(WDFREQUEST r);
BOOLEAN WdfRequestIsFrom32BitProcess(WDFREQUEST r);
NTSTATUS WdfDeviceEnqueueRequest(WDFDEVICE d, WDFREQUEST r);
PDEVICE_OBJECT WdfDeviceWdmGetDeviceObject(WDFDEVICE d);

typedef enum { WdfFalse = 0, WdfTrue = 1, WdfUseDefault = 2 } WDF_TRI_STATE;
typedef enum { WdfIoQueueDispatchSequential = 1, WdfIoQueueDispatchParallel = 2 }
	WDF_IO_QUEUE_DISPATCH_TYPE;
typedef VOID EVT_WDF_IO_QUEUE_IO_DEFAULT(WDFQUEUE q, WDFREQUEST r);
typedef struct _WDF_IO_QUEUE_CONFIG {
	WDF_IO_QUEUE_DISPATCH_TYPE DispatchType;
	WDF_TRI_STATE PowerManaged;
	BOOLEAN DefaultQueue;
	EVT_WDF_IO_QUEUE_IO_DEFAULT *EvtIoDefault;
} WDF_IO_QUEUE_CONFIG;
#define WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(c, t)                                    \
	(memset((c), 0, sizeof(*(c))), (c)->DispatchType = (t), (c)->DefaultQueue = TRUE, \
	 (c)->PowerManaged = WdfUseDefault)
#define WDF_NO_OBJECT_ATTRIBUTES NULL
#define WDF_NO_HANDLE NULL
NTSTATUS WdfIoQueueCreate(WDFDEVICE d, WDF_IO_QUEUE_CONFIG *c, PVOID attr, WDFQUEUE *q);

typedef VOID EVT_WDF_IO_IN_CALLER_CONTEXT(WDFDEVICE d, WDFREQUEST r);
typedef VOID EVT_WDF_DEVICE_FILE_CREATE(WDFDEVICE d, WDFREQUEST r, WDFFILEOBJECT f);
typedef VOID EVT_WDF_FILE_CLEANUP(WDFFILEOBJECT f);
typedef VOID EVT_WDF_FILE_CLOSE(WDFFILEOBJECT f);

/*
 * Last of the shim headers: neuter the AArch64 inline assembly in pw_os_win.c (cache
 * maintenance, CTR_EL0) so it builds on any host. `__asm__ __volatile__(...)` -> `(void)0`.
 * System headers that use __asm__ for symbol renaming were all included by ntddk.h above.
 */
#define __asm__
#define __volatile__(...) ((void)0)

#endif /* FAKE_WDF_H */
