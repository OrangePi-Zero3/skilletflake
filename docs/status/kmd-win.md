# Status: kmd-win (toolchain + KMDF glue → panwin.sys)

Date: 2026-10-30. Owner key: `kmd-win`.
Owned paths: `toolchain/**`, `kmd/win/**`, `kmd/meson.build`, `kmd/meson_options.txt`,
`docs/bringup/windows-install.md`.

## Summary

- **`panwin.sys` builds on Linux** for ARM64 Windows with llvm-mingw + meson, with either core:
  - `-Dcore=stub`: 53 KiB
  - `-Dcore=real` (the kmd-core sources as of 01:22): 80 KiB

  Both builds compile with `-Wall -Wextra` plus extra warnings and produce **zero warnings**. An
  automatic check verifies the image format and that every import is in our `.def` files.
- **Nothing has run on hardware or on any Windows.** Everything below that depends on
  Windows/KMDF/RK3588 behaviour is **untested on hardware**.
- **`toolchain/`** is the maintained form of `references/toolchain-try`. It contains:
  - ARM64-patched mingw DDK headers, now also with inline `READ/WRITE_REGISTER_*` (the stock
    headers declare them as ntoskrnl imports, which do not exist on ARM64).
  - The MIT KMDF 1.15 headers and their LICENSE.
  - The `.def` files, a dependency-free PE tool (`petool.py`), the reviewed clean-room KMDF stub,
    a meson cross file, and the signing scripts.
- **`kmd/win/`** is the KMDF driver:
  - Device setup: ACPI resources (1 MMIO + 3 IRQs, in job/mmu/gpu order), an RK3588 PMU
    power-domain safety check, 3 `WDFINTERRUPT`s (ISR → `pw_irq_top`, passive work item →
    `pw_irq_bottom`), and two-phase core creation.
  - User interface: file contexts, a device interface plus `\DosDevices\PanWin0`, and IOCTL
    dispatch (`GET_INFO`, `DRM_IOCTL_VERSION`, `GET_CAP`, `PANTHOR_DEV_QUERY` GPU/CSIF,
    `DEBUG_REG_READ`).
  - `pw_os_win.c` implements every function in `pw_os.h`. There is also an INF and an install
    guide.

## What was built and how to build/test it

```sh
cd /home/e3xp0/Projects/panfrost-win/repo
meson setup build-kmd kmd --cross-file toolchain/meson/aarch64-w64-kmd.ini   # -Dcore=stub (default) | real
                                                                            # -Ddebug_ioctls=true  -Dbuildtype=release
ninja -C build-kmd            # build-kmd/panwin.sys, panwin.pdb (CodeView), panwin.map
meson test -C build-kmd       # 'image': petool check-sys; 'host': pw_fmt unit test (native gcc)
toolchain/sign/make-test-cert.sh            # openssl -> toolchain/sign/out/{key,crt,cer,pfx}
toolchain/sign/sign-driver.sh build-kmd/panwin.sys   # needs osslsigncode (not installed: exits 2 with instructions)
python3 -I toolchain/scripts/petool.py --help        # approx-defs | defs-from-pe | exports | check-sys
```

The default is `-Dcore=stub`, because the core was still being written. The lead may want to flip
the default to `real`; it builds.

Files:

| Path | What |
|---|---|
| `toolchain/include/ddk/{wdm,ntddk}.h` | mingw-w64 (ReactOS, public domain) plus the ARM64 arch block plus ARM64 MMIO inlines (`dmb oshst`/`oshld` ordering, as in Linux writel/readl) |
| `toolchain/include/wdf/kmdf/1.15/`, `include/wdf/LICENSE`, `include/wdfcase/` | Microsoft KMDF 1.15 headers (MIT) with 1 clang fix, plus case-fixing symlinks |
| `toolchain/patches/*.patch` | The exact diffs against upstream. `scripts/update-ddk-headers.sh` re-applies them (tested: regenerates byte-identical headers) |
| `toolchain/lib/{ntoskrnl,hal,wdfldr}.def`, `lib/ref/arm32-*.def` | Approximate ARM64 import lists, plus the ARM32 reference lists used for warnings |
| `toolchain/scripts/petool.py` | `.def` generation (approximate, or from real ARM64 PEs) and `.sys` verification |
| `toolchain/kmdf/fxstub.c` | Clean-room `FxDriverEntry`/`WdfVersionBind`/`Unbind` stub, reviewed (see below) |
| `toolchain/meson/aarch64-w64-kmd.ini` | Cross file (llvm path is a `[constants]` override) |
| `toolchain/sign/{make-test-cert,sign-driver}.sh` | Test certificate and Authenticode signing |
| `toolchain/README.md` | Flags, gotchas, the KMDF stub review, and the day-one `.def` regeneration procedure |
| `kmd/meson.build`, `kmd/meson_options.txt` | Project: options `core`, `debug_ioctls`, `host_tests`; links with lld-link directly |
| `kmd/win/pw_win_driver.c` | DriverEntry, DeviceAdd, PnP/power callbacks, interrupts, registry parameters, PMU check |
| `kmd/win/pw_win_ioctl.c` | File objects, the in-caller-context stage, the sequential queue, DRM emulation, errno→NTSTATUS |
| `kmd/win/pw_win_user.c` | `copy_to/from_user` with `ProbeForRead/Write` under `__try` |
| `kmd/win/pw_os_win.c` | The `pw_os.h` implementation |
| `kmd/win/pw_fmt.{c,h}` | Freestanding C99 `vsnprintf` subset, used by logging (`pw_win_vsnprintf`) |
| `kmd/win/pw_core_stub.c` | `pw_core.h` stub, including the kmd-core additions (alloc/probe/fw_booted/fw_ping/mmu_info) |
| `kmd/win/panwin.inf` | `ACPI\RKCP7410` (and `ACPI\VEN_RKCP&DEV_7410`), KMDF 1.15, firmware copy, parameters, SDDL |
| `kmd/win/tests/test_fmt.c` | Host test that compares the formatter against glibc `snprintf` |

Key design points (details in source comments):

- **Core lifetime.** KMDF connects interrupts only after `EvtDeviceD0Entry`, and the core waits
  for IRQ-driven events while it probes. So the steps are:
  - `D0Entry`: PMU check, then mask all 3 `*_INT_MASK` registers.
  - `D0EntryPostInterruptsEnabled`: `pw_device_alloc`, publish the device to the ISR, then
    `pw_device_probe`.
  - `D0Exit`, which runs after KMDF has disconnected the interrupts and flushed the interrupt work
    items: `pw_device_destroy`.

  If probe fails, the device stays published until D0Exit, but ioctls do not use it. The PnP
  device stays started either way, so `GET_INFO` can report the failure. This deviates from the
  brief's "D0Entry → pw_device_create". The reason is that IRQs must be live during probe.
- **ISR safety net.** If the core does not claim an interrupt and the line's `INT_STAT` is
  non-zero, the glue masks that line and claims it. It also counts these events. Returning FALSE
  for an asserted level-triggered line would storm and hang the machine.
- **RK3588 PMU check** (registry `Rk3588PmuCheck`, default 1). Before the first GPU access the
  driver maps PMU2 at 0xFD8D8000 and requires:
  - `BISR_STATUS4` (offset 0x290) bit 1 set;
  - `BUS_IDLE_ST0` (0x120) bit 0 clear;
  - `BUS_IDLE_ACK0` (0x118) bit 0 clear.

  This is the same test as the edk2 patch's `GPON`. If it fails, no GPU register is touched.
  `pw_os_reg_*` returns `0xFFFFFFFF` or ignores the access while `regs_enabled == 0`, and it
  bounds-checks every offset.
- **IOCTLs.** The ioctl struct size and direction come from the Linux ioctl number (`_IOC_SIZE`,
  `_IOC_DIR`), as in drm_ioctl: a short struct is zero-extended and `min()` bytes are copied back.
  The handling is split in two stages:
  - In the caller's context: `DRM_IOCTL_VERSION` is answered on the spot (constant data, copied
    with `ProbeForWrite`+`memcpy` under `__try`; 32-bit callers are refused). For `DEV_QUERY`,
    the output object (`pointer`/`size`, at most 64 KiB) is locked with
    `WdfRequestProbeAndLockUserBufferForWrite`.
  - Everything else is queued on a sequential queue that runs at passive level.
- **DEV_QUERY** follows panthor exactly:
  - `pointer == 0` returns the size; GPU_INFO is 112 bytes and CSIF_INFO is 24.
  - A user size below the uAPI-1.0 minimum (`offsetofend(tiler_present)` / `(pad)`) gives
    -EINVAL.
  - Otherwise the driver copies `min(user, kernel)` bytes and zero-fills a larger user object.
  - Other query types give -EINVAL, as panthor 1.0 does.
- **DRM version.** The driver reports **`panthor` 1.0.0**, date "0" (Linux ≥ 6.14 behaviour). The
  1.0 behaviour (GPU_INFO, CSIF_INFO, and the flush-ID mmap) needs no version gate in Mesa
  `panthor_kmod.c`. Every later minor turns on something that is not implemented here, so the
  version stays at 1.0:

  | Minor | Gated feature |
  |---|---|
  | 1.1 | TIMESTAMP_INFO |
  | 1.2 | GROUP_PRIORITIES |
  | 1.4 | BO_SET_LABEL |
  | 1.5 | SET_USER_MMIO_OFFSET |
  | 1.6 | timestamp_device_coherent |
  | 1.7 | BO_QUERY_INFO, WB_MMAP |
  | 1.9 | MMU_INFO, MAP_SPARSE |

  Bump `PW_DRM_VERSION_MINOR` in `pw_win.h` as these land.
- **GET_CAP** follows Linux `drm_getcap` for a render-only driver:
  - TIMESTAMP_MONOTONIC = 1.
  - PRIME, SYNCOBJ and SYNCOBJ_TIMELINE = 0 (not implemented yet).
  - Anything else gives -EOPNOTSUPP.
- **errno→NTSTATUS** (`pw_win_status_from_errno`; libdrm-win already inverts this table):

  | errno | NTSTATUS |
  |---|---|
  | EPERM | ACCESS_DENIED |
  | ENOENT | NOT_FOUND |
  | EIO | IO_DEVICE_ERROR |
  | ENOMEM | INSUFFICIENT_RESOURCES |
  | EFAULT | ACCESS_VIOLATION |
  | EBUSY | DEVICE_BUSY (a warning-class code; Information=0) |
  | ENODEV | DEVICE_NOT_READY |
  | EINVAL | INVALID_PARAMETER |
  | ENOSPC | QUOTA_EXCEEDED |
  | ERANGE | INTEGER_OVERFLOW |
  | ENOSYS | NOT_IMPLEMENTED |
  | EOPNOTSUPP | NOT_SUPPORTED |
  | ETIMEDOUT | IO_TIMEOUT |

  An unknown ioctl gives NOT_SUPPORTED.
- **Imports.**
  - **Static imports (40 total):** WDFLDR.SYS `WdfVersionBind`/`Unbind`; hal.dll
    `KeQueryPerformanceCounter`, `KeStallExecutionProcessor`; and 36 from ntoskrnl.exe.
  - **Resolved at run time** with `MmGetSystemRoutineAddress`, because they are absent from both
    mingw lists:
    - `ExAllocatePool2` (falls back to `ExAllocatePoolWithTag(NonPagedPoolNx)`+zero);
    - `MmMapIoSpaceEx` (falls back to `MmMapIoSpace(MmNonCached)`).
  - **Never called:** the IRQL and current-thread helpers.
- **Memory** (`pw_os_mem_alloc`):
  - Allocation: `MmAllocatePagesForMdlEx(0..2^40-1, MM_ALLOCATE_FULLY_REQUIRED)`, or
    `MmAllocateContiguousNodeMemory` for `PW_MEM_CONTIGUOUS`.
  - Caching: the cache type (MmCached, or MmWriteCombined for `PW_MEM_UNCACHED`) is fixed at
    allocation and reused for every mapping, because ARM64 forbids mismatched aliases.
  - Kernel mapping: `MmMapLockedPagesSpecifyCache(..., NormalPagePriority|MdlMappingNoExecute)`.
  - Cache maintenance: `dc cvac` (flush) or `dc civac` (invalidate) loops with the line size from
    `CTR_EL0.DminLine`, then `dsb sy`. WC memory only needs the `dsb`. Pages without a kernel
    mapping get a temporary one for maintenance. Fresh cached allocations are cleaned once, so
    the GPU sees zeros.
- **Locks.** `FAST_MUTEX` with `KeEnterCriticalRegion` + `ExAcquireFastMutexUnsafe`, so the holder
  stays at PASSIVE_LEVEL and file I/O under the lock (firmware loading) is legal.
  **Events** are notification `KEVENT`s; a timeout of `0xFFFFFFFF` means infinite, and
  `STATUS_TIMEOUT` is handled explicitly (it is a success code).
- **Firmware:**
  - Path: `FirmwareRoot` (registry, default `\SystemRoot\System32\drivers\panwin\`) + name, with
    `/`→`\`.
  - Rejected names: absolute, `..`, `:`, non-ASCII.
  - Read with `ZwCreateFile`/`ZwQueryInformationFile`/`ZwReadFile` into nonpaged pool, at most
    64 MiB.
  - The INF installs it at the matching location, `drivers\panwin\arm\mali\arch10.8\mali_csffw.bin`.
- **KMDF stub review** (full notes in toolchain/README.md):
  - The layout matches MIT `fxldr.h`, and the built image's `.data` was checked: `Size` 48,
    `L"KmdfLibrary"`, version 1.15.0, `FuncCount` 444, `FuncTable=&WdfFunctions_01015`.
  - It follows the loader contract in `fxlibrarycommon.cpp`: with >1.13 the loader writes the
    table pointer into `*FuncTable`.
  - Unload displacement is gated on `DisplaceDriverUnload`.
  - Fixed relative to the experiment: leaks on failure paths, globals not cleared after unbind, and
    `DriverEntry(NULL)` being called.

## Verified (actually run here)

- **Builds.** All are warnings-clean under `-Wall -Wextra -Wmissing-prototypes -Wstrict-prototypes
  -Wshadow -Wvla -Wframe-larger-than=2048` (clean rebuild, `grep -i warning` empty):
  - core=stub in the default `debugoptimized` buildtype;
  - core=stub in `release` with `-Ddebug_ioctls=true`;
  - core=real, against kmd/core as of this session. `subdir('core')` works with the core owner's
    `meson.build`.

  The first core=real link failed: kmd-core also defines `pw_snprintf`/`pw_vsnprintf`, so I
  renamed mine `pw_win_*`. After that it built and passed `meson test`.
- **`meson test`: 2/2 OK** in all three build dirs:
  - `petool check-sys`: ARM64, NATIVE subsystem, WDM_DRIVER (dllchars 0x2160), exception table
    present, non-zero checksum, sections 4 KiB-aligned. Imports come only from
    ntoskrnl.exe/hal.dll/WDFLDR.SYS, all are in the `.def` files, and none is x64-only.
  - The pw_fmt host test: 87 checks against glibc, 0 failures. It was also run under clang
    ASan+UBSan, which reported nothing.
- **llvm-readobj cross-check of the stub build:**
  - `Machine: IMAGE_FILE_MACHINE_ARM64`, `Subsystem: IMAGE_SUBSYSTEM_NATIVE`,
    `IMAGE_DLL_CHARACTERISTICS_WDM_DRIVER`, NX/DYNAMIC_BASE/HIGH_ENTROPY_VA.
  - `ExceptionTableSize 0x150`.
  - Sections `.text .rdata .data .pdata .reloc`. A duplicate read-only `.data` (from the WDF
    context type info) was fixed with `-DWDF_TYPE_DEFAULT_SECTION_NAME=".rdata"`.
  - Every one of the 40 imports from `llvm-readobj --coff-imports` was grepped in the `.def`
    files: 0 missing.
- **SEH.** `pw_win_copy_to_user` has an `.xdata` `ExceptionHandler` pointing at the
  `__C_specific_handler` import thunk (scope table). Disassembly shows `ProbeForWrite` and
  `memcpy` as real calls inside the `__try` region, and the `__except` path returns
  0xC0000005.
- **No x18 use** in the image (`llvm-objdump -d | grep x18` = 0). The PDB is generated, and the
  debug directory names `panwin.pdb`.
- **`petool.py`:**
  - On Wine's x86_64 `ntoskrnl.exe`, `exports` matches `llvm-readobj --coff-exports` and `gendef`
    exactly (1673 names).
  - `defs-from-pe` refuses non-ARM64 images without `--any-machine`. With it, it writes `.def`
    files that `llvm-dlltool -m arm64` accepts.
  - `check-sys` exits 1 with clear errors on wrong-machine images or missing `.def` entries.
  - `approx-defs` regenerates the same name set as toolchain-try (2487/116).
- **Signing.** `make-test-cert.sh` produces key/crt/cer/pfx; `openssl pkcs12` reads the pfx, and
  `PFX_LEGACY=1` was also tested. `sign-driver.sh` exits 2 with instructions because osslsigncode
  is missing.

## Not verified / needs hardware

These are untested on hardware. This includes loading at all, `WdfVersionBind` succeeding with our
stub, the KMDF interrupt work-item behaviour, ACPI resource order, the PMU check, MMIO access
ordering, `MmMapIoSpaceEx`/`MmAllocateContiguousNodeMemory` on ARM64, cache maintenance
correctness, firmware file loading, the INF/catalog/pnputil flow, and the SDDL. Specific open
points:

- **The ARM64 export list is approximate.** Any wrong name means the driver will not load (code
  39). The hal.dll imports (`KeQueryPerformanceCounter`, `KeStallExecutionProcessor`) are
  plausible but unconfirmed for ARM64.
- **`MmWriteCombined` on ARM64:** whether it is Normal-NC (expected) or Device memory.
- **Clang SEH on aarch64-w64-mingw32** has not been exercised at run time.
- **`WDF_TYPE_DEFAULT_SECTION_NAME=".rdata"`** assumes KMDF never writes the const type info. This
  is true for 1.15 headers (the struct is `const`).
- **Windows-side steps in windows-install.md** that I could not try are marked [unverified]: UEFI
  `mm` syntax, DebugView ARM64, kdcom/KDNET on RK3588, and New-FileCatalog with pnputil.

## Known issues & TODO (prioritised)

1. **Day one on hardware:** regenerate the `.def` files from the board's ntoskrnl/hal/WDFLDR
   (`petool.py defs-from-pe`, procedure in toolchain/README.md), and settle whether ARM64 exports
   `KeGetCurrentIrql`.
2. **Timer resolution.** `pw_os_event_wait(e, 1)` and `pw_os_msleep(1)` sleep up to about 15.6 ms
   at the default Windows timer resolution, and the core uses 1 ms waits in polling loops. That is
   correct but slow. Fix with `ExAllocateTimer(EX_TIMER_HIGH_RESOLUTION)` waits or
   `ExSetTimerResolution` while the GPU is busy.
3. **Missing ioctls:** `MMAP`/`MUNMAP` (user mapping with `MmMapLockedPagesSpecifyCache(UserMode)`
   in caller context, torn down in `EvtFileCleanup`), the flush-ID page (unconditional in Mesa),
   syncobjs/`SYNCOBJ_EVENT`, and the remaining panthor ioctls as the core exposes them. Each one
   with nested arrays goes through the caller-context stage (`pw_win_copy_from_user` is ready).
4. **Optional ACPI power check:** use the edk2 `_DSM` function 4 (`IOCTL_ACPI_EVAL_METHOD`) for the
   power check instead of mapping PMU2 ourselves.
5. **Signing tools:** osslsigncode is not installed and there is no pacman package in the
   configured repos (AUR `osslsigncode`, or build from github.com/mtrojnar/osslsigncode with
   cmake+openssl+curl). Catalog creation must happen on Windows.
6. **Cache maintenance cost:** non-KMAP cached `pw_os_mem` flush/invalidate maps and unmaps on
   every call. Consider caching the mapping, or require `PW_MEM_KMAP` for CPU-maintained buffers.
7. **No runtime PM / D3 path:** the device is kept in D0. The edk2 `_PS3` is never exercised by
   us.
8. **WOW64 callers:** they are refused only for `DRM_IOCTL_VERSION` (pointer-size struct); the
   other structs are fixed-layout.
9. **No x86_64 build target** for a Wine `winedevice.exe` smoke test (suggested in the research
   note).

## Requested contract changes

None of these were edited. I worked around each of them.

1. **`pw_os.h`:**
   - Document `pw_os_event_wait(…, 0xFFFFFFFF)` = infinite (that is what pw_os_win.c does).
   - Say explicitly which functions are ISR-safe. On Windows, `pw_os_event_signal` (`KeSetEvent`)
     is **not** callable from the ISR (DIRQL), only from the passive bottom half. Today only
     reg/time/udelay/log are ISR-safe.
2. **`pw_os.h`:** the time base is QPC (`KeQueryPerformanceCounter`, ns = QPC·1e9/QPF). This
   matches the libdrm-win proposal for syncobj absolute timeouts; consider documenting it.
3. **`panwin_ioctl.h`:** it mentions a trailing `panwin_ioctl_status` that does not exist. I
   support libdrm-win's proposal `PANWIN_NTSTATUS_FROM_LINUX_ERRNO(e)` (0xED52xxxx). Once it is in
   the header, switching `pw_win_status_from_errno` is a one-line change. Until then I complete
   requests with the standard NTSTATUS codes in the table above.
4. **`pw_core.h`:**
   - The core exports generic global names (`pw_snprintf`, `pw_vsnprintf` from `pw_util.c`) that
     collided with mine. Either reserve the `pw_` prefix for core or move to `pw_core_*`. Glue
     symbols are now `pw_win_*`.
   - It would help if the core reported which panthor uAPI minor it implements (e.g.
     `pw_core_uapi_minor()`), so the emulated DRM version follows the core instead of a glue
     constant.
5. **`pw_core.h` (done by kmd-core in this session):** two-phase `pw_device_alloc`/`probe`, so
   the ISR can see the device during probe. The glue and the stub use it.

## Review

Independent review, 2026-10-30. Nothing here has run on hardware or on Windows. The review covers
the build, static inspection of the image, and reading the code against the references (KMDF
source in `references/Windows-Driver-Frameworks`, Linux panthor/pmdomain/DT, Mesa
`panthor_kmod.c`, libdrm-win `errno_map.c`, and the edk2 patch).

### Re-run (fresh build dirs, commands from this file)

| What | Result |
|---|---|
| `meson setup` + `ninja` with `core=stub` (debugoptimized), `core=real`, and `release -Ddebug_ioctls=true` | all 3 build, 0 warnings (`grep -ci warning` = 0) |
| `meson test` | 2/2 OK in each |
| Rebuild after the review fixes | all 3: 0 warnings, 2/2 OK |
| `llvm-readobj` (core=real) | ARM64, `IMAGE_SUBSYSTEM_NATIVE` 10.0, WDM_DRIVER\|NX\|DYNAMIC_BASE\|HIGH_ENTROPY_VA, checksum set |
| Sections | `.text` RX, `.rdata` R, `.data` RW, `.pdata`, `.reloc`; no RWX section |
| Imports | 40, from ntoskrnl.exe (36), hal.dll (2) and WDFLDR.SYS (2), all in the .def files |
| KMDF bind info, decoded from the built image | Size 48, `L"KmdfLibrary"`, 1.15.0, FuncCount 444 (= `WdfFunctionTableNumEntries_V1_15` in `fxlibrarycommon.cpp`), FuncTable = `&WdfFunctions_01015` |
| SEH | `pw_win_copy_to/from_user` carry `.xdata` with `__C_specific_handler` |
| `toolchain/patches/ddk-arm64.patch` on the llvm-mingw headers | reproduces `toolchain/include/ddk/{wdm,ntddk}.h` byte for byte |
| `pw_fmt` host test under gcc ASan+UBSan | 87 checks, 0 failures |
| `petool exports` on Wine's ntoskrnl.exe vs `llvm-readobj --coff-exports` | 1673 names in both; only the display of the 3 forwarders differs |
| `petool defs-from-pe --any-machine` | works |
| `make-test-cert.sh` | creates the cert, EKU codeSigning |
| `sign-driver.sh` | exits 2 with the documented osslsigncode message |

**Claims that held up:**

- **Deviation 1 (core start-up in D0EntryPostInterruptsEnabled).** KMDF calls `EvtDeviceD0Entry`,
  then `NotifyResourceObjectsD0` (which connects the interrupts), then
  `EvtDeviceD0EntryPostInterruptsEnabled` (`powerstatemachine.cpp:2263/2312/2321`). On the way
  down, `PowerGotoDxIoStoppedCommon` disconnects first, and `FxInterrupt::Disconnect` runs
  `FlushQueuedWorkitem()`, before `EvtDeviceD0Exit`. So destroying the core in D0Exit is safe.
- **Interrupt resources are handed out in creation order.** `m_InterruptListHead` uses
  InsertTailList, and `PnpMatchResources` walks it in that order. Creating the objects in
  job/mmu/gpu order is therefore correct.
- **Interrupt work items can be requeued while their callback is still running.**
  `FxSystemWorkItem::WorkItemHandler` clears `m_Enqueued` before it calls the callback, so no
  wakeup is lost under the core's mask-in-top / unmask-in-bottom scheme.
- **Register offsets** match panthor and the RK3588 DT/pmdomain:
  - interrupt blocks at 0x1000 (job), 0x2000 (mmu) and 0x20 (gpu), each with
    RAWSTAT/CLEAR/MASK/STAT at +0/+4/+8/+C;
  - the stub's GPU_ID/feature/PRESENT registers;
  - the PMU2 checks (0xFD8D8000: 0x118/0x120 bit 0, and 0x290 bit 1), which match Linux
    `rk3588_pmu` and the edk2 `GPON` method;
  - SPI 92/93/94 = GSIV 124/125/126, level-high.
- **DRM semantics:**
  - DEV_QUERY follows panthor: `pointer == 0` returns the size, the minimum sizes come from
    `PANTHOR_UOBJ_DECL`, and a larger user object is zero-filled.
  - `drm_copy_field` and `drm_getcap` for a render-only driver match Linux.
  - Version 1.0 is consistent with the gates in Mesa `panthor_kmod.c` (1.1, 1.2, 1.5, 1.9 and so
    on). As the report says, Mesa still fails on the ungated flush-ID mmap.
- **Windows API usage.** Argument order and IRQL use are correct for: MmAllocatePagesForMdlEx,
  MmMapLockedPagesSpecifyCache, MmAllocateContiguousNodeMemory, ZwCreateFile/ZwReadFile,
  KeWaitForSingleObject (STATUS_TIMEOUT handled), the FAST_MUTEX "Unsafe" pattern inside a
  critical region, WdfRequestProbeAndLockUserBufferForWrite (caller context only),
  ProbeForRead/Write under `__try`, and DbgPrintEx (no `%ws`).
- **INF.**
  - The model decoration and models section match.
  - `.HW` AddReg holds Security/DeviceCharacteristics (0x100 = FILE_DEVICE_SECURE_OPEN) and the
    Device Parameters that `PLUGPLAY_REGKEY_DEVICE` reads.
  - The firmware destination is `12,panwin\arm\mali\arch10.8`, which matches the default
    FirmwareRoot plus the core's derived name.
- **License hygiene.**
  - The DDK headers are public domain, and the WDF headers are MIT with their LICENSE.
  - No GPL text is in the owned files, and every SPDX tag is MIT.
  - fxstub.c is a clean-room implementation of the MIT `fxldr.h` contract.

### Bugs found and fixed (all minor; no correctness bug found in the main paths)

1. **`pw_reg_string` (pw_win_driver.c).** If the first registry key's query failed after writing
   partial data (for example, a value too long for the buffer), the caller could take
   `fw_name_w[0]` and end up with a 1-character garbage `FirmwareName`. The buffer is now cleared
   when no key returns the value.
2. **`petool.py write_def`.** It crashed with FileNotFoundError when `--out` did not exist yet. It
   now creates the directory.
3. **windows-install.md.** The expected log line said "PD_GPU on", but the driver prints
   "-> GPU on". Corrected.

### Remaining concerns (by severity)

1. **High, needs hardware: the import list is approximate.** `KeQueryPerformanceCounter` and
   `KeStallExecutionProcessor` are imported from hal.dll, and
   `ExAcquire/ReleaseFastMutexUnsafe`, `KeEnter/LeaveCriticalRegion` and `ExFreePool` are
   assumed to be ARM64 ntoskrnl exports. Regenerating the .def files on day one is mandatory. A
   wrong guess shows up as Code 39 / STATUS_PROCEDURE_NOT_FOUND.
2. **Medium: no ISR safety net while the GPU is unpowered.** If the PMU check fails, the
   interrupts are still connected, and `regs_enabled == 0` makes the ISR return FALSE. A line
   that is asserted while PD_GPU is off would then storm. It is unlikely (isolation clamps the
   outputs) but unguarded. Options are to fail D0Entry, or to not connect the interrupts
   (`WdfInterruptDisable` / report inactive) when the GPU is off.
3. **Medium, unverified: ACPI `_CRS` order.** That Windows keeps the `_CRS` order in the
   translated resource list is assumed, not shown. The driver logs vector/level per line; check
   it on day one.
4. **Low: the PMU check maps a physical page the device does not own.** It maps PMU2 with
   MmMapIoSpace. Driver Verifier/HVCI behaviour is unverified, and the `_DSM` alternative
   (TODO 4) avoids it.
5. **Low: errno round trip is lossy.** EPERM comes back as EACCES, ERANGE as EOVERFLOW, and
   ETIMEDOUT as ETIME, because the glue table and libdrm-win's `errno_map.c` are not inverses.
   The errno-in-NTSTATUS contract change would fix this.
6. **Low: DEV_QUERY rejects a user object larger than 64 KiB with EINVAL.** panthor would accept
   it. No current struct comes close.
7. **Low: debug builds expose raw register reads to every user.** The SDDL grants Everyone
   GRGW, so with `-Ddebug_ioctls=true` any user can read GPU registers. This is acceptable for
   bring-up only.
8. Already listed by the author: timer resolution, the missing MMAP/flush-ID/syncobj ioctls,
   signing tools, and the per-call temporary mapping for cache maintenance.

## M3-M5 update

2026-10-30, key `glue`. The full report is in [glue.md](glue.md). Nothing has run on hardware.

- **New `pw_os.h` functions:**
  - `pw_os_event_wait_user`: UserMode alertable `KeWaitForMultipleObjects` on the core event
    plus the file's abort event; QPC deadline → relative timeout.
  - `pw_os_copy_{from,to}_user`: ProbeFor* + memcpy under `__try`; the scope tables were checked
    with the new `toolchain/scripts/seh_scopes.py`.
  - `pw_os_work_*`: IO_WORKITEM + KTIMER/KDPC.
  - `timer_hz` comes from CNTFRQ_EL0.
- **Ioctls** are all executed and completed in `EvtIoInCallerContext`, with no sequential queue
  any more, so a WAIT cannot deadlock a SIGNAL.
  - Every DRM ioctl → `pw_file_ioctl`. Errors → `PANWIN_STATUS_FROM_ERRNO`.
  - The glue's VERSION/GET_CAP/DEV_QUERY are gone (the core and `pw_core_stub.c` do them).
  - GET_INFO and DEBUG_REG_READ are kept.
  - MMAP/MUNMAP: partial MDL of the BO's pages, or an MDL over our MMIO mapping, mapped with
    `MmMapLockedPagesSpecifyCache(UserMode)` + `MdlMappingNoWrite` for read-only.
  - SYNCOBJ_EVENT: `ObReferenceObjectByHandle` on the caller's event.
- **Files:** each has a `pw_file` with rundown protection. Cleanup and
  `EvtDeviceD0ExitPreInterruptsDisabled` tear down waits, notifies and mappings, then call
  `pw_file_close`.
- **Superseded parts of this file:**
  - The errno→NTSTATUS table: all failures are now the errno-carrying status.
  - The "sequential queue" design.
  - The DEV_QUERY/GET_CAP/VERSION notes: that logic now lives in the core and the stub.
  - TODO 3 (missing MMAP/SYNCOBJ ioctls): done in the glue.
- **Builds:** core=stub (57 KiB), core=real (108 KiB), and release+debug_ioctls build with 0
  warnings, and `meson test` passes 2/2 in each.
- **Imports:** 69 in total, all in the existing `.def`s, so no `.def` change was needed.
