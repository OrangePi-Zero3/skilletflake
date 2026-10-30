# Installing panwin.sys on an RK3588 Windows on ARM board (test-signed)

For the Orange Pi Zero3 (Mali-G31, job manager) see [`zero3.md`](zero3.md) instead; this guide
is the RK3588 one.

Status: written before any hardware run. Every step that depends on the board or on Windows
behaviour we could not try is marked **[unverified]**. Report what actually happens in
`docs/status/` so this guide can be corrected.

Target: RK3588 board (ROCK 5B/5B+, Orange Pi 5 Plus, ...) running edk2-rk3588 **with our GPU ACPI
patch** (`firmware/edk2-rk3588/`, device `_HID "RKCP7410"`, 1 memory + 3 interrupt resources) and
Windows 11 ARM64. Have the UART console connected (1500000 8N1 on edk2-rk3588 boards).

## 0. Before Windows: is the GPU powered?

Touching the GPU registers with the PD_GPU power domain off raises an SError (machine hang).
From the UEFI shell **[unverified syntax]**:

```
Shell> mm FD8D8290 -w 4 -MMIO -n     # PMU2 BISR_STATUS4: bit 1 set   = PD_GPU on, repaired
Shell> mm FD8D8120 -w 4 -MMIO -n     # PMU2 BUS_IDLE_ST0: bit 0 clear = GPU NIU out of idle
Shell> mm FD8D8118 -w 4 -MMIO -n     # PMU2 BUS_IDLE_ACK0: bit 0 clear
Shell> mm FD8D8180 -w 4 -MMIO -n     # PMU2 PWR_GATE_ST0: bit 0 clear = PD_GPU on (info)
Shell> mm FB000000 -w 4 -MMIO -n     # GPU_ID, only if the above say "on": expect 0xA867xxxx
```

panwin.sys performs the same check as the edk2 patch's `GPON` method before its first GPU access
(registry `Rk3588PmuCheck`, default 1) and refuses to touch the GPU if it fails. The check only
runs when the device's hardware ID is `RKCP7410` (kmd/win/pw_win_plat.c); a device whose ID the
driver does not recognise is never touched.

## 1. Build the package on Linux

```sh
meson setup build-kmd kmd --cross-file toolchain/meson/aarch64-w64-kmd.ini -Dcore=stub -Ddebug_ioctls=true
ninja -C build-kmd && meson test -C build-kmd
toolchain/sign/make-test-cert.sh                       # once; -> toolchain/sign/out/
toolchain/sign/sign-driver.sh build-kmd/panwin.sys     # needs osslsigncode (else sign on Windows, step 4)

mkdir -p pkg && cp kmd/win/panwin.inf build-kmd/panwin.sys build-kmd/panwin.pdb pkg/
cp /path/to/linux-firmware/arm/mali/arch10.8/mali_csffw.bin pkg/   # Arm CSF firmware, 282624 bytes
cp toolchain/sign/out/panwin-test.cer toolchain/sign/out/panwin-test.pfx pkg/
```

Start with `-Dcore=stub` (reads GPU_ID only, keeps all interrupts masked), then `-Dcore=real`
with `BootFirmware=0`, then `BootFirmware=1` (step 6). Copy `pkg/` to the board (USB stick,
network share).

## 2. Disable Secure Boot, enable test signing

1. edk2-rk3588 setup (Esc at boot): make sure Secure Boot is disabled / no keys enrolled
   **[unverified menu path]**. `bcdedit` refuses test signing while Secure Boot is on ("value is
   protected by Secure Boot policy").
2. Windows Security → Device security → Core isolation → **Memory integrity off** for bring-up
   (HVCI adds requirements: page hashes, no RWX, NX pool. We meet them on paper, untested).
3. Elevated command prompt:
   ```bat
   bcdedit /set testsigning on
   shutdown /r /t 0
   ```
   After reboot the desktop shows the "Test Mode" watermark.
   One-shot alternative: Advanced startup → Startup Settings → 7 "Disable driver signature
   enforcement" (lasts one boot).

## 3. Trust the test certificate

Elevated prompt in the package directory:

```bat
certutil -addstore -f Root panwin-test.cer
certutil -addstore -f TrustedPublisher panwin-test.cer
```

(PowerShell equivalent: `Import-Certificate -FilePath panwin-test.cer -CertStoreLocation
Cert:\LocalMachine\Root`, same for `Cert:\LocalMachine\TrustedPublisher`.) With test signing on,
any certificate is accepted for the kernel image itself; the Root/TrustedPublisher entries are what
let PnP install the catalog without the "Windows can't verify the publisher" prompt.

## 4. Sign the driver and create the catalog (on Windows)

PnP installation (`pnputil`) needs a signed catalog `panwin.cat` covering `panwin.sys` and
`mali_csffw.bin`. osslsigncode can sign but not create catalogs, so create it on Windows.

**Option A, WDK or EWDK available** (any x64/ARM64 Windows PC; known-good path):

```bat
Inf2Cat /driver:C:\pkg /os:10_VB_ARM64,10_CO_ARM64,10_NI_ARM64,10_GE_ARM64
signtool sign /fd SHA256 /f panwin-test.pfx /p panwin panwin.sys     (if not signed on Linux)
signtool sign /fd SHA256 /f panwin-test.pfx /p panwin panwin.cat
```

`Inf2Cat /?` lists the OS identifiers your WDK knows; the INF's models section is decorated
`NTarm64.10.0...19041`, so any ARM64 identifier from 10_VB (2004) up applies.

**Option B, no WDK** (PowerShell, elevated) **[unverified: pnputil may reject a catalog without
the OS attributes Inf2Cat adds]**:

```powershell
$pfx  = Import-PfxCertificate -FilePath .\panwin-test.pfx -CertStoreLocation Cert:\CurrentUser\My `
          -Password (ConvertTo-SecureString panwin -AsPlainText -Force)
Set-AuthenticodeSignature -FilePath .\panwin.sys -Certificate $pfx -HashAlgorithm SHA256
New-FileCatalog -Path C:\pkg -CatalogFilePath C:\pkg\panwin.cat -CatalogVersion 2.0
Set-AuthenticodeSignature -FilePath .\panwin.cat -Certificate $pfx -HashAlgorithm SHA256
```

Re-create and re-sign the catalog after **any** change to a file in the package (a rebuilt
panwin.sys invalidates it).

## 5. Install

First confirm the ACPI node exists: Device Manager → View → Devices by connection, or

```bat
pnputil /enum-devices /deviceid "ACPI\RKCP7410"
```

Without a driver it shows up as an unknown device; its Resources tab should list memory
`FB000000-FB1FFFFF` and three IRQs (GSIV 124, 125, 126). If it is missing, the edk2 patch is not
active and nothing below can work.

```bat
pnputil /add-driver C:\pkg\panwin.inf /install
pnputil /enum-devices /deviceid "ACPI\RKCP7410" /drivers
sc query panwin
```

The device appears under *System devices* as "Arm Mali-G610 GPU (panfrost-win, test)". Problems
are logged in `C:\Windows\INF\setupapi.dev.log` (search for RKCP7410). Code 52 = signature
problem; code 39/31 = the driver failed to load (most likely a missing import, see step 8);
code 10 = start failed (resource mismatch: panwin.sys requires exactly 1 memory + 3 interrupt
resources and logs what it got).

Files land in `C:\Windows\System32\drivers\panwin.sys` and
`C:\Windows\System32\drivers\panwin\arm\mali\arch10.8\mali_csffw.bin`.

Update after a rebuild: copy the new package, recreate/re-sign the catalog, then
`pnputil /add-driver C:\pkg\panwin.inf /install` again (or Device Manager → Update driver →
Have disk) and reboot or restart the device. Remove: `pnputil /enum-drivers` (find the
`oemNN.inf` for panwin) and `pnputil /delete-driver oemNN.inf /uninstall /force`.

## 6. Driver parameters

Set by the INF in the device's hardware key, `HKLM\SYSTEM\CurrentControlSet\Enum\<instance>\Device
Parameters` (instance path from step 5, e.g. `ACPI\RKCP7410\0` **[unverified]**):

| Value | Type | Default | Meaning |
|---|---|---|---|
| `BootFirmware` | DWORD | 0 | 0 = M1 (GPU_ID, reset, L2); 1 = load and boot the CSF firmware |
| `FirmwareRoot` | REG_SZ | `\SystemRoot\System32\drivers\panwin\` | NT path prefix for firmware files |
| `FirmwareName` | REG_SZ | (unset) | override `arm/mali/arch<maj>.<min>/mali_csffw.bin` |
| `Rk3588PmuCheck` | DWORD | 1 | read PMU PD_GPU state before touching the GPU (0 = skip, risky); RK3588 hardware ID only |
| `AcpiPscCheck` | DWORD | 1 | not used on the RK3588 (its node has no `_PSC`); see zero3.md |

The service key `HKLM\SYSTEM\CurrentControlSet\Services\panwin\Parameters` is used as a
fallback. Values are read when the device starts:

```bat
reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7410\0\Device Parameters" /v BootFirmware /t REG_DWORD /d 1 /f
pnputil /restart-device "ACPI\RKCP7410\0"
```

## 7. Seeing the driver's log (DbgPrint)

panwin.sys logs with `DbgPrintEx(DPFLTR_IHVDRIVER_ID, level, "panwin: ...")`. Levels:
error → 0 (always delivered), warning → 1, info → 2, debug → 3. Levels above 0 are filtered by
default; enable them and reboot:

```bat
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter" /v IHVDRIVER /t REG_DWORD /d 0xF /f
```

(or live in a kernel debugger: `ed nt!Kd_IHVDRIVER_Mask 0xf`).

- **DebugView** (Sysinternals): run elevated, enable *Capture Kernel* and *Enable Verbose Kernel
  Output*. It needs its own kernel driver, so it must be a native ARM64 build; an x64 build under
  emulation cannot capture kernel output **[unverified whether the current DebugView ships ARM64]**.
- **Kernel debugger (recommended for bring-up, survives hangs)** with WinDbg on a second PC:
  - Serial: `bcdedit /debug on` and `bcdedit /dbgsettings serial debugport:1 baudrate:1500000`.
    On ARM64, kdcom finds the UART through the ACPI DBG2 table, so edk2-rk3588 must publish a
    DBG2 entry for the debug UART and Windows must support that UART type
    **[unverified on RK3588; check `DBG2` in the edk2 tables]**. If the console UART is shared
    with the firmware log, expect interleaving.
  - Network (KDNET): `bcdedit /dbgsettings net hostip:<pc-ip> port:50000 key:1.2.3.4`; only works if
    KDNET supports the board's NIC (`kdnet.exe` on the device lists supported adapters)
    **[unverified for the RK3588 boards' Realtek NICs]**.
  - Symbols: copy `panwin.pdb` next to the sources on the debugger PC, then
    `.sympath+ C:\pkg`, `.reload /f panwin.sys`, `lm m panwin`.

## 8. Day-one checklist (first boot with the board)

1. UEFI shell: PMU/GPU_ID reads from step 0. Record the values.
2. Copy `C:\Windows\System32\ntoskrnl.exe`, `hal.dll`, `drivers\WDFLDR.SYS`, `drivers\Wdf01000.sys`
   to the build host and regenerate the import definitions (toolchain/README.md, "Day one on the
   board"). Rebuild; `meson test` must still pass. Until then the import list is an
   approximation and the driver may fail to load with code 39 / STATUS_PROCEDURE_NOT_FOUND.
   While there, settle the open question: `petool.py exports ntoskrnl.exe | grep -i irql` — are
   `KeGetCurrentIrql`/`KfRaiseIrql` exported on ARM64?
3. Note the KMDF version (`Wdf01000.sys` → Properties → Details) and the Windows build (`winver`).
4. Install the `core=stub` build (BootFirmware=0). Expected log, in order: `panwin.sys 0.1.0 ...`,
   `board: RK3588 Mali-G610 (CSF, _HID RKCP7410, ...)`, `params: ...`, `irq 0 (job) ... irq 2 (gpu)`, `MMIO 0xfb000000+0x200000 mapped`, `PMU: ... -> GPU
   on`, `CNTFRQ_EL0 = 24000000 Hz`, `core stub: GPU_ID 0xa867....`, `core device up`. Then run
   `tools/panwin-info` (GET_INFO with `abi_version 2`, DRM_IOCTL_VERSION `panthor 1.0.0`,
   DEV_QUERY GPU_INFO, then `user MMIO page: LATEST_FLUSH_ID = 0x...` - the read-only user
   mapping of GPU register page 0x10000 through PANWIN_IOCTL_MMAP; with `-Ddebug_ioctls=true` also
   raw register reads, e.g. `--reg 0x10000` should print the same flush ID). CSIF_INFO fails with
   ENOSYS on the stub (no firmware). [unverified] Extra ABI v2 checks worth doing here:
   - the flush-ID page is really read-only: a write from user mode must raise an access
     violation (relies on `MdlMappingNoWrite` being honoured for UserMode MDL mappings);
   - closing panwin-info while it runs (Ctrl+C) leaves no mapping behind (Driver Verifier with
     "pool tracking" + "I/O verification" on panwin.sys is the cheap way to catch leaks);
   - `pnputil /disable-device` while a client has the device open: its ioctls must then fail
     with ENODEV (file revoked in D0ExitPreInterruptsDisabled), no bugcheck.
5. Install the `core=real` build with BootFirmware=0, then 1 (M2: firmware boot).
6. Write down everything that differed from this guide in `docs/status/`.
