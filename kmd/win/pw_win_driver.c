// SPDX-License-Identifier: MIT
/*
 * pw_win_driver.c - KMDF driver/device lifetime for panwin.sys: DriverEntry, EvtDeviceAdd,
 * hardware resources (1 MMIO window + 3 interrupts in ACPI _CRS order job/mmu/gpu), power
 * transitions and interrupt plumbing into the OS-neutral core.
 *
 * Two boards (pw_win_plat.c, from the ACPI hardware IDs read in EvtDeviceAdd): the RK3588
 * Mali-G610 (CSF, _HID RKCP7410) and the Allwinner H616/H618 Mali-G31 (job manager, _HID
 * AWGP0031, Orange Pi Zero3). Every SoC-specific step below runs only on its own board.
 *
 * Lifetime of the core device (struct pw_device):
 *   EvtDevicePrepareHardware          map MMIO, check resources
 *   EvtDeviceD0Entry                  power check (RK3588: PMU2 registers; H616: ACPI _PSC),
 *                                     mask the 3 interrupt sources
 *   (KMDF connects the interrupts)
 *   EvtDeviceD0EntryPostInterruptsEnabled  pw_device_alloc(), publish to the ISR, then
 *                                     pw_device_probe() - IRQs are live, so the core can wait
 *                                     for interrupt-driven events while it boots
 *   EvtDeviceD0ExitPreInterruptsDisabled   stop new users of the core, revoke every open file
 *                                     (user waits interrupted, user mappings and notifies torn
 *                                     down, pw_file_close) while IRQs still work
 *   (KMDF disconnects the interrupts, flushing queued ISR work items)
 *   EvtDeviceD0Exit                   pw_device_destroy()     - no ISR/work item can run
 *   EvtDeviceReleaseHardware          unmap MMIO
 * A core failure does not fail the device start: the device stays up so GET_INFO and the
 * debug register ioctl can report what went wrong during bring-up.
 * UNTESTED ON HARDWARE.
 */
#include "pw_win.h"
#include "pw_fmt.h"
#include "panwin/panwin_ioctl.h"

#include <acpiioct.h>

_Static_assert(sizeof(WCHAR) == sizeof(uint16_t), "hardware IDs are UTF-16");

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD pw_win_device_add;
static EVT_WDF_DRIVER_UNLOAD pw_win_driver_unload;
static EVT_WDF_DEVICE_PREPARE_HARDWARE pw_win_prepare_hardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE pw_win_release_hardware;
static EVT_WDF_DEVICE_D0_ENTRY pw_win_d0_entry;
static EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED pw_win_d0_entry_post_irq;
static EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED pw_win_d0_exit_pre_irq;
static EVT_WDF_DEVICE_D0_EXIT pw_win_d0_exit;
static EVT_WDF_INTERRUPT_ISR pw_win_isr;
static EVT_WDF_INTERRUPT_WORKITEM pw_win_irq_work;

struct pw_win_routines pw_win_rt;

static const GUID pw_interface_guid = PANWIN_INTERFACE_GUID_INIT;

/*
 * Interrupt blocks of the three lines (RAWSTAT +0, CLEAR +4, MASK +8, STAT +0xC). The glue only
 * touches MASK/STAT, and only when no core device exists: to quiesce the GPU before connecting
 * the interrupts, and to mask a line that fires while there is nobody to handle it (otherwise a
 * level-triggered line would storm and hang the machine).
 */
static const ULONG pw_irq_base[PW_IRQ_COUNT] = {
	[PW_IRQ_JOB] = 0x1000,
	[PW_IRQ_MMU] = 0x2000,
	[PW_IRQ_GPU] = 0x0020,
};
static const char *const pw_irq_name[PW_IRQ_COUNT] = { "job", "mmu", "gpu" };

/*
 * RK3588 PMU (PMU2 block at 0xFD8D8000): touching 0xFB000000 with PD_GPU off, or with the GPU's
 * bus interface (NIU) still in idle/isolation, raises an SError or hangs the bus. Before the
 * first GPU register access we apply the same test as the edk2 patch's GPON method
 * (firmware/edk2-rk3588, \_SB.GPU0): BISR_STATUS4 bit 1 set (domain on and memory repair done),
 * BUS_IDLE_ST0 bit 0 and BUS_IDLE_ACK0 bit 0 clear (NIU out of idle). PWR_GATE_ST0 is logged too.
 * The PMU is not one of our resources; it is mapped for these reads only, and only when the
 * device's hardware ID says RK3588 (pw_win_platform.rk3588_pmu): on the H616 that physical
 * address is DRAM or unmapped.
 */
#define RK3588_PMU2_BASE          0xFD8D8000ULL
#define RK3588_PMU_BUS_IDLE_ACK0  0x118
#define RK3588_PMU_BUS_IDLE_ST0   0x120
#define RK3588_PMU_PWR_GATE_ST0   0x180
#define RK3588_PMU_BISR_STATUS4   0x290
#define RK3588_GPU_BISR_BIT       (1u << 1)
#define RK3588_GPU_NIU_BIT        (1u << 0)

void pw_win_resolve_routines(void)
{
	UNICODE_STRING name;

	RtlInitUnicodeString(&name, L"ExAllocatePool2");
	pw_win_rt.ExAllocatePool2 = (PFN_EX_ALLOCATE_POOL2)MmGetSystemRoutineAddress(&name);
	RtlInitUnicodeString(&name, L"MmMapIoSpaceEx");
	pw_win_rt.MmMapIoSpaceEx = (PFN_MM_MAP_IO_SPACE_EX)MmGetSystemRoutineAddress(&name);
}

static PVOID pw_win_map_io(PHYSICAL_ADDRESS pa, SIZE_T len)
{
	if (pw_win_rt.MmMapIoSpaceEx)
		return pw_win_rt.MmMapIoSpaceEx(pa, len, PAGE_READWRITE | PAGE_NOCACHE);
	return MmMapIoSpace(pa, len, MmNonCached);
}

/* Architected timer frequency (CNTFRQ_EL0, readable at EL1; firmware programs it: 24 MHz on
 * RK3588 and on the H616, whose arch timer runs from the 24 MHz OSC). Passed to the core as
 * pw_device_params.timer_hz (FW timeouts on CSF); 0 lets the core use its default. */
static uint32_t pw_win_read_cntfrq(void)
{
	uint64_t f;

	__asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(f));
	PW_WLOG(PW_LOG_INFO, "CNTFRQ_EL0 = %lu Hz", (unsigned long)f);
	return f > 0xFFFFFFFFu ? 0 : (uint32_t)f;
}

/* ------------------------------------------------------------------- registry params */

static ULONG pw_reg_ulong(WDFKEY *keys, int nkeys, const WCHAR *name, ULONG def)
{
	UNICODE_STRING n;
	ULONG v;

	RtlInitUnicodeString(&n, name);
	for (int i = 0; i < nkeys; i++)
		if (keys[i] && NT_SUCCESS(WdfRegistryQueryULong(keys[i], &n, &v)))
			return v;
	return def;
}

/* Returns the length in WCHARs (0 if absent). */
static USHORT pw_reg_string(WDFKEY *keys, int nkeys, const WCHAR *name, WCHAR *buf, USHORT cap)
{
	UNICODE_STRING n, val;
	USHORT bytes;

	RtlInitUnicodeString(&n, name);
	for (int i = 0; i < nkeys; i++) {
		if (!keys[i])
			continue;
		val.Buffer = buf;
		val.Length = 0;
		val.MaximumLength = (USHORT)((cap - 1) * sizeof(WCHAR));
		if (NT_SUCCESS(WdfRegistryQueryUnicodeString(keys[i], &n, &bytes, &val))) {
			USHORT chars = val.Length / sizeof(WCHAR);

			while (chars && buf[chars - 1] == 0) /* REG_SZ may include the NUL */
				chars--;
			buf[chars] = 0;
			return chars;
		}
	}
	buf[0] = 0; /* a failed query (e.g. value too long) may have left partial data */
	return 0;
}

/* Board from DevicePropertyHardwareID (REG_MULTI_SZ), pw_win_plat.c. NULL if unknown. */
static const struct pw_win_platform *pw_win_query_platform(WDFDEVICE device)
{
	WCHAR ids[256];
	char first[64];
	const struct pw_win_platform *p;
	ULONG bytes = 0;
	NTSTATUS st;
	ULONG i;

	RtlZeroMemory(ids, sizeof(ids));
	/* Two WCHARs short so the list stays double-NUL terminated whatever is returned. */
	st = WdfDeviceQueryProperty(device, DevicePropertyHardwareID,
				    sizeof(ids) - 2 * sizeof(WCHAR), ids, &bytes);
	if (!NT_SUCCESS(st)) {
		PW_WLOG(PW_LOG_ERR, "cannot read the hardware IDs (0x%08x): board unknown",
			(unsigned)st);
		return NULL;
	}
	for (i = 0; i + 1 < sizeof(first) && ids[i]; i++)
		first[i] = ids[i] < 0x80 ? (char)ids[i] : '?';
	first[i] = 0;
	p = pw_win_platform_from_hwids((const uint16_t *)ids, bytes / sizeof(WCHAR));
	if (p)
		PW_WLOG(PW_LOG_INFO, "board: %s %s (%s, _HID %s, hardware ID %s)", p->soc, p->gpu,
			p->csf ? "CSF" : "job manager", p->hid, first);
	else
		PW_WLOG(PW_LOG_ERR, "board not recognised from hardware ID %s: no SoC power check "
			"can run, so GPU registers will not be touched", first);
	return p;
}

static void pw_win_read_params(PW_DEVICE_CTX *ctx)
{
	WDFKEY keys[2] = { NULL, NULL };
	WCHAR root[PW_PATH_MAX];
	USHORT n;

	/* "Device Parameters" (INF [*.HW] AddReg HKR) first, then the service Parameters key. */
	if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(ctx->device, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
						 WDF_NO_OBJECT_ATTRIBUTES, &keys[0])))
		keys[0] = NULL;
	if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ,
							   WDF_NO_OBJECT_ATTRIBUTES, &keys[1])))
		keys[1] = NULL;

	ctx->boot_firmware = pw_reg_ulong(keys, 2, PW_REG_BOOT_FIRMWARE, 0);
	ctx->pmu_check = pw_reg_ulong(keys, 2, PW_REG_PMU_CHECK, 1);
	ctx->psc_check = pw_reg_ulong(keys, 2, PW_REG_PSC_CHECK, 1);
	n = pw_reg_string(keys, 2, PW_REG_FIRMWARE_ROOT, root, PW_PATH_MAX);
	if (n)
		pw_os_win_set_firmware_root(&ctx->os, root, n);
	n = pw_reg_string(keys, 2, PW_REG_FIRMWARE_NAME, ctx->fw_name_w, PW_PATH_MAX);
	for (USHORT i = 0; i <= n; i++)
		ctx->fw_name[i] = ctx->fw_name_w[i] < 0x80 ? (char)ctx->fw_name_w[i] : '?';

	for (int i = 0; i < 2; i++)
		if (keys[i])
			WdfRegistryClose(keys[i]);

	PW_WLOG(PW_LOG_INFO, "params: BootFirmware=%lu Rk3588PmuCheck=%lu AcpiPscCheck=%lu "
		"FirmwareName=%s", (unsigned long)ctx->boot_firmware, (unsigned long)ctx->pmu_check,
		(unsigned long)ctx->psc_check, ctx->fw_name[0] ? ctx->fw_name : "(from GPU_ID)");
	/* Only the checks of this board apply (pw_win_d0_entry). */
	if (ctx->plat && !ctx->plat->rk3588_pmu && ctx->pmu_check)
		PW_WLOG(PW_LOG_DBG, "Rk3588PmuCheck ignored: not an RK3588");
	if (ctx->plat && !ctx->plat->acpi_psc && !ctx->psc_check)
		PW_WLOG(PW_LOG_DBG, "AcpiPscCheck ignored: %s has no _PSC check", ctx->plat->soc);
}

/* ------------------------------------------------------------------------ interrupts */

static BOOLEAN pw_win_isr(WDFINTERRUPT interrupt, ULONG message_id)
{
	PW_IRQ_CTX *ic = PwIrqCtx(interrupt);
	PW_DEVICE_CTX *ctx = ic->dev;
	struct pw_device *pw = __atomic_load_n(&ctx->pw, __ATOMIC_ACQUIRE);
	ULONG base = pw_irq_base[ic->line];

	(void)message_id;
	if (pw && pw_irq_top(pw, ic->line)) {
		WdfInterruptQueueWorkItemForIsr(interrupt);
		return TRUE;
	}
	/*
	 * Nobody claimed it: no core device yet, or the core declined (e.g. line suspended). If
	 * the line really is asserted, returning FALSE would make a level-triggered interrupt
	 * storm and hang the machine, so mask it here and claim it (counted for diagnostics).
	 */
	if (!ctx->os.regs_enabled || !pw_os_reg_read32(&ctx->os, base + 0xC))
		return FALSE;
	pw_os_reg_write32(&ctx->os, base + 0x8, 0);
	ctx->irq_spurious[ic->line]++;
	return TRUE;
}

static VOID pw_win_irq_work(WDFINTERRUPT interrupt, WDFOBJECT associated)
{
	PW_IRQ_CTX *ic = PwIrqCtx(interrupt);
	struct pw_device *pw = __atomic_load_n(&ic->dev->pw, __ATOMIC_ACQUIRE);

	(void)associated;
	if (pw)
		pw_irq_bottom(pw, ic->line);
}

static NTSTATUS pw_win_create_interrupts(PW_DEVICE_CTX *ctx)
{
	for (int i = 0; i < PW_IRQ_COUNT; i++) {
		WDF_INTERRUPT_CONFIG cfg;
		WDF_OBJECT_ATTRIBUTES attr;
		NTSTATUS st;

		/* KMDF hands the interrupt resources to the objects in creation order, so the
		 * creation order must be the ACPI _CRS order: job, mmu, gpu. */
		WDF_INTERRUPT_CONFIG_INIT(&cfg, pw_win_isr, NULL);
		cfg.EvtInterruptWorkItem = pw_win_irq_work;
		cfg.AutomaticSerialization = FALSE;
		WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, PW_IRQ_CTX);
		st = WdfInterruptCreate(ctx->device, &cfg, &attr, &ctx->irq[i]);
		if (!NT_SUCCESS(st)) {
			PW_WLOG(PW_LOG_ERR, "WdfInterruptCreate(%s) failed: 0x%08x", pw_irq_name[i],
				(unsigned)st);
			return st;
		}
		PwIrqCtx(ctx->irq[i])->dev = ctx;
		PwIrqCtx(ctx->irq[i])->line = (enum pw_irq_line)i;
	}
	return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ hardware resources */

static NTSTATUS pw_win_prepare_hardware(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
	PW_DEVICE_CTX *ctx = PwDeviceCtx(device);
	ULONG count = WdfCmResourceListGetCount(translated);
	ULONG nmem = 0, nirq = 0;
	PHYSICAL_ADDRESS pa = { 0 };
	ULONGLONG len = 0;

	for (ULONG i = 0; i < count; i++) {
		PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(translated, i);

		if (!d)
			continue;
		switch (d->Type) {
		case CmResourceTypeMemory:
			if (!nmem++) {
				pa = d->u.Memory.Start;
				len = d->u.Memory.Length;
			}
			break;
		case CmResourceTypeMemoryLarge:
			if (!nmem++) {
				pa = d->u.Memory.Start;
				if (d->Flags & CM_RESOURCE_MEMORY_LARGE_40)
					len = (ULONGLONG)d->u.Memory40.Length40 << 8;
				else if (d->Flags & CM_RESOURCE_MEMORY_LARGE_48)
					len = (ULONGLONG)d->u.Memory48.Length48 << 16;
				else if (d->Flags & CM_RESOURCE_MEMORY_LARGE_64)
					len = (ULONGLONG)d->u.Memory64.Length64 << 32;
			}
			break;
		case CmResourceTypeInterrupt: {
			/* The raw descriptor carries the ACPI GSIV (logged for comparison with the
			 * firmware node: UNVERIFIED that Vector holds the GSIV on ARM64). */
			PCM_PARTIAL_RESOURCE_DESCRIPTOR r = i < WdfCmResourceListGetCount(raw) ?
				WdfCmResourceListGetDescriptor(raw, i) : NULL;
			ULONG gsiv = (r && r->Type == CmResourceTypeInterrupt) ? r->u.Interrupt.Vector :
									    0;

			ULONG want = (ctx->plat && nirq < PW_IRQ_COUNT) ? ctx->plat->gsiv[nirq] : 0;

			PW_WLOG(PW_LOG_INFO, "irq %lu (%s): raw vector %lu (firmware node: GSIV %lu%s), "
				"vector %lu level %lu %s", (unsigned long)nirq,
				nirq < PW_IRQ_COUNT ? pw_irq_name[nirq] : "extra", (unsigned long)gsiv,
				(unsigned long)want, !want ? ", board unknown" :
				gsiv == want ? "" : ", DIFFERENT",
				(unsigned long)d->u.Interrupt.Vector,
				(unsigned long)d->u.Interrupt.Level,
				(d->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? "edge" : "level");
			nirq++;
			break;
		}
		default:
			break;
		}
	}
	if (nmem != 1 || nirq != PW_IRQ_COUNT) {
		PW_WLOG(PW_LOG_ERR,
			"expected 1 memory + 3 interrupt resources (ACPI _CRS job/mmu/gpu), got %lu + %lu",
			(unsigned long)nmem, (unsigned long)nirq);
		return STATUS_DEVICE_CONFIGURATION_ERROR;
	}
	if (len < 0x10000 || len > 0x1000000) {
		PW_WLOG(PW_LOG_ERR, "unexpected MMIO window size 0x%llx", (unsigned long long)len);
		return STATUS_DEVICE_CONFIGURATION_ERROR;
	}
	/* Per board: RK3588 0xFB000000 + 0x200000, H616 0x01800000 + 0x40000 (pw_win_plat.c). */
	if (!ctx->plat)
		PW_WLOG(PW_LOG_WARN, "MMIO window 0x%llx+0x%llx (board not recognised)",
			(unsigned long long)pa.QuadPart, (unsigned long long)len);
	else if (len != ctx->plat->mmio_size || pa.QuadPart != (LONGLONG)ctx->plat->mmio_base)
		PW_WLOG(PW_LOG_WARN, "MMIO window is 0x%llx+0x%llx, the %s GPU is 0x%lx+0x%lx",
			(unsigned long long)pa.QuadPart, (unsigned long long)len, ctx->plat->soc,
			(unsigned long)ctx->plat->mmio_base, (unsigned long)ctx->plat->mmio_size);

	ctx->os.mmio = pw_win_map_io(pa, (SIZE_T)len);
	if (!ctx->os.mmio) {
		PW_WLOG(PW_LOG_ERR, "mapping MMIO 0x%llx+0x%llx failed",
			(unsigned long long)pa.QuadPart, (unsigned long long)len);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	ctx->os.mmio_pa = pa;
	ctx->os.mmio_len = (SIZE_T)len;
	PW_WLOG(PW_LOG_INFO, "MMIO 0x%llx+0x%llx mapped at %p (%s)", (unsigned long long)pa.QuadPart,
		(unsigned long long)len, (void *)ctx->os.mmio,
		pw_win_rt.MmMapIoSpaceEx ? "MmMapIoSpaceEx" : "MmMapIoSpace");
	return STATUS_SUCCESS;
}

static NTSTATUS pw_win_release_hardware(WDFDEVICE device, WDFCMRESLIST translated)
{
	PW_DEVICE_CTX *ctx = PwDeviceCtx(device);

	(void)translated;
	ctx->os.regs_enabled = 0;
	if (ctx->os.mmio) {
		MmUnmapIoSpace((PVOID)ctx->os.mmio, ctx->os.mmio_len);
		ctx->os.mmio = NULL;
		ctx->os.mmio_len = 0;
	}
	return STATUS_SUCCESS;
}

/* --------------------------------------------------------------------------- power */

static BOOLEAN pw_win_rk3588_gpu_powered(void)
{
	PHYSICAL_ADDRESS pa;
	volatile UCHAR *pmu;
	ULONG ack, idle, gate, bisr;
	BOOLEAN on;

	pa.QuadPart = (LONGLONG)RK3588_PMU2_BASE;
	pmu = pw_win_map_io(pa, PAGE_SIZE);
	if (!pmu) {
		PW_WLOG(PW_LOG_ERR, "PMU check: cannot map 0x%llx", (unsigned long long)RK3588_PMU2_BASE);
		return FALSE;
	}
	ack = READ_REGISTER_ULONG((volatile ULONG *)(pmu + RK3588_PMU_BUS_IDLE_ACK0));
	idle = READ_REGISTER_ULONG((volatile ULONG *)(pmu + RK3588_PMU_BUS_IDLE_ST0));
	gate = READ_REGISTER_ULONG((volatile ULONG *)(pmu + RK3588_PMU_PWR_GATE_ST0));
	bisr = READ_REGISTER_ULONG((volatile ULONG *)(pmu + RK3588_PMU_BISR_STATUS4));
	MmUnmapIoSpace((PVOID)pmu, PAGE_SIZE);
	on = (bisr & RK3588_GPU_BISR_BIT) && !(idle & RK3588_GPU_NIU_BIT) &&
	     !(ack & RK3588_GPU_NIU_BIT);
	PW_WLOG(on ? PW_LOG_INFO : PW_LOG_ERR,
		"PMU: BISR_STATUS4 0x%08lx BUS_IDLE_ST0 0x%08lx BUS_IDLE_ACK0 0x%08lx PWR_GATE_ST0 "
		"0x%08lx -> GPU %s", (unsigned long)bisr, (unsigned long)idle, (unsigned long)ack,
		(unsigned long)gate, on ? "on" : "NOT accessible");
	return on;
}

/*
 * H616 (firmware/zero3 GPU0 node): evaluate _PSC through ACPI.sys (IOCTL_ACPI_EVAL_METHOD on the
 * PDO, our default I/O target). The node returns 0 only when the PRCM PPU GPU domain is on, the
 * bus clock gate is open and the reset is released, i.e. when a GPU register access cannot hang
 * the bus. ACPI has already run _PS0 when D0Entry is called. UNTESTED ON HARDWARE.
 */
static BOOLEAN pw_win_acpi_psc_on(PW_DEVICE_CTX *ctx)
{
	ACPI_EVAL_INPUT_BUFFER in;
	union {
		ACPI_EVAL_OUTPUT_BUFFER out;
		UCHAR raw[sizeof(ACPI_EVAL_OUTPUT_BUFFER) + 16];
	} o;
	WDF_MEMORY_DESCRIPTOR din, dout;
	WDF_REQUEST_SEND_OPTIONS opt;
	ULONG_PTR got = 0;
	NTSTATUS st;
	ULONG psc;

	RtlZeroMemory(&in, sizeof(in));
	RtlZeroMemory(&o, sizeof(o));
	in.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
	in.MethodName[0] = '_';
	in.MethodName[1] = 'P';
	in.MethodName[2] = 'S';
	in.MethodName[3] = 'C';
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&din, &in, sizeof(in));
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&dout, &o, sizeof(o));
	WDF_REQUEST_SEND_OPTIONS_INIT(&opt, 0);
	WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opt, WDF_REL_TIMEOUT_IN_SEC(2));
	st = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(ctx->device), NULL,
					       IOCTL_ACPI_EVAL_METHOD, &din, &dout, &opt, &got);
	if (!NT_SUCCESS(st)) {
		PW_WLOG(PW_LOG_ERR, "ACPI _PSC: evaluation failed: 0x%08x", (unsigned)st);
		return FALSE;
	}
	if (got < sizeof(ACPI_EVAL_OUTPUT_BUFFER) ||
	    o.out.Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || o.out.Count < 1 ||
	    o.out.Argument[0].Type != ACPI_METHOD_ARGUMENT_INTEGER) {
		PW_WLOG(PW_LOG_ERR, "ACPI _PSC: unexpected result (%lu bytes, signature 0x%08lx, "
			"count %lu, type %u)", (unsigned long)got, (unsigned long)o.out.Signature,
			(unsigned long)o.out.Count, (unsigned)o.out.Argument[0].Type);
		return FALSE;
	}
	psc = o.out.Argument[0].Argument;
	PW_WLOG(psc ? PW_LOG_ERR : PW_LOG_INFO, "ACPI _PSC = %lu -> GPU %s", (unsigned long)psc,
		psc ? "NOT accessible (domain, bus gate or reset still off)" : "on (D0)");
	return psc == 0;
}

static NTSTATUS pw_win_d0_entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
	PW_DEVICE_CTX *ctx = PwDeviceCtx(device);
	const struct pw_win_platform *plat = ctx->plat;

	(void)previous;
	/* Each board's own check, never another board's (pw_win_plat.h). */
	if (!plat)
		ctx->gpu_powered = FALSE;
	else if (plat->rk3588_pmu)
		ctx->gpu_powered = ctx->pmu_check ? pw_win_rk3588_gpu_powered() : TRUE;
	else if (plat->acpi_psc)
		ctx->gpu_powered = ctx->psc_check ? pw_win_acpi_psc_on(ctx) : TRUE;
	else
		ctx->gpu_powered = TRUE;
	if (!ctx->gpu_powered) {
		if (!plat)
			PW_WLOG(PW_LOG_ERR, "board not recognised: not touching GPU registers");
		else if (plat->rk3588_pmu)
			PW_WLOG(PW_LOG_ERR, "GPU power domain is off: not touching GPU registers. "
				"Fix the firmware (edk2 GPU power-up) or set Rk3588PmuCheck=0 at "
				"your own risk");
		else
			PW_WLOG(PW_LOG_ERR, "GPU not powered according to ACPI: not touching GPU "
				"registers. Fix the firmware (GPU0 _PS0/_PSC) or set AcpiPscCheck=0 "
				"at your own risk");
		return STATUS_SUCCESS;
	}
	__atomic_store_n(&ctx->os.regs_enabled, 1, __ATOMIC_SEQ_CST);
	/* Quiesce: no source may assert before a core device exists to handle it. */
	for (int i = 0; i < PW_IRQ_COUNT; i++)
		pw_os_reg_write32(&ctx->os, pw_irq_base[i] + 0x8, 0);
	return STATUS_SUCCESS;
}

static NTSTATUS pw_win_d0_entry_post_irq(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
	PW_DEVICE_CTX *ctx = PwDeviceCtx(device);
	struct pw_device_params p = {
		.os = &ctx->os,
		.fw_name = ctx->fw_name[0] ? ctx->fw_name : NULL,
		.boot_firmware = ctx->boot_firmware != 0,
		.timer_hz = ctx->timer_hz,
	};
	struct pw_device *pw = NULL;
	const char *name = NULL;
	uint32_t maj = 0, min = 0, patch = 0;
	char desc[128];
	int ret;

	(void)previous;
	if (!ctx->gpu_powered) {
		ctx->core_status = PW_EIO;
		return STATUS_SUCCESS;
	}
	for (int i = 0; i < PW_IRQ_COUNT; i++)
		ctx->irq_spurious[i] = 0;

	/* Two-phase create: publish the device to the ISR before probe unmasks anything. */
	ret = pw_device_alloc(&p, &pw);
	ctx->core_status = ret;
	if (ret) {
		PW_WLOG(PW_LOG_ERR, "pw_device_alloc failed: %d", ret);
		return STATUS_SUCCESS;
	}
	__atomic_store_n(&ctx->pw, pw, __ATOMIC_RELEASE);

	ret = pw_device_probe(pw);
	ctx->core_status = ret;
	ctx->backend = pw_device_backend(pw);
	__atomic_store_n(&ctx->probed, 1, __ATOMIC_RELEASE);
	if (ctx->backend == PW_BACKEND_JM && (ctx->boot_firmware || ctx->fw_name[0]))
		PW_WLOG(PW_LOG_INFO, "job-manager GPU: BootFirmware=%lu and FirmwareName=%s are "
			"ignored (no firmware; the core always does its full init)",
			(unsigned long)ctx->boot_firmware, ctx->fw_name[0] ? ctx->fw_name : "(unset)");
	if (ctx->plat && ctx->backend != PW_BACKEND_NONE &&
	    (ctx->backend == PW_BACKEND_CSF) != ctx->plat->csf)
		PW_WLOG(PW_LOG_WARN, "GPU_ID says %s, but the %s board has a %s GPU",
			ctx->backend == PW_BACKEND_CSF ? "CSF" : "job manager", ctx->plat->soc,
			ctx->plat->csf ? "CSF" : "job-manager");
	if (ret) {
		/* Keep the failed device published until D0Exit: interrupts are connected, and only
		 * the disconnect (which flushes the ISR work items) makes destroying it safe. */
		PW_WLOG(PW_LOG_ERR, "pw_device_probe failed: %d (device stays up for diagnostics; "
			"lines masked by glue: job %lu mmu %lu gpu %lu)", ret,
			(unsigned long)ctx->irq_spurious[0], (unsigned long)ctx->irq_spurious[1],
			(unsigned long)ctx->irq_spurious[2]);
		return STATUS_SUCCESS;
	}
	__atomic_store_n(&ctx->core_ready, 1, __ATOMIC_RELEASE);
	pw_device_describe(pw, desc, sizeof(desc));
	pw_device_drm_version(pw, &maj, &min, &patch, &name);
	PW_WLOG(PW_LOG_INFO, "core device up: %s (DRM %s %u.%u.%u)", desc, name ? name : "?",
		maj, min, patch);
	return STATUS_SUCCESS;
}

/*
 * First half of the way down, interrupts still connected: no new file or device-level ioctl may
 * reach the core (core_ready = 0, core_rundown), and every open file is revoked - its pending
 * user waits end with EINTR (libdrm-win retries and then gets ENODEV), its user mappings (BOs
 * and the MMIO page) are unmapped and its core state is released with pw_file_close(). The
 * handles stay open; later ioctls on them fail with ENODEV (a "device lost" for the client).
 */
static NTSTATUS pw_win_d0_exit_pre_irq(WDFDEVICE device, WDF_POWER_DEVICE_STATE target)
{
	PW_DEVICE_CTX *ctx = PwDeviceCtx(device);

	(void)target;
	__atomic_store_n(&ctx->core_ready, 0, __ATOMIC_RELEASE);
	ExWaitForRundownProtectionRelease(&ctx->core_rundown);
	pw_win_revoke_files(ctx);
	return STATUS_SUCCESS;
}

static NTSTATUS pw_win_d0_exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target)
{
	PW_DEVICE_CTX *ctx = PwDeviceCtx(device);
	struct pw_device *pw;

	(void)target;
	/* Interrupts are already disconnected and their work items flushed by KMDF, and
	 * pw_win_d0_exit_pre_irq() revoked every file and drained device-level users, so nothing
	 * else can be using the core device. */
	__atomic_store_n(&ctx->core_ready, 0, __ATOMIC_RELEASE);
	pw = __atomic_exchange_n(&ctx->pw, NULL, __ATOMIC_ACQ_REL);
	if (pw)
		pw_device_destroy(pw);
	__atomic_store_n(&ctx->probed, 0, __ATOMIC_RELEASE);
	ctx->backend = PW_BACKEND_NONE;
	__atomic_store_n(&ctx->os.regs_enabled, 0, __ATOMIC_SEQ_CST);
	ExReInitializeRundownProtection(&ctx->core_rundown); /* for the next D0Entry */
	if (ctx->irq_spurious[0] | ctx->irq_spurious[1] | ctx->irq_spurious[2])
		PW_WLOG(PW_LOG_WARN, "lines masked by glue this session: job %lu mmu %lu gpu %lu",
			(unsigned long)ctx->irq_spurious[0], (unsigned long)ctx->irq_spurious[1],
			(unsigned long)ctx->irq_spurious[2]);
	return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- device add */

static NTSTATUS pw_win_device_add(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
	WDF_PNPPOWER_EVENT_CALLBACKS pnp;
	WDF_FILEOBJECT_CONFIG fcfg;
	WDF_OBJECT_ATTRIBUTES attr;
	UNICODE_STRING link;
	PW_DEVICE_CTX *ctx;
	WDFDEVICE device;
	NTSTATUS st;

	(void)driver;
	WdfDeviceInitSetDeviceType(init, PANWIN_FILE_DEVICE);
	WdfDeviceInitSetCharacteristics(init, FILE_DEVICE_SECURE_OPEN, TRUE);
	WdfDeviceInitSetIoType(init, WdfDeviceIoBuffered);

	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
	pnp.EvtDevicePrepareHardware = pw_win_prepare_hardware;
	pnp.EvtDeviceReleaseHardware = pw_win_release_hardware;
	pnp.EvtDeviceD0Entry = pw_win_d0_entry;
	pnp.EvtDeviceD0EntryPostInterruptsEnabled = pw_win_d0_entry_post_irq;
	pnp.EvtDeviceD0ExitPreInterruptsDisabled = pw_win_d0_exit_pre_irq;
	pnp.EvtDeviceD0Exit = pw_win_d0_exit;
	WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);

	WDF_FILEOBJECT_CONFIG_INIT(&fcfg, pw_win_file_create, pw_win_file_close,
				   pw_win_file_cleanup);
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, PW_FILE_CTX);
	WdfDeviceInitSetFileObjectConfig(init, &fcfg, &attr);

	WdfDeviceInitSetIoInCallerContextCallback(init, pw_win_io_in_caller_context);
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, PW_REQ_CTX);
	WdfDeviceInitSetRequestAttributes(init, &attr);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, PW_DEVICE_CTX);
	st = WdfDeviceCreate(&init, &attr, &device);
	if (!NT_SUCCESS(st)) {
		PW_WLOG(PW_LOG_ERR, "WdfDeviceCreate failed: 0x%08x", (unsigned)st);
		return st;
	}
	ctx = PwDeviceCtx(device);
	ctx->device = device;
	pw_os_win_init(&ctx->os, device);
	pw_win_files_init(ctx);
	ctx->plat = pw_win_query_platform(device);
	pw_win_read_params(ctx);
	ctx->timer_hz = pw_win_read_cntfrq();

	st = WdfDeviceCreateDeviceInterface(device, &pw_interface_guid, NULL);
	if (!NT_SUCCESS(st)) {
		PW_WLOG(PW_LOG_ERR, "WdfDeviceCreateDeviceInterface failed: 0x%08x", (unsigned)st);
		return st;
	}
	/* Convenience name for tools; a second GPU instance just goes without it. */
	RtlInitUnicodeString(&link, L"\\DosDevices\\PanWin0");
	st = WdfDeviceCreateSymbolicLink(device, &link);
	if (!NT_SUCCESS(st))
		PW_WLOG(PW_LOG_WARN, "symbolic link \\DosDevices\\PanWin0 not created: 0x%08x",
			(unsigned)st);

	st = pw_win_queue_init(ctx);
	if (!NT_SUCCESS(st))
		return st;
	return pw_win_create_interrupts(ctx);
}

static VOID pw_win_driver_unload(WDFDRIVER driver)
{
	(void)driver;
	PW_WLOG(PW_LOG_INFO, "unloaded");
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT driver_object, PUNICODE_STRING registry_path)
{
	WDF_DRIVER_CONFIG cfg;
	NTSTATUS st;
	uint32_t maj, min, patch;
	const char *name = NULL;

	pw_win_resolve_routines();
	/* Without a device this is the CSF (panthor) version; a job-manager device reports
	 * panfrost, logged per device in pw_win_d0_entry_post_irq(). */
	pw_device_drm_version(NULL, &maj, &min, &patch, &name);
	PW_WLOG(PW_LOG_INFO, "panwin.sys " PANWIN_VERSION_STRING " (ABI %u, CSF uAPI %s %u.%u.%u, "
		"JM uAPI panfrost%s), ExAllocatePool2 %s, MmMapIoSpaceEx %s", PANWIN_ABI_VERSION,
		name ? name : "panthor", maj, min, patch,
		PANWIN_DEBUG_IOCTLS ? ", debug ioctls" : "",
		pw_win_rt.ExAllocatePool2 ? "yes" : "no", pw_win_rt.MmMapIoSpaceEx ? "yes" : "no");

	WDF_DRIVER_CONFIG_INIT(&cfg, pw_win_device_add);
	cfg.EvtDriverUnload = pw_win_driver_unload;
	cfg.DriverPoolTag = PW_POOL_TAG;
	st = WdfDriverCreate(driver_object, registry_path, WDF_NO_OBJECT_ATTRIBUTES, &cfg,
			     WDF_NO_HANDLE);
	if (!NT_SUCCESS(st))
		PW_WLOG(PW_LOG_ERR, "WdfDriverCreate failed: 0x%08x", (unsigned)st);
	return st;
}
