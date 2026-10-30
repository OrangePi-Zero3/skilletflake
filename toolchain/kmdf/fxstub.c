// SPDX-License-Identifier: MIT
/*
 * fxstub.c - clean-room KMDF client stub, replacing the WDK's WdfDriverEntry.lib.
 *
 * The image entry point is FxDriverEntry (link with /entry:FxDriverEntry). It binds the driver
 * to the in-box KMDF runtime through WDFLDR.SYS!WdfVersionBind, which fills in
 *   - WdfFunctions (named WdfFunctions_01015 by wdf.h): pointer to the framework's DDI table,
 *     the target of every WdfXxx() call in the KMDF headers;
 *   - WdfDriverGlobals: the per-driver globals passed as the first argument of every DDI;
 * then calls the driver's DriverEntry, and on unload calls the driver's (framework-installed)
 * unload routine followed by WdfVersionUnbind.
 *
 * The WDF_BIND_INFO layout is the v1 layout from Microsoft's MIT-licensed fxldr.h
 * (Windows-Driver-Frameworks src/framework/shared/inc/private/common/fxldr.h:79). For a client
 * built against KMDF >= 1.15 the loader stores a pointer to its table into *FuncTable
 * (fxlibrarycommon.cpp: "FuncTable arrives with a ptr to &WdfFunctions"), and FuncCount must
 * be exactly one of the released table sizes (444 for 1.15). Bind info v2 (KMDF >= 1.25 headers)
 * is not implemented; we target KMDF 1.15, which every Windows 10/11 release ships or exceeds.
 *
 * Not copied from ReactOS (GPL) or the WDK. UNTESTED ON HARDWARE.
 */
#include <ntddk.h>
#include <wdf.h>

#ifndef KMDF_VERSION_MAJOR
#define KMDF_VERSION_MAJOR 1
#endif
#ifndef KMDF_VERSION_MINOR
#define KMDF_VERSION_MINOR 15
#endif

#define FXSTUB_TAG 'bxFP'

typedef VOID (*FXSTUB_WDFFUNC)(VOID);

/* fxldr.h: WDF_VERSION + WDF_BIND_INFO (v1). */
typedef struct {
	ULONG Major;
	ULONG Minor;
	ULONG Build;
} FXSTUB_VERSION;

typedef struct {
	ULONG Size;
	PWCHAR Component;
	FXSTUB_VERSION Version;
	ULONG FuncCount;
	FXSTUB_WDFFUNC *FuncTable;
	PVOID Module; /* written by the loader */
} FXSTUB_BIND_INFO;

/* WDFLDR.SYS exports (fxldr.h). ComponentGlobals is the PWDF_DRIVER_GLOBALS for KMDF. */
NTSTATUS NTAPI WdfVersionBind(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath,
			      FXSTUB_BIND_INFO *BindInfo, PVOID *ComponentGlobals);
NTSTATUS NTAPI WdfVersionUnbind(PUNICODE_STRING RegistryPath, FXSTUB_BIND_INFO *BindInfo,
				PVOID ComponentGlobals);

/* Provided by the driver. */
NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
NTSTATUS NTAPI FxDriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);

/* Referenced by the KMDF headers (wdf.h renames WdfFunctions to WdfFunctions_010xx). */
const WDFFUNC *WdfFunctions;
PWDF_DRIVER_GLOBALS WdfDriverGlobals;

/* Must be writable: the loader fills Module and *FuncTable. */
static FXSTUB_BIND_INFO fxstub_bind = {
	.Size = sizeof(FXSTUB_BIND_INFO),
	.Component = L"KmdfLibrary",
	.Version = { KMDF_VERSION_MAJOR, KMDF_VERSION_MINOR, 0 },
	.FuncCount = WdfFunctionTableNumEntries,
	.FuncTable = (FXSTUB_WDFFUNC *)&WdfFunctions,
	.Module = NULL,
};

/* Private copy of the registry path: the caller's buffer is not guaranteed to outlive
 * DriverEntry, but WdfVersionUnbind needs it at unload. */
static UNICODE_STRING fxstub_regpath;
static PDRIVER_UNLOAD fxstub_client_unload;

static void fxstub_free_regpath(void)
{
	if (fxstub_regpath.Buffer) {
		ExFreePoolWithTag(fxstub_regpath.Buffer, FXSTUB_TAG);
		fxstub_regpath.Buffer = NULL;
		fxstub_regpath.Length = fxstub_regpath.MaximumLength = 0;
	}
}

static void fxstub_unbind(void)
{
	if (WdfDriverGlobals) {
		WdfVersionUnbind(&fxstub_regpath, &fxstub_bind, WdfDriverGlobals);
		WdfDriverGlobals = NULL;
	}
	fxstub_free_regpath();
}

static VOID NTAPI fxstub_driver_unload(PDRIVER_OBJECT DriverObject)
{
	/* The framework's own unload (which calls EvtDriverUnload) must run before unbinding. */
	if (fxstub_client_unload)
		fxstub_client_unload(DriverObject);
	fxstub_unbind();
}

NTSTATUS NTAPI FxDriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
	NTSTATUS status;
	USHORT max;

	if (!DriverObject || !RegistryPath)
		return STATUS_INVALID_PARAMETER;

	if (RegistryPath->Length > 0xFFFF - sizeof(WCHAR))
		return STATUS_NAME_TOO_LONG;
	max = (USHORT)(RegistryPath->Length + sizeof(WCHAR));
	fxstub_regpath.Buffer = ExAllocatePoolWithTag(NonPagedPoolNx, max, FXSTUB_TAG);
	if (!fxstub_regpath.Buffer)
		return STATUS_INSUFFICIENT_RESOURCES;
	fxstub_regpath.Length = 0;
	fxstub_regpath.MaximumLength = max;
	RtlCopyUnicodeString(&fxstub_regpath, RegistryPath);

	status = WdfVersionBind(DriverObject, &fxstub_regpath, &fxstub_bind,
				(PVOID *)&WdfDriverGlobals);
	if (!NT_SUCCESS(status)) {
		WdfDriverGlobals = NULL;
		fxstub_free_regpath();
		return status;
	}

	status = DriverEntry(DriverObject, RegistryPath);
	if (!NT_SUCCESS(status)) {
		fxstub_unbind();
		return status;
	}

	/*
	 * WdfDriverCreate installs the framework's dispatch table and unload routine and tells us
	 * through DisplaceDriverUnload whether the stub must interpose on unload (it is FALSE only
	 * for NoDispatchOverride/non-PnP drivers without EvtDriverUnload, see fxdriverapi.cpp).
	 */
	if (WdfDriverGlobals->DisplaceDriverUnload) {
		fxstub_client_unload = DriverObject->DriverUnload;
		DriverObject->DriverUnload = fxstub_driver_unload;
	}
	return STATUS_SUCCESS;
}
