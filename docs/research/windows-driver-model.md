# Windows driver model and toolchain for a Mali (RK3588 / G610) GPU driver on Windows on ARM

Researcher topic: the Windows driver model, building drivers on Linux, signing, and the
MMIO/IRQ/DMA/ACPI plumbing.
Date: 2026-10-30. Status: research and toolchain experiments. **Nothing here has run on real ARM64 Windows.**

Legend: **[V]** means verified: I read the code or doc, or ran the command here. **[I]** means inference or memory that still needs checking. **[?]** means an open question.

Reference clones I own:

- `references/Windows-driver-samples`: sparse clone of `video/KMDOD`, `video/IndirectDisplay`, `general/echo`, `general/ioctl` and `general/PLX9x5x`, at commit f4c1ff8 (2026-10-28).
- `references/Windows-Driver-Frameworks`: shallow clone at b6191d9 (2024-09-10). It holds the MIT-licensed KMDF/UMDF sources and public headers.
- `references/mingw-w64`: sparse clone, `mingw-w64-crt/lib{32,64,arm32}/{ntoskrnl,hal}.def` only.
- `references/toolchain-try`: my driver build experiment (see §2).

---

## 0. TL;DR recommendation

1. **Phase 1: KMDF function driver plus a private IOCTL uAPI (option c).** The kernel driver is a PnP
   KMDF driver bound to an ACPI node such as `ACPI\PFWG0610` that edk2 has to add. It exposes a panthor-shaped IOCTL set. The user-mode
   side is panvk with a new Windows kmod backend. The Vulkan ICD is registered through
   `HKLM\SOFTWARE\Khronos\Vulkan\Drivers`, and presentation uses Mesa's existing GDI blit WSI path.
   OpenGL comes from Mesa's own `opengl32.dll` (WGL frontend) on zink.
   - We can build this **today on Linux** with `/opt/llvm-mingw`. I built a WDM driver and a KMDF driver
     (§2) whose imports, machine type, subsystem, WDM flag, `.pdata` and checksum all check out.
2. **Phase 2 (optional): a WDDM render-only/MCDM-style KMD.** This adds Task Manager visibility, TDR, VidMm
   residency, OS scheduling and per-process GPU VA. The Vulkan ICD would then be found per adapter (`VulkanDriverName`)
   and the UMD would talk to the kernel through D3DKMT (the headers are MIT, `microsoft/libdxg`). The kernel side needs the **real WDK
   headers** (`d3dkmddi.h`, `dispmprt.h`, and `displib.lib` for `DxgkInitialize`). No complete open copy exists, so
   phase 2 needs the WDK NuGet package. That package needs license acceptance: **ask the user**.
3. **Full WDDM with display and DWM composition on Mali (option a) is a multi-year effort.** DWM needs a D3D11 UMD. A
   D3D12 UMD plus the OS 11on12 path (`D3D11_UMD_MODE_SYSTEM11ON12`) is the least-bad route. Either way it means writing a D3D12 DDI driver.
   It also needs a separate VOP2 display KMD. Not recommended before phases 1 and 2.

---

## 1. GPU driver options on Windows

### (a) Full WDDM display + render driver

- The KMD is a display miniport: `DriverEntry` calls `DxgkInitialize(DriverObject, RegistryPath, &DRIVER_INITIALIZATION_DATA)`.
  `DxgkInitialize` is not an ntoskrnl export. It lives in the static WDK lib `displib.lib`, which binds to `dxgkrnl.sys` **[I]**.
  The open-source KMDOD sample uses `DxgkInitializeDisplayOnlyDriver` (`video/KMDOD/bdd_ddi.cxx:64`) and reports
  `WDDMVersion = DXGKDDI_WDDMv1_2` (`video/KMDOD/bdd.cxx:293`) **[V]**.
- The UMD is registered in the adapter's class key
  `HKLM\System\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\000N`
  through `UserModeDriverName`, `OpenGLDriverName`, `OpenGLVersion` and `OpenGLFlags` (plus `*Wow` values for 32-bit). The MS doc says
  `UserModeDriverName` is "required for the operation of a Direct3D rendering device" **[V]**
  (https://learn.microsoft.com/en-us/windows-hardware/drivers/display/loading-an-opengl-installable-client-driver).
- **Is a D3D12 UMD mandatory for DWM?** DWM is a D3D11 client **[I]**, so the display adapter needs a D3D11 UMD.
  The WDK's `d3d10umddi.h` defines `D3D11_UMD_MODE { NATIVE11, SYSTEM11ON12, VENDOR11ON12 }`, which is returned by
  `PFND3D11DDI_QUERYDESIREDMODE1` **[V]** (https://ntdoc.m417z.com/d3d11_umd_mode). So a vendor can ship a D3D12 UMD and
  let the OS-shipped 11on12 run D3D11 on top of it. That means D3D12 is the realistic "one big UMD", but **some** D3D UMD is
  unavoidable for DWM on that adapter. A Vulkan ICD alone cannot drive DWM. Writing a D3D12 DDI driver is the
  dominant cost (person-years).
- Display: the RK3588 VOP2/HDMI would need its own display KMD (a KMDOD-style or full display miniport). Today Windows on
  edk2-rk3588 uses the GOP framebuffer with the Microsoft Basic Display Driver **[I]**.
- Cost: very high. Not a starting point.

### (b) WDDM render-only / MCDM (compute-only)

- MCDM requires Windows 10 1903 / WDDM 2.6 or later. It is "a scaled down subset of WDDM 2.0+", and "the driver must advertise itself as
  a *render-only* device without display functionality" **[V]**
  (https://learn.microsoft.com/en-us/windows-hardware/drivers/display/mcdm).
- From the MCDM KMD guidelines **[V]**
  (https://learn.microsoft.com/en-us/windows-hardware/drivers/display/mcdm-implementation-guidelines):
  - INF: `Class=ComputeAccelerator`, `ClassGuid={F01A9D53-3FF6-48D2-9F97-C8A7004BE10C}`.
  - Required DDIs (~37): AddDevice, StartDevice, StopDevice, RemoveDevice, Unload, BuildPagingBuffer, CalibrateGpuClock,
    Create/Open/Close/Destroy/DescribeAllocation, CreateContext/DestroyContext, CreateDevice/DestroyDevice,
    DpcRoutine, InterruptRoutine, FormatHistoryBuffer, GetNodeMetadata, GetStandardAllocationDriverData,
    PreemptCommand, QueryAdapterInfo, QueryDependentEngineGroup, QueryEngineStatus, QueryChildRelations/Status,
    ResetDevice/ResetEngine/ResetFromTimeout/RestartFromTimeout, SetPowerState, SetStablePowerState,
    SetVirtualMachineData, CollectDbgInfo.
  - With GPU VA: CreateProcess/DestroyProcess, GetRootPageTableSize, SetRootPageTable, SubmitCommandVirtual.
  - Optional groups: hardware scheduling (CreateHwContext/CreateHwQueue/SubmitCommandToHwQueue/SignalMonitoredFence/…),
    `DxgkDdiEscape` (private channel), power runtime (`SetPowerComponentFState`, required on Modern Standby systems).
  - DRIVERCAPS: `WDDMVersion >= DXGKDDI_WDDMv2_6`, `ComputeOnly = TRUE`, `SupportPerEngineTDR = TRUE`,
    `SchedulingCaps.MultiEngineAware = PreemptionAware = TRUE`, `ComputePreemptionGranularity != NONE`,
    `MemoryManagementCaps.VirtualAddressingSupported` with `GpuMmuSupported` and/or `IoMmuSupported`.
    "Virtual addressing is required." The user-mode feature subset is D3D12 Core 1.0.
- A **render-only graphics** adapter (Display class, no VidPN/display DDIs, not ComputeOnly) has existed since WDDM 1.2
  (Microsoft Basic Render Driver is one) **[V: search result/WDDM 1.2 docs]**. For graphics rather than compute it is
  the better fit, but the D3D runtime needs a D3D UMD on it. **[I]** dxgkrnl does not check that a UMD exists when it loads the KMD, so a KMD
  with only a Vulkan ICD should load. D3D11/12 device creation on that adapter would just fail. **[?]** Needs a test on hardware.
- **GPU VA model mapping to Mali CSF:**
  - Mali G610 (Valhall v10, CSF) has its own MMU with up to 8–16 address spaces (AS). AS0 is used by the MCU firmware. Panthor uses the standard
    AArch64 LPAE stage-1 format (4 KB granule, 48-bit VA) **[I: from panthor knowledge; the panthor researcher should verify]**.
    → **GpuMmu model** fits. VidMm "is responsible for allocating, growing, updating, ensuring residency, and freeing page
    tables. The hardware format … is abstracted through DDIs", and the UMD assigns GPU VAs (the same as panthor's VM_BIND) **[V]**
    (https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpummu-model).
  - `DXGK_GPUMMUCAPS.PageTableUpdateMode = DXGK_PAGETABLEUPDATE_CPU_VIRTUAL` lets the KMD write PTEs with the CPU (Mali has no
    paging engine). `PageTableLevelCount` can be up to `DXGK_MAX_PAGE_TABLE_LEVEL_COUNT` (min 2). Mali can update PTEs under an AS lock,
    so `PageTableUpdateRequireAddressSpaceIdle = 0` **[V caps fields; mapping is I]**
    (https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_gpummucaps).
  - **IoMmu model does not fit.** It is for devices that share the CPU page tables (SVM). The RK3588 GPU node has no `iommus` property
    (`linux/arch/arm64/boot/dts/rockchip/rk3588-base.dtsi:449-465`) **[V]**.
  - With 8–16 AS slots and WDDM's "one root page table per process", the KMD multiplexes processes onto AS slots
    (LRU), the same way panthor does **[I]**.
- **Hardware scheduling (HwSch, WDDM 2.7+)** maps conceptually onto CSF (firmware-scheduled CS groups/queues, ring buffers, doorbells,
  `SYNC_SET64/ADD64` usable as monitored fences). But the HwSch contract (run lists, priority bands, context suspend/resume through
  the firmware) is a lot of work. Start with the **software (VidSch) packet scheduler**: `SubmitCommandVirtual` puts each
  DMA buffer on a single CSG queue per context and raises `DXGK_INTERRUPT_DMA_COMPLETED` / monitored fences from the IRQ **[I]**.
- **Per-adapter Vulkan ICD:** the Vulkan loader reads `VulkanDriverName` / `VulkanDriverNameWow` from the adapter key and from
  SoftwareComponent child keys **[V]**. It does this through `D3DKMTEnumAdapters2` + `D3DKMTQueryAdapterInfo` (registry query) in
  `windows_read_manifest_from_d3d_adapters` (Vulkan-Loader `loader/loader_windows.c:511-565`, HEAD 15d99425) and through
  CM_* enumeration of the Display class GUID with SoftwareComponent `{5c4c3332-344d-483c-8739-259e934c9cc8}` children (`:216`).
  Physical devices are sorted by DXGI adapter LUID (`IDXGIFactory6::EnumAdapterByGpuPreference`, ~`:850`).
  **[?]** I have not checked whether the D3DKMT path enumerates `ComputeAccelerator`-class adapters. The CM path uses only the Display class GUID **[V]**,
  so for graphics use a render-only adapter in the Display class.
- UMD↔KMD channel without a D3D UMD: user mode can call D3DKMT thunks directly (gdi32): `D3DKMTCreateDevice`,
  `CreateContextVirtual`, `CreateAllocation2`, `MapGpuVirtualAddress`, `SubmitCommand`, `Escape`. The official
  **`d3dkmthk.h` / `d3dkmdt.h` / `d3dukmdt.h` are MIT** in https://github.com/microsoft/libdxg (`include/dxg/`) **[V]**, so
  panvk could include them without WDK licensing. Escape (`DxgkDdiEscape`) could tunnel a panthor-like uAPI early on.
- **Kernel-side headers:** ReactOS has MIT/PD **partial** `d3dkmddi.h` (2074 lines, many DDI typedefs stubbed as `VOID*`,
  e.g. `PDXGKDDI_SUBMITCOMMANDVIRTUAL`) and `dispmprt.h` (1754 lines) **[V: fetched]**. They are not enough for a WDDM 2.6+ KMD.
  Realistically we need the WDK `d3dkmddi.h`.
- Cost: high (my estimate is 4–8 person-months beyond phase 1 for VidMm/VidSch integration, paging buffers, fences, TDR). It brings
  multi-process isolation enforced by the OS, Task Manager, TDR and scheduler fairness.

### (c) Plain KMDF/WDM device driver + private IOCTLs + Vulkan/GL ICDs (recommended phase 1)

- **Vulkan registration without WDDM: confirmed [V].** The loader scans `HKLM\SOFTWARE\Khronos\Vulkan\Drivers`
  (`VK_DRIVERS_INFO_REGISTRY_LOC "SOFTWARE\\Khronos\\Vulkan" VK_VARIANT_REG_STR "\\Drivers"`,
  `loader/vk_loader_platform.h:212`). Value **name** = full path to the JSON manifest, type DWORD, `0` = enabled.
  32-bit apps on 64-bit Windows use `HKLM\SOFTWARE\WOW6432Node\...`. The doc calls this key "the older method … but is the primary
  location for software based drivers". Unknown driver names are kept: `loader_windows.c:439` logs
  "not recognized as a known driver. It will be assumed to be active". Only `igvk64.json`, `nv-vk64.json`,
  `amd-vulkan64.json` and `amdvlk64.json` are checked against DXGI vendor IDs (`:347-372`).
  `VK_DRIVER_FILES` / `VK_ADD_DRIVER_FILES` also work for dev, but are ignored for elevated processes
  (https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md).
  - The manifest's `library_path` points at e.g. `vulkan_panfrost.dll` (aarch64). A Windows ARM64 loader (`vulkan-1.dll`) is needed on the
    device. The Vulkan Runtime installer or an app-local copy both work **[I]**.
- **Presentation without WDDM: confirmed from Mesa source [V].** `src/vulkan/wsi/wsi_common_win32.cpp` has two paths.
  The DXGI swapchain is used only if the driver supplies `win32.get_d3d12_command_queue` (Dozen), see `:188`, `:420`.
  Otherwise it uses the GDI path: `CreateDIBSection` (`:592`), then a row memcpy from `image->base.cpu_map` into the DIB and a
  `StretchBlt` to the window DC (`wsi_win32_queue_present`, ~`:830-855`). Mali is UMA, so a linear, host-visible
  WC/uncached image works. Cost: one CPU copy per frame (1080p×4 B ≈ 8.3 MB/frame), and DWM then composites through Basic
  Display/WARP. That is fine for bring-up, but it is a real throughput limit (~0.5 GB/s at 60 fps) **[I]**.
- **OpenGL ICD discovery [V]:** on WDDM, `opengl32.dll` calls `D3DKMTQueryAdapterInfo(KMTQAITYPE_UMOPENGLINFO)` and loads
  `OpenGLDriverName` from the **display adapter's** class key. With a non-WDDM device the adapter for the window's HDC
  is Basic Display, which has no GL ICD, so opengl32 falls back to Microsoft's GDI generic GL 1.1 **[I]**. The legacy
  `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\<name>` key was keyed by the XPDM display driver name
  **[I]**. I found no evidence that it works for a non-adapter device. **Workaround:** ship Mesa's own `opengl32.dll`
  (`src/gallium/targets/libgl-gdi`, `opengl32.def.in`) app-local, or `libgallium_wgl.dll` (`src/gallium/targets/wgl`). The WGL
  frontend already supports zink (`src/gallium/frontends/wgl/meson.build:6-7`) **[V]**. Select it with `GALLIUM_DRIVER=zink`.
- D3D9/10/11/12 apps: DXVK / vkd3d-proton ship their own `d3d*.dll`/`dxgi.dll`, so they do not need a WDDM adapter **[I]**.
- Downsides: no Task Manager GPU, no OS TDR (we implement our own hang detection and reset), no VidMm residency or
  overcommit, isolation is entirely our responsibility (validate every IOCTL), and DWM/D3D apps don't benefit.
- Upside: a minimal kernel surface. It maps 1:1 onto panthor (`DEV_QUERY`, `VM_CREATE/BIND`, `BO_CREATE/MMAP_OFFSET`,
  `GROUP_CREATE/SUBMIT`, `TILER_HEAP_*`, syncobj). It is buildable with open headers and our toolchain today (§2), and it is
  debuggable with WinDbg/KDNET.
- Cost estimate **[I]**: KMD with panthor feature parity at 2–4 person-months, and a panvk Windows kmod backend at 1–2 person-months.

### Option matrix

| | (c) KMDF + IOCTL | (b) WDDM render-only/MCDM | (a) Full WDDM |
|---|---|---|---|
| Kernel headers | mingw DDK (patched) + MIT KMDF | WDK required | WDK required |
| Builds on Linux now | **yes (tested)** | with WDK NuGet + clang (untested) | same as (b) |
| Vulkan ICD discovery | Khronos key | adapter `VulkanDriverName` | adapter |
| Present | GDI blit (Mesa, exists) | GDI blit (D3DKMT present: hard) | DXGI/DWM native |
| DWM/D3D native | no | no (unless D3D UMD) | yes (needs D3D12 UMD + 11on12) |
| Effort | lowest | +4–8 pm | years |

---

## 2. Building ARM64 Windows kernel drivers on Linux (actually tried)

Location: `/home/e3xp0/Projects/panfrost-win/references/toolchain-try/`. Running `./build.sh` reproduces everything.

Files:
- `src/hello.c`: WDM legacy driver. It creates `\Device\PanHello` + `\DosDevices\PanHello` and handles CREATE/CLOSE plus a
  METHOD_BUFFERED IOCTL that returns a version, and has an unload routine.
- `src/kmdf_hello.c`: KMDF driver (`WdfDriverCreate`, `EvtDeviceAdd` → `WdfDeviceCreate` + default queue).
- `src/fxstub.c`: **clean-room KMDF client stub** that replaces the WDK's `WdfDriverEntry.lib`. Its entry point `FxDriverEntry` calls
  `WdfVersionBind(drv, reg, &bindinfo, &WdfDriverGlobals)` → `DriverEntry` → it hooks unload and calls `WdfVersionUnbind`.
  The bind-info layout follows MIT `fxldr.h:79` (`Size, Component=L"KmdfLibrary", {Major,Minor,Build}, FuncCount, FuncTable, Module`).
  ReactOS has a GPL version with the same logic (`sdk/lib/drivers/wdf/wdfdriverentry/wdfdriverentry.c`). I did not copy it.
- `include/ddk/wdm.h`, `include/ddk/ntddk.h`: copies of the llvm-mingw DDK headers with an added
  `#elif defined(__aarch64__)` arch block. **The stock mingw `wdm.h`/`ntddk.h` hit `#error Unknown Architecture` on aarch64 [V]**
  (`wdm.h:8328-8333`, `ntddk.h:3342-3344`). The block defines IRQL levels, PAGE_SIZE, KFLOATING_SAVE, YieldProcessor, and
  declares the IRQL routines as imports. **[?]** On ARM64 the real WDK probably inlines `KeGetCurrentIrql` as a read of the
  KPCR through x18. Wine lists `KeGetCurrentIrql`/`KeLowerIrql`/`KfRaiseIrql` as `-arch=x86_64` only (wine
  `dlls/ntoskrnl.exe/ntoskrnl.exe.spec:568,608,686`) **[V]**. **So don't import IRQL functions until we've checked them against a
  real ARM64 ntoskrnl.** I removed the call from `hello.c`.
- `include/wdf/`: KMDF 1.15 public headers (MIT) copied from `Windows-Driver-Frameworks/src/publicinc/wdf/kmdf/1.15`. One patch:
  the `WDF_REQUEST_TYPE` enum moved from `wdfrequest.h` to replace the forward declaration in `wdfdevice.h:497`, because clang/C rejects
  an incomplete-enum parameter at `wdfdevice.h:4174`; MSVC accepts it. `include/wdfcase/` has case-fixing symlinks
  (`WdfQueryInterface.h`, `WdfWMI.h`, `wdfDmaEnabler.h`, …).
- `lib/ntoskrnl.def`, `lib/hal.def`: **approximate** ARM64 export lists. mingw-w64 has no `libarm64/ntoskrnl.def` **[V]**, so each is the
  union of `libarm32` (gendef from a real ARM32 ntoskrnl, 2427 names) and `lib64` (2129 names), 2487 names in total.
  `lib/wdfldr.def`: WdfVersionBind/Unbind/BindClass/UnbindClass, WdfRegisterLibrary/ClassLibrary, WdfLdrQueryInterface
  (cross-checked with ReactOS `wdfldr.spec`).

Exact commands that worked (from `build.sh`):

```sh
# import libs
/opt/llvm-mingw/bin/llvm-dlltool -m arm64 -d lib/ntoskrnl.def -l lib/libntoskrnl.a
/opt/llvm-mingw/bin/llvm-dlltool -m arm64 -d lib/hal.def      -l lib/libhal.a
/opt/llvm-mingw/bin/llvm-dlltool -m arm64 -d lib/wdfldr.def   -l lib/libwdfldr.a
# compile
CFLAGS="-O2 -Wall -Wno-pragma-pack -Wno-unknown-pragmas -Wno-nonportable-include-path -Wno-switch \
 -ffreestanding -fno-builtin -fno-stack-protector -funwind-tables -fno-auto-import \
 -D_KERNEL_MODE -DNTDDI_VERSION=0x0A000000 -D_WIN32_WINNT=0x0A00 \
 -Iinclude/ddk -I/opt/llvm-mingw/aarch64-w64-mingw32/include/ddk -Iinclude/wdf -Iinclude/wdfcase"
aarch64-w64-mingw32-clang -c $CFLAGS src/hello.c -o build/hello.o      # likewise kmdf_hello.c, fxstub.c
# link (lld-link directly, MSVC-style)
LDFLAGS="/machine:arm64 /driver /driver:wdm /subsystem:native,10.0 /nodefaultlib /dynamicbase /nxcompat \
 /tsaware:no /release /base:0x140000000 /filealign:0x1000 /align:0x1000 /opt:ref /opt:icf"
lld-link $LDFLAGS /entry:DriverEntry   /out:build/panhello.sys  build/hello.o lib/libntoskrnl.a lib/libhal.a
lld-link $LDFLAGS /entry:FxDriverEntry /out:build/kmdfhello.sys build/kmdf_hello.o build/fxstub.o \
         lib/libwdfldr.a lib/libntoskrnl.a lib/libhal.a
```

`llvm-readobj --file-headers --sections --coff-imports` results **[V]**:
- `panhello.sys`: `Machine: IMAGE_FILE_MACHINE_ARM64 (0xAA64)`, `Subsystem: IMAGE_SUBSYSTEM_NATIVE`,
  `IMAGE_DLL_CHARACTERISTICS_WDM_DRIVER (0x2000)` + DYNAMIC_BASE/HIGH_ENTROPY_VA/NX_COMPAT, a non-zero `CheckSum`, and sections
  `.text .rdata .data .pdata .reloc` (`ExceptionTableSize 0x20`). It imports only from `ntoskrnl.exe`: DbgPrintEx, IoCreateDevice,
  IoCreateSymbolicLink, IoDeleteDevice, IoDeleteSymbolicLink, IofCompleteRequest.
- `kmdfhello.sys`: the same header properties. It imports `WDFLDR.SYS!WdfVersionBind/WdfVersionUnbind` plus ntoskrnl DbgPrintEx,
  ExAllocatePoolWithTag, ExFreePoolWithTag, RtlCopyUnicodeString, memset.
- Struct offsets in the disassembly match the real 64-bit layout: `DRIVER_OBJECT.DriverUnload @0x68`, `MajorFunction @0x70`,
  `DEVICE_OBJECT.Flags @0x30` **[V disasm; layout from memory I]**.

Gotchas found (all **[V]** here unless marked):
1. **`-ffreestanding` silently drops `.pdata/.xdata` on aarch64-w64-mingw32.** Add `-funwind-tables`, otherwise there is no unwind info,
   and kernel exception dispatch and stack walks break.
2. **`/driver:wdm` alone does not count as `/driver`.** lld still warns "/align specified without /driver". Pass both `/driver /driver:wdm`.
   Without `/tsaware:no`, lld sets TERMINAL_SERVER_AWARE. Without `/release` the checksum is 0.
3. **x18 is reserved** for `*-windows` triples. In a 40-variable register-pressure test, aarch64-w64-mingw32 used x18 0 times while
   aarch64-linux-gnu used it 3 times.
4. **SEH works:** `__try/__except` compiles with `-fms-extensions` on aarch64-w64-mingw32 and emits an xdata handler
   `__C_specific_handler`, which is in the ntoskrnl def. That is needed around `ProbeForRead/Write` and `MmProbeAndLockPages`. It has not been run.
5. With `-fno-auto-import` the compiler no longer emits `.refptr` indirections. mingw pseudo-relocations must never be needed: there is no runtime to apply them.
6. No CRT: `memset/memcpy/memmove/memcmp/strlen/__chkstk/__C_specific_handler` come from ntoskrnl (they are in the def; **[I]** they exist
   on ARM64). Stack protector: either keep `-fno-stack-protector` or provide `__stack_chk_guard`/`__stack_chk_fail`
   (→ `KeBugCheckEx`). The MSVC `/GS` equivalent (`__security_cookie`) lives in the static WDK lib `BufferOverflowFastFailK.lib`.
7. FP/NEON in the kernel: "Arm64 in general, is similar to AMD64 in that you don't need to call save floating point state
   first" **[V]** (https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/using-floating-point-or-mmx-in-a-wdm-driver).
8. The `aarch64-pc-windows-msvc` triple with mingw headers fails (`vadefs.h: VARARGS not implemented`) **[V]**. The msvc triple
   needs real SDK/WDK headers.
9. Import robustness: until we have a real ARM64 export list, keep the static imports to the core set and resolve the rest with
   `MmGetSystemRoutineAddress`, the same pattern the PLX9x5x sample uses for `MmMapIoSpaceEx` (`general/PLX9x5x/sys/Init.c:34-68`).
   A missing static import means the load fails with error 127 / `STATUS_PROCEDURE_NOT_FOUND` **[I]**.
10. CFG (`/guard:cf`) is optional for drivers. HVCI compatibility means no RWX sections, page-aligned sections, NX pool
    (`ExAllocatePool2`/`NonPagedPoolNx`). Our images have RX `.text` and RW `.data`, both 4 KB aligned **[V]**.

**Getting the real ARM64 export lists (top TODO once hardware exists):** copy `C:\Windows\System32\ntoskrnl.exe`, `hal.dll`,
`drivers\WDFLDR.SYS` and `drivers\dxgkrnl.sys` from the RK3588 Windows install, then run
`gendef ntoskrnl.exe` (`/opt/llvm-mingw/bin/gendef` exists) or `llvm-readobj --coff-exports`, and replace `lib/*.def`.

**WDK route (plan B, needed for WDDM):** NuGet `Microsoft.Windows.WDK.ARM64` 10.0.28000.2526 (49,371,679 bytes,
`requireLicenseAcceptance=true`, depends on `Microsoft.Windows.SDK.cpp.ARM64 [10.0.28000,10.0.28001)`) **[V: nuget API]**.
**I did not download it, because it needs license acceptance from the user.** With it: `clang --target=aarch64-pc-windows-msvc`
(or `clang-cl`) + WDK/SDK headers + `lld-link` against the WDK `ntoskrnl.lib`, `hal.lib`, `wdfldr.lib`, `WdfDriverEntry.lib`,
`displib.lib` and `BufferOverflowFastFailK.lib` **[I, untested]**. Alternative: a Windows CI runner (GitHub Actions `windows-latest`)
with MSBuild + WDK, which cross-compiles for ARM64 **[I]**.

Not possible here: running the ARM64 `.sys` (no qemu-system-aarch64, and Wine has no ARM64 kernel). **[I]** As a cheap smoke test of
platform-independent driver logic, the same sources could be built for x86_64-w64-mingw32 and loaded by Wine's `winedevice.exe`.

---

## 3. Signing and test-signing on Windows ARM64

- `bcdedit /set testsigning on` + reboot. If it reports "protected by Secure Boot policy", disable Secure Boot in firmware
  (on edk2-rk3588 this is a setup option **[I]**). When test signing is enabled, "Any certificate can sign drivers … Certificate
  validation isn't required to come from a trusted root … However, each driver image file must have a digital signature."
  With HVCI on, "you must test-sign the binary using any self-created test cert. An unsigned binary isn't supported" **[V]**
  (https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option).
  There are no ARM64-specific differences in that doc. One-shot alternative: Advanced Startup → "Disable driver signature enforcement" (F7) **[I]**.
- Certificate on Linux **[V, ran in scratchpad]**:
  ```sh
  openssl req -x509 -newkey rsa:3072 -sha256 -days 3650 -nodes -keyout pf-test.key -out pf-test.crt \
    -subj "/CN=panfrost-win test signing" -addext "extendedKeyUsage=codeSigning" \
    -addext "keyUsage=critical,digitalSignature" -addext "basicConstraints=critical,CA:FALSE"
  openssl pkcs12 -export -inkey pf-test.key -in pf-test.crt -out pf-test.pfx -passout pass:test
  ```
- Embedded Authenticode on Linux: `osslsigncode` (https://github.com/mtrojnar/osslsigncode) **[I, not run]**:
  `osslsigncode sign -certs pf-test.crt -key pf-test.key -h sha256 -ph -n "panfrost-win" -in x.sys -out x-signed.sys`
  then `osslsigncode verify -in x-signed.sys -CAfile pf-test.crt`. It is **not installed** here. It is not in the local pacman
  sync DB, and the permission classifier blocked building it from a fresh clone, so **the user should install it**. osslsigncode can sign
  an existing `.cat` but cannot **create** one. `/usr/bin/signtool` on this machine is NSS's JAR signer, not Microsoft signtool **[V]**.
- Driver-package catalog (`.cat`, needed for PnP install via `pnputil /add-driver x.inf /install`):
  - On the device itself, without the WDK: PowerShell `New-SelfSignedCertificate -Type CodeSigningCert`, import the cert into
    `Cert:\LocalMachine\Root` and `TrustedPublisher`, `New-FileCatalog -CatalogVersion 2.0`, and `Set-AuthenticodeSignature` on the `.cat`
    and `.sys`. **[?]** I have not checked whether pnputil accepts a `New-FileCatalog` catalog (it lacks OS attributes). Inf2Cat-made catalogs are the known-good path.
  - With the SDK/WDK on any Windows box: `Inf2Cat /driver:dir /os:10_NI_ARM64` (or `makecat`) + `signtool sign /fd sha256`.
  - Simplest bring-up that avoids catalogs: a **non-PnP legacy service** (`sc create panhello type= kernel binPath= ...`,
    `sc start panhello`). It needs only the embedded signature in test mode. It is good for the hello driver and for
    MmMapIoSpaceEx MMIO pokes, but it cannot get translated GIC interrupts, so the real driver must be PnP (ACPI).
- Production signing (attestation through the Partner Center with an EV cert) is out of scope.

---

## 4. Interrupts, MMIO, DMA, ACPI on ARM64 Windows

- **The ACPI node does not exist yet [V].** No `Gpu.asl`/Mali device exists in `edk2-rk3588/edk2-rockchip/Silicon/Rockchip/RK3588/AcpiTables/`
  (grep for mali/gpu/fb000000 found nothing). The edk2 owner must add something like the following **[I, values V from DT]**:
  ```asl
  Device (GPU0) {
    Name (_HID, "PFWG0610")   // placeholder; vendor IDs should be unique (PNPID registry) - pick a project prefix
    Name (_UID, 0)
    Name (_CCA, 0)            // RK3588 GPU is not IO-coherent (no dma-coherent in DT); edk2 already uses _CCA 0 widely
    Method (_CRS, 0, Serialized) {
      Name (RBUF, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0xFB000000, 0x00200000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 124 }  // SPI 92 "job"
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 125 }  // SPI 93 "mmu"
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 126 }  // SPI 94 "gpu"
      })
      Return (RBUF)
    }
  }
  ```
  DT source: `reg = <0x0 0xfb000000 0x0 0x200000>`, `interrupts = <GIC_SPI 92/93/94 IRQ_TYPE_LEVEL_HIGH>`,
  `interrupt-names = "job","mmu","gpu"`, `power-domains = <&power RK3588_PD_GPU>`, clock `SCMI_CLK_GPU`
  (`linux/arch/arm64/boot/dts/rockchip/rk3588-base.dtsi:449-465`) **[V]**. GSIV = SPI + 32 **[I, standard GIC]**.
  The edk2 idiom matches, e.g. `Dma.asl` uses `Memory32Fixed` + `Interrupt(ResourceConsumer, Level, ActiveHigh, Exclusive)` **[V]**.
- **Clocks and power:** edk2 exposes SCMI clock control as ASL methods `CLRG`/`CLRS` on `\_SB.SCMI` (`Scmi.asl`) **[V]**. A
  driver can call ACPI methods with `IOCTL_ACPI_EVAL_METHOD` (`acpiioct.h`, present in the mingw ddk dir) on its PDO, or edk2 can
  wrap them in a GPU `_DSM` or `_PS0/_PS3`. The RK3588 GPU power domain (PMU) must be on before we touch registers. Either the firmware
  leaves it on or `_PS0` turns it on **[?]** (edk2 owner).
- **KMDF resource flow [I, standard]:** `EvtDriverDeviceAdd` creates three `WDFINTERRUPT`s with `WdfInterruptCreate`; KMDF matches them
  to the interrupt resources in order (see `general/PLX9x5x/sys/IsrDpc.c:69`). `EvtDevicePrepareHardware(raw, translated)`
  walks `WdfCmResourceListGetDescriptor` for `CmResourceTypeMemory` and calls `MmMapIoSpaceEx(pa, len, PAGE_READWRITE|PAGE_NOCACHE)`.
  For register access, use `READ/WRITE_REGISTER_ULONG` or plain volatile accesses plus `__dmb`/`dsb` barriers. ISRs are level-triggered,
  so mask or clear the source (`*_IRQ_MASK`/`*_IRQ_CLEAR`) in the ISR and queue the DPC. For WDDM, `DxgkDdiInterruptRoutine` gets a
  MessageNumber. **[?]** Does dxgkrnl connect all three line interrupts of an ACPI device, or only one? If only one, the miniport
  could connect the others itself through `IoConnectInterruptEx` on the PDO.
- **DMA and coherency:** the device is non-coherent (`_CCA 0`). edk2-rk3588 already marks eMMC, GMAC, SPI, I2C, I2S and USB2 as `_CCA 0`
  (`grep _CCA` in `AcpiTables/*.asl`) **[V]**, and those devices work on Windows. So Windows ARM64 handles `_CCA=0` through the HAL DMA
  path **[I]**. Options:
  - `IoGetDmaAdapter`/`WdfDmaEnablerCreate` (DEVICE_DESCRIPTION v3, 64-bit, scatter/gather, bus master). `AllocateCommonBuffer` should
    return memory with suitable attributes for non-coherent devices **[I]**. Good for the firmware/MCU interface and ring buffers.
  - Bulk BOs: `MmAllocatePagesForMdlEx(low, high, skip, size, MmWriteCombined or MmNonCached, MM_ALLOCATE_FULLY_REQUIRED |
    MM_DONT_ZERO_ALLOCATION...)`. Then the kernel mapping is `MmGetSystemAddressForMdlSafe` / `MmMapLockedPagesSpecifyCache(KernelMode, same cache type)`,
    and the user mapping (panthor's mmap) is `MmMapLockedPagesSpecifyCache(mdl, UserMode, MmWriteCombined, NULL, FALSE, NormalPagePriority |
    MdlMappingNoExecute)` inside `__try`. It must be called in the context of the target process (IOCTL handler / `EvtIoInCallerContext`). Unmap at
    close/cleanup. Use the same cache type for every mapping of a page, because ARM64 forbids mismatched attributes **[I]**.
  - Cached CPU mappings + explicit cache maintenance: no public kernel API cleans a VA range except the DMA APIs
    (`FlushAdapterBuffersEx`). With clang, though, we can emit `dc cvac`/`dc civac` + `dsb sy` with inline asm. MSVC ARM64 has no inline asm.
    That favours keeping clang/mingw **[I]**.
  - GPU page tables live in WC/uncached memory, or are cleaned to PoC after CPU writes, because the Mali MMU table walks are
    non-coherent here **[I]**.
  - MMIO to user mode (CSF doorbells, if ever exposed): `MmMapIoSpaceEx` → `IoAllocateMdl` + `MmBuildMdlForNonPagedPool` →
    `MmMapLockedPagesSpecifyCache(UserMode, MmNonCached)`. It is documented but discouraged. Panthor rings doorbells from the
    kernel, so this is not needed initially **[I]**.
- **Firmware loading:** the CSF firmware (`mali_csffw.bin`) can be read from `\SystemRoot\System32\drivers\...` with
  `ZwCreateFile`/`ZwReadFile` at PASSIVE_LEVEL, or installed through the INF into the DriverStore, with its path read from the
  device's registry key **[I]**. The firmware sections must be placed at the fixed MCU VAs through AS0.

---

## 5. Risks and open questions

1. **ARM64 export list is approximate.** One wrong import and the driver does not load. Fix: gendef from the target's ntoskrnl (first hardware task).
2. **IRQL primitives on ARM64.** Inline vs exported is unknown. Avoid them, or resolve them dynamically until verified.
3. **mingw DDK coverage.** These headers are aimed at ReactOS/Wine-era APIs, and newer DDIs (ExAllocatePool2, MmMapIoSpaceEx, MmAllocatePagesForMdlEx
   flags, DMA v3) may be missing or wrong. Expect to keep a project `include/` with our own declarations.
   This stays manageable for KMDF + ~100 kernel APIs. For WDDM it is not manageable, so we need the WDK.
4. **WDK license.** For phase 2 the user must accept the WDK/SDK NuGet license. The binary/header redistribution terms limit
   what can be committed to the repo. Do not commit WDK headers.
5. **Test signing and Secure Boot on edk2-rk3588.** Check that Secure Boot can be disabled and that HVCI/VBS is off or compatible.
6. **Present performance of the GDI blit path.** Later options: a KMDOD for VOP2 with its own present path, or a WDDM display driver.
7. **Does the loader's D3DKMT path enumerate ComputeAccelerator adapters?** Does dxgkrnl load a render-only KMD with no D3D UMD? Both need hardware tests.
8. Multiple line interrupts under WDDM (see §4).

## 6. Sources

- MCDM overview / KMD guidelines: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/mcdm ,
  https://learn.microsoft.com/en-us/windows-hardware/drivers/display/mcdm-implementation-guidelines
- GpuMmu: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpummu-model ,
  https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_gpummucaps
- OpenGL ICD loading: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/loading-an-opengl-installable-client-driver
- Test signing: https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option
- FP in kernel: https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/using-floating-point-or-mmx-in-a-wdm-driver
- ACPI device objects: https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/device-management-namespace-objects
- D3D11_UMD_MODE: https://ntdoc.m417z.com/d3d11_umd_mode
- Vulkan loader: https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md , `loader/loader_windows.c` @15d99425
- MIT D3DKMT headers: https://github.com/microsoft/libdxg
- ReactOS WDDM/KMDF bits: https://github.com/reactos/reactos (`sdk/include/ddk/d3dkmddi.h`, `sdk/lib/drivers/wdf/`)
- WDK NuGet: https://api.nuget.org/v3-flatcontainer/microsoft.windows.wdk.arm64/index.json
