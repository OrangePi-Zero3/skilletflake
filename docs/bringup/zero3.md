# Orange Pi Zero3 (Mali-G31): bring-up to Z1

Status: written 2026-10-30, **before any run on the board**. Nothing below has been tried on a
Zero3. Steps that depend on the board, the firmware or Windows behaviour we could not check are
marked **[unverified]**. Please write down everything that differs (or paste the outputs listed
in step 7) so this guide and the mock's guessed register values can be corrected.

Target: halal-beef's Orange Pi Zero3 (Allwinner H618, 4x Cortex-A53, Mali-G31 MP2, 1.5 GB),
his Mu-Silicium UEFI on SPI NOR, Windows 11 ARM64 **23H2 or older** on the microSD card (the A53
is ARMv8.0; 24H2 and later reportedly need ARMv8.1), installed with his `awmmc` driver, test
signing already on.

**Z1** (docs/PLAN.md) means: the firmware describes the GPU (`\_SB.GPU0`, `_HID "AWGP0031"`),
`panwin.sys` loads test-signed, ACPI `_PSC` reports 0 (powered), `GPU_ID` reads back as a
Mali-G31, and `panwin-info` prints every `DRM_PANFROST_PARAM_*` value so we can replace the
guessed values in the mock (table "Values to confirm" in
[`docs/status/jm-arch.md`](../status/jm-arch.md)). Running jobs (Z2) and Mesa (Z3) come later;
Z3 is also blocked on the userspace side (jm-arch open question 1).

What you need:

- A Linux host with the UEFI-Builder dependencies (its `setup-env.sh` installs them), ACPICA's
  `iasl` (and `acpiexec` for the optional AML test), `sunxi-tools` (`sunxi-fel`), and for the
  driver: `/opt/llvm-mingw` (or any llvm-mingw with the aarch64 target), meson, ninja, python3,
  openssl, optionally `osslsigncode`.
- A USB-UART adapter (3.3 V) on the Zero3's debug UART (UART0, the 3-pin header). The usual
  Allwinner setting is 115200 8N1 **[unverified: use whatever your UEFI prints at]**.
- A USB 2.0 hub for the keyboard (Windows only has EHCI on this board, no OHCI).
- A way to create the driver catalog: the WDK/EWDK `Inf2Cat` on any Windows PC, or PowerShell
  `New-FileCatalog` (step 4).
- A known-good `spl-bootable.img` (the one you run today) to go back to.

## 1. Firmware with the GPU0 node

Our patch [`firmware/zero3/patches/0001-H616-add-Mali-G31-GPU0-node.diff`](../../firmware/zero3/patches/)
adds `\_SB.GPU0` to Silicium-ACPI's `SoCs/Allwinner/H616/DSDT_minimal.dsl`: the GPU block
(0x01800000 + 0x40000, GSIV 127/128/129 = job/mmu/gpu), `_PS0` (PPU domain on, GPU clock 400 MHz
from `gpu1`, bus gate, reset release), `_PS3`, and `_PSC` (0 only when domain, bus gate and reset
are all on). The firmware ships the DSDT as prebuilt AML (`Zero3Pkg/Include/ACPI.inc` pulls
`H616/DSDT_minimal.aml` from the `Silicium-ACPI` submodule), so the `.aml` must be rebuilt
after patching.

```sh
git clone --recursive https://github.com/OrangePi-Zero3/UEFI-Builder
cd UEFI-Builder
./setup-env.sh -p apt            # or dnf / pacman; runs Mu-Silicium/setup_env.sh (needs sudo)

# The patch is against Silicium-ACPI 6f911ab, which is what UEFI-Builder (Mu-Silicium 76b15a8)
# pins. Check before applying:
git -C Mu-Silicium log -1 --format=%h                  # 76b15a8 (or newer: check the next line)
git -C Mu-Silicium/Silicium-ACPI log -1 --format=%h    # 6f911ab

PW=/path/to/panfrost-win/repo
git -C Mu-Silicium/Silicium-ACPI apply --check "$PW/firmware/zero3/patches/0001-H616-add-Mali-G31-GPU0-node.diff"
git -C Mu-Silicium/Silicium-ACPI apply "$PW/firmware/zero3/patches/0001-H616-add-Mali-G31-GPU0-node.diff"

# Rebuild the AML the firmware embeds (iasl writes DSDT_minimal.aml next to the .dsl).
( cd Mu-Silicium/Silicium-ACPI/SoCs/Allwinner/H616 && iasl DSDT_minimal.dsl )
grep -c AWGP0031 Mu-Silicium/Silicium-ACPI/SoCs/Allwinner/H616/DSDT_minimal.aml   # >= 1
```

Optional, on the Linux host: our acpiexec test runs `_PS0` / `_PSC` / `_PS3` of the patched node
against emulated memory and checks the register sequence (not the hardware's reaction):

```sh
IASL=$(command -v iasl) ACPIEXEC=$(command -v acpiexec) \
  "$PW/firmware/zero3/tests/run_aml_test.sh" "$PWD/Mu-Silicium/Silicium-ACPI"   # AML TEST: PASSED
```

Build (DEBUG gives firmware logs on the UART):

```sh
./build.sh DEBUG                 # -> spl-bootable.img (UEFI + TF-A + thirty SPL)
```

Notes:

- `build.sh` calls `build_uefi.py -c` (clean build) but not `-u`. Don't add `-u`: it runs
  `git pull` + `git submodule update` and may move Silicium-ACPI away from the patched tree
  **[unverified whether it discards the change; re-check with `git -C ... diff --stat`]**.
- If `git apply --check` fails, the submodule is not at 6f911ab: tell us the commit and don't
  hand-merge the patch.

### Flashing

Try the new firmware from RAM first, so a broken build cannot leave the board unbootable:

1. Put the board in FEL mode (UEFI-Builder's guide leaves the method to you; one way is to boot
   with an SD card that forces FEL, e.g. the `fel-sdboot.sunxi` image from sunxi-tools
   **[unverified on the Zero3]**), connect the USB-C OTG port to the host.
2. `./fel-boot-uefi.sh` in UEFI-Builder: loads SPL, TF-A (debug build if present) and
   `Mu-Silicium/Mu-Zero3.bin` over USB and starts them, without touching the SPI flash. Then
   swap in the Windows SD card... **[unverified: FEL needs the SD slot free or a FEL SD card;
   if you cannot boot Windows this way, use it only to check that the UEFI comes up]**.

Then write the SPI flash, either from FEL:

```sh
sudo sunxi-fel -p spiflash-write 0 spl-bootable.img
```

or with the firmware's own updater (UEFI-Builder `GUIDES/Updating-UEFI.md`): copy
`spl-bootable.img` to the root of a GPT + FAT32 USB stick, boot with only that stick attached,
wait for the reboot. Keep the previous image to flash back.

## 2. Is the GPU node there? (no driver yet)

Boot Windows and, in an elevated prompt:

```bat
pnputil /enum-devices /deviceid "ACPI\AWGP0031"
```

Expected: one device, instance `ACPI\AWGP0031\0` **[unverified exact instance path]**, status
"problem" / no driver (an "Unknown device" under Other devices). Device Manager → the device →
Resources should show memory `01800000 - 0183FFFF` and three IRQs, 127, 128, 129
**[unverified how Device Manager numbers ARM64 interrupts]**. If the device is missing, the
patched AML is not in the flashed firmware: re-check step 1 before going on.

Without a driver nothing runs `_PS0`, so the GPU stays unpowered here; that is expected.

## 3. Build the driver package (Linux host)

From the panfrost-win repo. Build the **stub core first**: it only reads GPU_ID and the feature
registers, keeps all three interrupt lines masked and implements `GET_PARAM` from raw register
reads, which is all Z1 needs. The real core (job slots, MMU, interrupts) comes second.

```sh
meson setup build-kmd-stub kmd --cross-file toolchain/meson/aarch64-w64-kmd.ini \
      -Dcore=stub -Ddebug_ioctls=true
ninja -C build-kmd-stub && meson test -C build-kmd-stub
meson setup build-kmd-real kmd --cross-file toolchain/meson/aarch64-w64-kmd.ini \
      -Dcore=real -Ddebug_ioctls=true
ninja -C build-kmd-real && meson test -C build-kmd-real

userspace/libdrm-win/build.sh arm64       # -> out/arm64/bin/panwin-info.exe + libdrm.dll

toolchain/sign/make-test-cert.sh                          # once -> toolchain/sign/out/
toolchain/sign/sign-driver.sh build-kmd-stub/panwin.sys   # needs osslsigncode, else sign in step 4

mkdir -p pkg-stub
cp kmd/win/panwin-zero3.inf build-kmd-stub/panwin.sys build-kmd-stub/panwin.pdb pkg-stub/
cp toolchain/sign/out/panwin-test.cer toolchain/sign/out/panwin-test.pfx pkg-stub/
cp out/arm64/bin/panwin-info.exe out/arm64/bin/libdrm.dll pkg-stub/
```

Same for `pkg-real/` from `build-kmd-real/`. Use **`panwin-zero3.inf`**: it installs
`panwin.sys` only. `panwin.inf` also matches the Zero3 (same G31 sections, kept identical by
`kmd/win/tests/check_inf.py`), but it lists the RK3588's `mali_csffw.bin` in `[SourceDisksFiles]`
and then needs that file in the package.

`-Ddebug_ioctls=true` enables `PANWIN_IOCTL_DEBUG_REG_READ` (raw register reads from
`panwin-info --reg`). It is for bring-up only.

**Import lists are approximate.** `toolchain/lib/*.def` were generated without a real ARM64
Windows; the driver may fail to load (code 39, STATUS_PROCEDURE_NOT_FOUND) if it imports
something your build of Windows does not export. On the first boot, copy
`C:\Windows\System32\ntoskrnl.exe`, `hal.dll`, `drivers\WDFLDR.SYS` (and note the version of
`drivers\Wdf01000.sys`) to the host and regenerate the `.def` files as described in
[`toolchain/README.md`](../../toolchain/README.md) ("Day one on the board"), then rebuild.

## 4. Catalog, certificate, test signing (Windows)

Test signing is already on (`bcdedit /enum {current}` shows `testsigning Yes`; the desktop
shows "Test Mode"). The Mu firmware is built without `-s`, so Secure Boot should be off
**[unverified]**.

Trust the test certificate (elevated, in the package directory):

```bat
certutil -addstore -f Root panwin-test.cer
certutil -addstore -f TrustedPublisher panwin-test.cer
```

Create and sign `panwin.cat`, exactly as in [`windows-install.md`](windows-install.md) step 4,
with `panwin-zero3.inf` in the directory instead of `panwin.inf`:

- WDK/EWDK on another PC: `Inf2Cat /driver:C:\pkg-stub /os:10_CO_ARM64,10_NI_ARM64`
  (Windows 11 21H2 / 22H2-23H2), then `signtool sign /fd SHA256 /f panwin-test.pfx /p panwin`
  on `panwin.sys` (if not signed on Linux) and on `panwin.cat`.
- No WDK: PowerShell `New-FileCatalog` + `Set-AuthenticodeSignature`
  **[unverified: pnputil may reject a catalog without the OS attributes Inf2Cat adds]**.

Re-create and re-sign the catalog after every rebuild of `panwin.sys`.

## 5. Logging, before installing

panwin.sys logs with `DbgPrintEx(DPFLTR_IHVDRIVER_ID, ...)`, prefix `panwin:`. Enable all levels
and reboot:

```bat
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter" /v IHVDRIVER /t REG_DWORD /d 0xF /f
```

What to capture, in order of usefulness:

- **DebugView** (Sysinternals), elevated, *Capture Kernel* + *Enable Verbose Kernel Output*,
  *Log to file*. Kernel capture needs DebugView's native ARM64 build **[unverified which file of
  the current DebugView zip that is]**; an x64 build under emulation cannot capture kernel output.
  Start it before installing the driver.
- **UART** (DEBUG firmware): the firmware log, and whatever is printed if the board dies on the
  first GPU register access (an SError usually shows up as a hang with no Windows log at all).
  The DBG2 table describes UART0 (0x05000000, 16550-style), so kernel debugging over serial
  (`bcdedit /debug on`, `bcdedit /dbgsettings serial debugport:1 baudrate:115200`) might work
  with WinDbg on the host **[unverified: the port is shared with the firmware console]**.
- **Install and PnP state**: `C:\Windows\INF\setupapi.dev.log` (search `AWGP0031`),
  Event Viewer → System → source *Kernel-PnP* (device start problems), and an ETW trace of PnP
  around the install **[unverified provider behaviour on this build]**:
  ```bat
  logman start pnptrace -p Microsoft-Windows-Kernel-PnP -o C:\pnp.etl -ets
  rem ... install / restart the device ...
  logman stop pnptrace -ets
  ```
- A bugcheck leaves `C:\Windows\Minidump\*.dmp` (enable small dumps in System Properties →
  Advanced → Startup and Recovery). Keep `panwin.pdb` with each build.

**If Windows hangs at boot once the driver is installed** (the device is present at every boot,
so the driver loads every boot): take the SD card to the Linux host, mount the NTFS partition
and rename `Windows/System32/drivers/panwin.sys`; the device then fails to start and Windows
boots. Safe Mode should also skip it **[unverified]**.

## 6. Install

```bat
cd C:\pkg-stub
pnputil /add-driver panwin-zero3.inf /install
pnputil /enum-devices /deviceid "ACPI\AWGP0031" /drivers
sc query panwin
```

The device moves to *System devices* as "Arm Mali-G31 GPU (panfrost-win, test)". Codes:
52 = signature/catalog problem (step 4); 39 or 31 = the image did not load (most likely an
import, step 3); 10 = start failed (resource mismatch: the driver needs exactly 1 memory +
3 interrupt resources and logs what it got). The driver keeps the device started even when
the GPU part fails, so `panwin-info` can still say why.

Registry parameters (`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\AWGP0031\0\Device Parameters`,
read at device start; `pnputil /restart-device "ACPI\AWGP0031\0"` to re-read):

| Value | Zero3 default | Meaning |
|---|---|---|
| `AcpiPscCheck` | 1 | evaluate GPU0 `_PSC` before the first GPU register access; anything but 0 → registers are not touched. 0 skips the check (only if `_PSC` itself is broken) |
| `Rk3588PmuCheck` | 0 | ignored on this board: the RK3588 PMU check is keyed on the RK3588 hardware ID and can never run here |
| `BootFirmware`, `FirmwareName` | unset | ignored on a job-manager GPU (logged) |

## 7. Z1 checks, in order

Record each result (copy the text) before going to the next.

**Z1.1 – driver identifies the board.** In DebugView, in this order (values from the code, not
from a run):

```
panwin: panwin.sys 0.1.0 (ABI 2, CSF uAPI panthor 1.0.0, JM uAPI panfrost, debug ioctls), ...
panwin: board: H616/H618 Mali-G31 MP2 (job manager, _HID AWGP0031, hardware ID ACPI\...)
panwin: params: BootFirmware=0 Rk3588PmuCheck=0 AcpiPscCheck=1 FirmwareName=(from GPU_ID)
panwin: CNTFRQ_EL0 = 24000000 Hz
panwin: irq 0 (job): raw vector 127 (firmware node: GSIV 127), vector ... level ... level
panwin: irq 1 (mmu): raw vector 128 (firmware node: GSIV 128), ...
panwin: irq 2 (gpu): raw vector 129 (firmware node: GSIV 129), ...
panwin: MMIO 0x1800000+0x40000 mapped at ... (MmMapIoSpaceEx)
```

(the stub reports CSF uAPI 1.0.0, the real core 1.6.0). A "board not recognised" error means the
hardware ID was not `AWGP0031`; nothing touches the GPU then. "MMIO window is ..., the H616/H618
GPU is 0x1800000+0x40000" or "GSIV ..., DIFFERENT" means the `_CRS` is not what the patch says.
Whether the raw vector really is the GSIV on ARM64 is **[unverified]**; if all three say
DIFFERENT but the device works, it is just the log's assumption that is wrong.

**Z1.2 – `_PSC` is 0.**

```
panwin: ACPI _PSC = 0 -> GPU on (D0)
```

`_PSC = 3` means `_PS0` did not take effect (PPU gate, bus gate or reset): stop here and send
the log; the driver has not touched the GPU. "ACPI _PSC: evaluation failed: 0x..." means the
evaluation path itself does not work on this Windows build **[unverified]**: send the status
code. Only then consider `AcpiPscCheck=0` (the next access may hang the board).

**Z1.3 – GPU_ID.**

```
panwin: core stub: GPU_ID 0x7093.... (arch 7.0 rev 9, product major 3, r?p?)
panwin: core device up: core stub: GPU_ID 0x7093.... arch 7.0, job manager (GET_PARAM only) (DRM panfrost 1.0.0)
```

Expected arch 7, product major 3 (Mesa `PAN_PROD_ID(7, 0, 3)` = G31); the low 16 bits (revision
and status) are what we want to learn. `GPU_ID 0x00000000` / `0xffffffff` ("GPU not responding")
means the clock or reset is still off despite `_PSC`.

**Z1.4 – panwin-info.**

```bat
cd C:\pkg-stub
panwin-info.exe > z1-stub.txt
panwin-info.exe --reg 0x0 --reg 0x4 --reg 0x14 --reg 0x18 --reg 0x1c --reg 0xe00 >> z1-stub.txt
```

Expected shape (shown here from the mock G31 through libdrm-win's in-process core transport
under wine: the numbers are the mock's guesses, **not** hardware values):

```
devices: 1
  [0] /dev/dri/renderD128  bus=platform /gpu@1800000 (allwinner,sun50i-h616-mali)  win32=\\?\ACPI#AWGP0031#...
GET_INFO:
  abi_version 2, flags 0x6 debug-build job-manager, emulated DRM 1.0, MMIO base 0x1800000
drmGetVersion: panfrost 1.0.0 (0) "panfrost DRM (panfrost-win core stub)"
DRM_CAP_SYNCOBJ_TIMELINE: 0 (as Linux panfrost: Mesa emulates timelines)
GET_PARAM (DRM_IOCTL_PANFROST_GET_PARAM, all 45 params):
   0 GPU_PROD_ID                 0x0000000000007093  28819
   1 GPU_REVISION                0x0000000000000000  0
   ...
summary:
  Mali-G31 id 0x7093 major 0x0 minor 0x0 status 0x0 (GPU_ID 0x70930000, arch 7.0.9)
  Features: L2:0x07110206 Shader:0x00000002 Tiler:0x00000209 Mem:0x1 MMU:0x00002830 AS:0xff JS:0x7
  shader_present=0x3 l2_present=0x1 (2 cores, 1 L2)
  ...
user MMIO page: none on panfrost (mmap refused: Invalid argument (errno 22))
```

With the stub, `SYSTEM_TIMESTAMP` may read 0 (the stub never starts the cycle counter) and
`SYSTEM_TIMESTAMP_FREQUENCY` is CNTFRQ_EL0. The "user MMIO page" line must say EINVAL.

**Z1.5 – compare.** Put `z1-stub.txt` next to the "Values to confirm on the board" table in
[`docs/status/jm-arch.md`](../status/jm-arch.md) (GPU_ID, L2/CORE/TILER/MEM/MMU features,
AS/JS_PRESENT, the *_PRESENT masks, THREAD_*, TLS_ALLOC, JS_FEATURES0-2, TEXTURE_FEATURES0,
COHERENCY, AFBC, STACK_PRESENT). Send the file and the DebugView log; the mock values in
`kmd/tests/mock_gpu.h` get replaced by them. The `summary:` lines use the layout of Linux
panfrost's probe messages, so a Linux boot log of the same board (if you have one) can be
compared line by line.

**Z1.6 – real core.** Uninstall the stub package (`pnputil /enum-drivers`, find the panwin
`oemNN.inf`, `pnputil /delete-driver oemNN.inf /uninstall /force`), install `pkg-real` the same
way. The real core soft-resets the GPU, powers L2, shader cores and tiler, programs the MMU and
unmasks the job/MMU/GPU interrupts, so this is the first step that does more than read
registers **[untested on hardware]**. Expected:

```
panwin: core device up: Mali-G31 r0p0 arch7.0 (id 0x7093), 2 cores, job manager (no firmware) (DRM panfrost 1.5.0)
```

Then `panwin-info.exe > z1-real.txt`: the same GET_PARAM values as the stub (now from the core),
`drmGetVersion: panfrost 1.5.0`, and `SYSTEM_TIMESTAMP` non-zero and increasing between two runs.
Also check `pnputil /restart-device "ACPI\AWGP0031\0"` and `pnputil /disable-device` /
`/enable-device` leave the machine alive (D0Exit, `_PS3`, D0Entry, `_PS0`), and that the
DebugView log has no "lines masked by glue" warnings (an interrupt line that fired without a
handler).

That is Z1. Z2 (a kernel-built WRITE_VALUE job on JS1) needs the job-manager scheduler that is
still being written; Z3 needs Mesa's panfrost kmod on Windows (jm-arch open question 1).
