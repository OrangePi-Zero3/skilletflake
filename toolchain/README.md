# toolchain/ — building ARM64 Windows kernel drivers on Linux

Everything needed to build `panwin.sys` with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
(`/opt/llvm-mingw`, clang 23) and meson, without the WDK. Grown out of the
`references/toolchain-try` experiment (docs/research/windows-driver-model.md §2).
**Nothing built with it has been loaded on real ARM64 Windows yet.**

```
include/ddk/          wdm.h, ntddk.h: mingw-w64 (ReactOS DDK, public domain) + ARM64 arch block
include/wdf/kmdf/1.15 Microsoft KMDF 1.15 public headers (MIT, include/wdf/LICENSE), 1 clang fix
include/wdfcase/      case-fixing symlinks (the KMDF headers include e.g. "WdfQueryInterface.h")
patches/              the exact diffs of the two header sets above vs. their upstream copies
lib/*.def             import-library definitions: ntoskrnl.exe, hal.dll, WDFLDR.SYS
lib/ref/              mingw-w64 libarm32 defs (gendef of real ARM32 binaries), used for warnings
kmdf/fxstub.c         clean-room KMDF client stub (FxDriverEntry → WdfVersionBind → DriverEntry)
meson/aarch64-w64-kmd.ini  meson cross file for the kernel target
scripts/petool.py     .def generation (approximate / from real PE files) + .sys verification
scripts/update-ddk-headers.sh  re-apply patches/ddk-arm64.patch to a new llvm-mingw
sign/                 self-signed test certificate (openssl) + Authenticode signing (osslsigncode)
```

## Build

```sh
meson setup build-kmd kmd --cross-file toolchain/meson/aarch64-w64-kmd.ini   # -Dcore=real|stub
ninja -C build-kmd          # build-kmd/panwin.sys, panwin.pdb, panwin.map
meson test -C build-kmd     # petool check-sys + host unit tests
```

Key compiler flags (see `kmd/meson.build` for the full list and why):
`-ffreestanding -fno-builtin -fno-stack-protector -funwind-tables -fno-auto-import -fms-extensions
-D_KERNEL_MODE -DNTDDI_VERSION=0x0A000000`. `-funwind-tables` is mandatory: with
`-ffreestanding`, aarch64-w64-mingw32 clang emits no `.pdata/.xdata` otherwise. x18 is reserved by
clang on `*-windows` targets (the ARM64 kernel keeps the KPCR there).

Link: `lld-link /machine:arm64 /driver /driver:wdm /subsystem:native,10.0 /entry:FxDriverEntry
/nodefaultlib /release /tsaware:no /dynamicbase /nxcompat /highentropyva /filealign:0x1000
/align:0x1000 /opt:ref /opt:icf /debug:full` + the three import libraries.

## ARM64 header shim (include/ddk)

The stock mingw-w64 `wdm.h`/`ntddk.h` stop with `#error Unknown Architecture` on aarch64. The patch
adds an `__aarch64__` block (IRQL levels, `PAGE_SIZE`, `KFLOATING_SAVE`, `YieldProcessor`, …) and
inline `READ/WRITE_REGISTER_{UCHAR,USHORT,ULONG,ULONG64}` (+ `_BUFFER_`) with Linux `readl/writel`
ordering (`dmb oshst` before a store, `dmb oshld` after a load): the mingw headers would otherwise
declare them as ntoskrnl imports, which do not exist on ARM64.

**Do not call `KeGetCurrentIrql`, `KfRaiseIrql`, `KeLowerIrql`, `KeGetCurrentThread`.** They are
declared as imports so the headers compile, but the real ARM64 WDK most likely inlines them (Wine's
ntoskrnl spec lists them as x86_64-only). `petool.py check-sys` rejects them.

Upgrading llvm-mingw: `toolchain/scripts/update-ddk-headers.sh /path/to/llvm-mingw`, then rebuild.

## KMDF

`include/wdf/kmdf/1.15` is copied from microsoft/Windows-Driver-Frameworks @ b6191d9
(`src/publicinc/wdf/kmdf/1.15`). One change (patches/wdf-1.15-clang.patch): the `WDF_REQUEST_TYPE`
enum is defined in `wdfdevice.h` instead of being forward-declared there, because C (clang) rejects
an incomplete enum type as a parameter where MSVC accepts it.

KMDF 1.15 is the target on purpose: every Windows 10/11 release ships KMDF ≥ 1.15, and the v1 bind
info is enough (1.25+ headers need the v2 bind info with structure tables). The INF declares
`KmdfLibraryVersion = 1.15`. Build with `-DWDF_TYPE_DEFAULT_SECTION_NAME=".rdata"` (done in
kmd/meson.build) so `WDF_DECLARE_CONTEXT_TYPE` type info lands in `.rdata` instead of a second,
read-only `.data` section.

### fxstub.c (replaces WdfDriverEntry.lib)

Reviewed against Microsoft's MIT sources (`fxldr.h`, `fxlibrarycommon.cpp`, `fxdriverapi.cpp`):

- `WDF_BIND_INFO` v1: `{Size=48, Component=L"KmdfLibrary", {1,15,0}, FuncCount=444,
  FuncTable=&WdfFunctions_01015, Module}`. For FuncCount > the 1.13 table size the loader writes the
  table **pointer** into `*FuncTable` (so `FuncTable` must point at the `WdfFunctions` variable,
  not at an array), and FuncCount must exactly equal a released table size (444 for 1.15). Checked
  in the built image: `.data` holds `30 00 00 00 … 01 0f 00 bc 01 …` (Size 0x30, 1.15, 444).
- `WdfVersionBind(DriverObject, RegistryPath, &bind, (PVOID*)&WdfDriverGlobals)`, then
  `DriverEntry`, then — only if `WdfDriverGlobals->DisplaceDriverUnload` — save the framework's
  `DriverUnload` and interpose `fxstub_driver_unload`, which calls it and then
  `WdfVersionUnbind(RegistryPath, &bind, WdfDriverGlobals)`.
- Fixes vs. the experiment: registry path copy is freed on every failure path; `WdfDriverGlobals`
  is cleared after unbind; `FxDriverEntry(NULL, …)` returns `STATUS_INVALID_PARAMETER` instead of
  calling `DriverEntry(NULL)`; the copy uses NonPagedPoolNx.
- Not implemented: class-extension binding (`WdfVersionBindClass`), bind info v2, `/GS` cookie
  init (we build with `-fno-stack-protector`).

## Import libraries (lib/*.def) — APPROXIMATE until day one on hardware

mingw-w64 has no ARM64 `ntoskrnl.def`. `ntoskrnl.def`/`hal.def` are the union of mingw-w64's
`lib64` (x64) and `libarm32` (gendef of a real ARM32 Windows) lists — 2487 + 116 names.
`wdfldr.def` is hand-written from `fxldr.h`. A name that is in the .def but not in the real ARM64
export table makes the driver fail to load (STATUS_PROCEDURE_NOT_FOUND / error 127), so:

- `petool.py check-sys` (run by `meson test`) fails if the image imports anything outside the three
  DLLs or outside the .def files, or one of the forbidden IRQL/register functions, and warns about
  imports only known from x64 (not in `lib/ref/arm32-*.def`).
- Windows 10+ APIs that are in neither list (`ExAllocatePool2`, `MmMapIoSpaceEx`) are resolved with
  `MmGetSystemRoutineAddress` at run time, with fallbacks.

Regenerate the approximation: `python3 -I toolchain/scripts/petool.py approx-defs --mingw-crt
references/mingw-w64/mingw-w64-crt --out toolchain/lib`.

### Day one on the board: real export lists

1. On the RK3588 Windows install, copy `C:\Windows\System32\ntoskrnl.exe`,
   `C:\Windows\System32\hal.dll` and `C:\Windows\System32\drivers\WDFLDR.SYS` to the Linux host
   (also keep `drivers\Wdf01000.sys` to record the KMDF version: file properties → version).
2. Generate the .def files (machine is checked to be ARM64; DATA flags are carried over):
   ```sh
   python3 -I toolchain/scripts/petool.py defs-from-pe \
       --pe ntoskrnl.exe --pe hal.dll --pe WDFLDR.SYS \
       --data-from toolchain/lib/ntoskrnl.def --out toolchain/lib
   ```
   Cross-check with the LLVM tools: `llvm-readobj --coff-exports ntoskrnl.exe | grep -c Name:` must
   equal the count petool prints, and `/opt/llvm-mingw/bin/gendef ntoskrnl.exe` produces an
   equivalent list (gendef also guesses DATA exports).
3. `ninja -C build-kmd && meson test -C build-kmd`. Any import that disappeared is now an error.
   On Windows 11 24H2 hal.dll is mostly forwarders to ntoskrnl; importing through it still works.
4. Commit the new `.def` files with the Windows build number in the commit message
   (`ver` / `winver` on the device).

## Signing (test signing only)

```sh
toolchain/sign/make-test-cert.sh                        # -> toolchain/sign/out/ (gitignored)
toolchain/sign/sign-driver.sh build-kmd/panwin.sys      # needs osslsigncode
```

`osslsigncode` is not installed on the build host and is not in the configured pacman repos; the
script detects that and prints install options and the Windows-side alternative (`signtool` with
the generated `.pfx`). Catalog (`.cat`) creation and the rest of the device-side steps are in
docs/bringup/windows-install.md.
