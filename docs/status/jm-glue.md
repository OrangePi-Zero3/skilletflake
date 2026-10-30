# jm-glue: Windows glue, libdrm-win and panwin-info for the Mali-G31 (Zero3)

Owner key: `jm-glue`. Task list D of [`jm-arch.md`](jm-arch.md), plus the Zero3 bring-up guide
[`docs/bringup/zero3.md`](../bringup/zero3.md). Date: 2026-10-30. **Untested on hardware.**

## Summary

- **Board identification** (new `kmd/win/pw_win_plat.c/.h`, OS-independent, host-tested): the
  driver reads `DevicePropertyHardwareID` in EvtDeviceAdd and matches `ACPI\RKCP7410`,
  `*RKCP7410`, `ACPI\VEN_RKCP&DEV_7410[&...]` (RK3588 G610) or the same forms of `AWGP0031`
  (H616/H618 G31). Every SoC-specific step is keyed on it:
  - the RK3588 PMU2 check (maps 0xFD8D8000) runs **only** on the RK3588 ID (`Rk3588PmuCheck`
    still gates it there); on the H616 it can never run, whatever the registry says;
  - on the H616 the driver evaluates the GPU0 node's `_PSC` through ACPI.sys
    (`IOCTL_ACPI_EVAL_METHOD` on the default I/O target, in D0Entry) and touches no register
    unless it returns 0 (new registry value `AcpiPscCheck`, default 1);
  - an unrecognised ID touches no GPU register at all (logged);
  - MMIO window and GSIVs are compared per board ("the H616/H618 GPU is 0x1800000+0x40000"),
    replacing the fixed "RK3588 has 0x200000" warning.
- **`pw_win_driver.c`**: `timer_hz` from CNTFRQ_EL0 was already passed; comment now says 24 MHz on
  both SoCs. After probe the backend is recorded; on JM `BootFirmware` / `FirmwareName` are
  logged as ignored, a backend/board mismatch is warned about, and "core device up" prints the
  device's DRM name and version (`pw_device_drm_version`). DriverEntry logs the CSF version via
  `pw_device_drm_version(NULL)`.
- **GET_INFO** (`pw_win_ioctl.c`): DRM version from `pw_device_drm_version()` of the probed
  device (also after a failed probe, guarded by `probed` + `core_rundown`; panthor when no GPU was
  probed, as before), new additive flag `PANWIN_INFO_JM`. The user-MMIO mmap path returns the
  core's `pw_file_mmap_lookup()` errno unchanged, so the JM EINVAL reaches userspace as EINVAL
  (checked end to end through the core transport, below).
- **`pw_core_stub.c`** (`-Dcore=stub`): backend from GPU_ID like the core; on a Bifrost GPU it
  reports `panfrost` 1.0.0, implements `DRM_IOCTL_PANFROST_GET_PARAM` from raw register reads
  (panfrost meanings), refuses the MMIO offsets, and adds `pw_backend_for_gpu_id`,
  `pw_device_backend`, `pw_device_drm_version`. This gives the Z1 register dump without the real
  JM core and without unmasking any interrupt.
- **INF**: `panwin.inf` gets `ACPI\AWGP0031` / `ACPI\VEN_AWGP&DEV_0031` → `Panwin_Device_Zero3`
  (copies `panwin.sys` only, `Rk3588PmuCheck = 0`, `AcpiPscCheck = 1`). New `panwin-zero3.inf`
  with the identical G31 sections and no `mali_csffw.bin` in `[SourceDisksFiles]`, so a Zero3
  package doesn't need Arm's CSF blob. `kmd/win/tests/check_inf.py` (meson test "INF check")
  keeps both in sync and checks the RK3588 install path is unchanged.
- **`panwin_ioctl.h`** (additive): `PANWIN_INFO_JM`, `PANWIN_ACPI_HID_G31`, comments no longer
  panthor-only (driver ioctl numbers, mmap offsets, drm_version fields).
- **libdrm-win `xf86drm_win.c`**: the driver name of a device is what its VERSION says (fallback:
  GET_INFO `PANWIN_INFO_JM` → "panfrost", else "panthor"). `drmOpenWithType(name)` opens the first
  device with that name; `drmGetDevices2` / `drmGetDevice2` / `drmGetDeviceFromDevId` report
  `/gpu@1800000`, `allwinner,sun50i-h616-mali`, `arm,mali-bifrost` for panfrost and the RK3588
  info for panthor. Asking costs one transient transport open per device (no cache).
- **libdrm-win `transport_core.c`**: `PANWIN_CORE_MODEL=g31` → `mock_config.model =
  MOCK_MODEL_G31`, no firmware, bring-up succeeds when the probe picked the JM backend. The
  `0x40 + DRM_PANTHOR_GROUP_DESTROY` CS-sampling special case (= panfrost `JM_CTX_CREATE`), the CS
  statistics and the LATEST_FLUSH_ID page are panthor-only now. GET_INFO mirrors kmd/win.
  The fake kernel gained `PANWIN_FAKE_DRM_NAME=panfrost|panfrost-nocore` (name only) to test the
  naming logic without the core.
- **panwin-info**: on "panfrost" prints all 45 `DRM_PANFROST_PARAM_*` values, a summary in the
  layout of Linux panfrost's probe messages, and checks the MMIO offset is refused with EINVAL;
  Bifrost names in the product table; compatible string in the device list.
- **docs**: [`docs/bringup/zero3.md`](../bringup/zero3.md) (firmware patch + build + flash,
  driver build/sign/install, what to capture, Z1 checks in order); a pointer and the new
  parameter in `windows-install.md`.

## How to build/test

Run in a private copy (others work in the shared tree):

```sh
W=/tmp/claude-1000/jmglue; R=/home/e3xp0/Projects/panfrost-win/repo
mkdir -p $W/repo && cp -a $R/kmd $R/include $R/toolchain $W/repo/ && mkdir -p $W/repo/userspace $W/repo/tools \
  && cp -a $R/userspace/libdrm-win $W/repo/userspace/ && cp -a $R/tools/panwin-info $W/repo/tools/
cd $W/repo
# panwin.sys, both cores (+ the bring-up variant)
meson setup $W/b-stub kmd --cross-file toolchain/meson/aarch64-w64-kmd.ini -Dcore=stub
meson setup $W/b-real kmd --cross-file toolchain/meson/aarch64-w64-kmd.ini -Dcore=real
meson setup $W/b-dbg  kmd --cross-file toolchain/meson/aarch64-w64-kmd.ini -Dcore=stub -Ddebug_ioctls=true
for b in b-stub b-real b-dbg; do ninja -C $W/$b && meson test -C $W/$b; done
# glue host harness (+ test_plat)
make -C kmd/win/tests && make -C kmd/win/tests clean && make -C kmd/win/tests CC=clang
# libdrm-win both arches + panwin-info + wine tests (fake kernel)
sh userspace/libdrm-win/build.sh all
# libdrm-win with the in-process core (G610 + G31) under wine
meson setup $W/b-core userspace/libdrm-win --cross-file userspace/libdrm-win/cross/x86_64-w64-mingw32.ini \
  -Dcore_transport=true -Dfake_kernel=true \
  -Dcore_fw='Z:/home/e3xp0/Projects/panfrost-win/references/firmware/mali_csffw.bin'
ninja -C $W/b-core && meson test -C $W/b-core --print-errorlogs
python3 -I kmd/win/tests/check_inf.py kmd/win/panwin.inf kmd/win/panwin-zero3.inf
# panwin-info on the mock G31
cp out/x86_64/bin/panwin-info.exe $W/b-core/ && cd $W/b-core && \
  WINEDEBUG=-all PANWIN_TRANSPORT=core PANWIN_CORE_MODEL=g31 wine ./panwin-info.exe
```

If ninja in `$W/b-core` misses changes after re-copying `kmd/` with `cp -a` (old mtimes), run
`ninja -C $W/b-core -t clean` first.

## Verified (run here, 2026-10-30, private copy identical to the repo for every file I own)

- `-Dcore=stub`, `-Dcore=real`, `-Dcore=stub -Ddebug_ioctls=true`: panwin.sys links, **0
  warnings** (clean rebuilds); `meson test` 4/4 OK each (pw_fmt, new pw_win_plat host test, new
  INF check, image + imports: no new imports, the new WDF calls go through the function table).
  `-Dcore=real` was built against the kmd/core snapshot of ~14:00 including the other engineers'
  in-progress JM files.
- `make -C kmd/win/tests` (gcc, ASan+UBSan, -Werror) and `CC=clang`: test_plat 25 checks, 0
  failures; test_glue 9 tests OK, 0 failures.
- `userspace/libdrm-win/build.sh all`: arm64 and x86_64 libdrm.dll + panwin-info.exe build with 0
  warnings; wine: `libdrm_fake` OK (251 checks, includes the new driver-name tests: H616 info and
  `drmOpenWithType("panfrost")` with `PANWIN_FAKE_DRM_NAME=panfrost` and `=panfrost-nocore`, RK3588
  info and `drmOpenWithType("panfrost") == ENODEV` without), `ioctl_map` OK.
- Core transport build (`-Dcore_transport=true`), wine: `libdrm_core` (G610 + FW) OK 80 checks,
  `libdrm_fake` OK, new `libdrm_core_g31` OK 96 checks: device path `core:mock-g31`,
  drmGetDevices2 → `/gpu@1800000` / `allwinner,sun50i-h616-mali` / `arm,mali-bifrost`,
  `drmOpenWithType("panthor")` ENODEV, `("panfrost")` opens, GET_INFO flags `0x4` (JM, no
  FW_BOOTED) and DRM 1.5, VERSION `panfrost` 1.5.0, GET_CAP SYNCOBJ 1 / TIMELINE 0, **all 45
  GET_PARAMs answered by the real jm-hw code and equal to `mock_gpu.h`**, pad/unknown param EINVAL,
  `panwin_mmap` at both user-MMIO offsets → EINVAL, binary syncobj, teardown with 0 leaks, 0
  model errors, 0 unclaimed IRQs.
- panwin-info under wine on the mock G31 prints the full dump, summary and "user MMIO page: none
  on panfrost (mmap refused: Invalid argument)" (exit 0).
- `check_inf.py`: OK; mutation checks (PMU check 1, firmware CopyFiles in the G31 section) fail
  as expected.
- Also ran, unchanged code of others: `make -C kmd/tests` gcc and clang (110 tests, 0 failed, 52
  skips at that snapshot) and `make -C kmd/tests freestanding` (OK) in the private copy.

## Not verified / needs hardware

- Everything on the board: the INF install, pnputil/Inf2Cat acceptance of `panwin-zero3.inf`,
  `WdfDeviceQueryProperty(DevicePropertyHardwareID)` content, the `_PSC` evaluation in D0Entry
  (that the default I/O target accepts IOCTL_ACPI_EVAL_METHOD there, and the output layout),
  the raw interrupt descriptor's `Vector` being the GSIV, the stub's register reads on a G31,
  and every step of `zero3.md` (firmware build, FEL boot, flashing, DebugView/ETW capture).
- The KMDF glue changes in `pw_win_driver.c` have no host test (the fake WDK harness covers
  `pw_win_ioctl.c` but not the driver/PnP file); they are only compiled. GET_INFO's JM flag is
  verified through the core transport's copy of the logic, not through `pw_win_ioctl.c` (the
  glue harness has no GPU model to probe a G31).
- `tests/e2e/run.sh` (Mesa panvk under wine) was not re-run. It enumerates with drmGetDevices2,
  which now opens the device once more (one extra mock bring-up/teardown); its teardown check does
  not count bring-ups.

## Known issues & TODO (prioritised)

1. **Z1 on the board** following `zero3.md`; then replace the mock's guessed values (owner jm-hw)
   and correct the guide.
2. If the `_PSC` evaluation path fails on Windows, the H616 never gets past D0Entry unless
   `AcpiPscCheck=0`; a fallback would be reading the PPU/CCU registers directly (like the RK3588
   PMU check), which needs mapping SoC registers we do not own.
3. A board whose hardware ID cannot be read is now never touched. On the RK3588 this replaces
   "run the PMU check" in that (unlikely) failure case: safer, but a behaviour difference.
4. libdrm-win has no driver-name cache: each drmGetDevices2 opens every device once (cheap on
   panwin.sys, one mock bring-up on the core transport). Add a per-index cache if it shows up.
5. The stub's `SYSTEM_TIMESTAMP` is the raw TIMESTAMP register (the stub never starts the cycle
   counter); the real core does it properly.
6. Mesa on Windows has no panfrost kmod (jm-arch open question 1): Z3 stays blocked on a human.
7. `panwin.inf` `MfgName` changed from "panfrost-win (Rockchip RK3588)" to "panfrost-win"
   (display string only).

## Contract changes

- `include/panwin/panwin_ioctl.h` (additive, ABI stays 2): `PANWIN_INFO_JM (1u << 2)` (set when
  the probed GPU is a job-manager GPU, also after a failed probe that read GPU_ID; old drivers
  never set it), `PANWIN_ACPI_HID_G31 "AWGP0031"`; `drm_version_*` documented as the device's
  own version (panthor when no GPU was probed: unchanged for CSF). Reason: task D3; userspace
  needs the uAPI without a working VERSION.
- kmd/core contracts: none changed. The glue now calls the existing `pw_device_backend()` and
  `pw_device_drm_version()` (`pw_core_drm_version()` is no longer used by kmd/win); the stub core
  implements both plus `pw_backend_for_gpu_id()`.
- Registry (driver parameters): new `AcpiPscCheck` (DWORD, default 1, boards whose node has
  `_PSC`, i.e. the H616); `Rk3588PmuCheck` now only applies to the RK3588 hardware ID.
- libdrm-win: new env `PANWIN_CORE_MODEL` (core transport) and `PANWIN_FAKE_DRM_NAME` (fake
  kernel); `panwin_core_report.devices_created` counts G31 probes too; `panfrost_drm.h` is now
  installed under `include/libdrm/` (panwin-info needs it); `drmOpenWithType()` accepts the
  device's own driver name instead of only "panthor".
- Build: `kmd/win/meson.build` adds `pw_win_plat.c`, the `pw_win_plat (host)` and `INF check`
  tests and installs `panwin-zero3.inf`; `kmd/win/tests/Makefile` builds and runs `test_plat`;
  `userspace/libdrm-win/meson.build` adds the `libdrm_core_g31` test.

## Review

Independent review of task list D, 2026-10-30. **Untested on hardware**, like the piece itself.

### Re-run from clean (private copy `/tmp/claude-1000/rev-jmglue`, fresh `cp -a` of the tree)

- `-Dcore=stub`, `-Dcore=real`, `-Dcore=stub -Ddebug_ioctls=true`: 0 warnings, `meson test` 4/4
  OK each.
- `make -C kmd/win/tests` (gcc) and `CC=clang`: OK; also `make SAN=thread CC=clang`: OK.
- `userspace/libdrm-win/build.sh all`: both arches build; wine `libdrm_fake` + `ioctl_map` OK.
- Core transport build: `libdrm_core_g31`, `libdrm_core`, `libdrm_fake`, `ioctl_map` 4/4 OK;
  `panwin-info.exe` with `PANWIN_CORE_MODEL=g31` prints all 45 params, summary and "mmap refused:
  Invalid argument" (exit 0), as reported.
- `tests/e2e/run.sh` (not run by the engineer): **PASS**, 14/14 vk_e2e steps and icd_smoke; the
  extra enumeration open shows up as "devices 2 up/2 down", with no leaks.
- `make -C kmd/tests` gcc and clang: 130 tests, 0 failed, 52 skips; `make -C kmd/tests
  freestanding`: OK (at that snapshot of the other pieces).
- `check_inf.py`: OK.

The engineer's claims held up. I found no wrong numbers or behaviour.

### Checked against references

- Stub `GET_PARAM`: register offsets match Linux `panfrost_regs.h`. The semantics match
  `panfrost_drv.c` `panfrost_ioctl_get_param()` and `panfrost_gpu.c` `panfrost_gpu_init_features()`:
  - pad and unknown params give EINVAL;
  - `GPU_PROD_ID` is `id >> 16` and `REVISION` is `id & 0xffff`;
  - only popcount(JS_PRESENT) `JS_FEATURES` are read;
  - `NR_CORE_GROUPS` is popcount(L2_PRESENT);
  - `COHERENCY_FEATURES` is read from the register, which is right because G31 has
    `HW_FEATURE_COHERENCY_REG`;
  - `SELECTED_COHERENCY` is NONE, because there is no `dma-coherent`;
  - `ALLOWED_JM_CTX_PRIORITIES` is LOW|MEDIUM.

  No GPL code or text was copied (grep for GPL and copyright lines in the owned files: none).
- `_PSC` through the default I/O target in D0Entry: the KMDF source
  (`references/Windows-Driver-Frameworks`, `fxiotarget.cpp:63`) creates targets in the Started state.
  The power state machine never stops the default target. So the send in D0Entry is not refused
  on target state, on first start or after Dx, and `IGNORE_TARGET_STATE` is not needed.
  Whether ACPI.sys lets a function driver evaluate `_PSC` is still unverified.
- GET_INFO, rundown and `probed` ordering: the backend is written before the release store of
  `probed`, and D0Exit waits for the rundown before it destroys. No race found.
- The guide's pins are correct against the references: Mu-Silicium 76b15a8 and Silicium-ACPI
  6f911ab. `ACPI.inc` pulls `H616/DSDT_minimal.aml`, and the `build.sh` steps match.

### Tests added (coverage was thin where the glue meets a JM device)

- `kmd/win/tests/test_glue.c` has a new test, `jm`. The real core probes a static Mali-G31
  register file through `pw_win_ioctl.c`.
  - After a failed probe (no reset completion):
    - GET_INFO has `PANWIN_INFO_JM` and panfrost 1.5;
    - DRM ioctls give ENODEV;
    - once `probed` is cleared, GET_INFO gives the panthor answer again.
  - After a successful probe:
    - VERSION is "panfrost";
    - `GET_PARAM` works;
    - both user-MMIO mmap offsets give EINVAL and nothing is mapped.

  If future core init outgrows the static file, the second half is skipped with a message rather
  than failing.
- New `kmd/win/tests/test_stub.c`, built and run by `make` through the Makefile: the
  `-Dcore=stub` core (the Z1 build) behind the real glue.
  - G31: the probe masks all three lines; VERSION is panfrost; GET_INFO has JM; every one of the
    45 params is answered with the expected register values; unknown params and pad give EINVAL;
    other ioctls give ENOTTY; MMIO mmap gives EINVAL.
  - G610: panthor and the read-only user-MMIO page still map.

  It passes with gcc, clang and TSan.

### Bugs fixed

None needed. No product code was changed; only the tests and the Makefile above.

### Remaining concerns (ranked)

1. Hardware unknowns, already listed by the engineer: whether ACPI.sys accepts
   `IOCTL_ACPI_EVAL_METHOD` `_PSC` from the FDO, the content of `DevicePropertyHardwareID`, the
   raw `Vector` as GSIV, and Inf2Cat/pnputil on `panwin-zero3.inf`.
2. `pw_win_driver.c` (PnP, D0, the `_PSC` evaluation) still has no host test; the fake WDK has
   no PnP/ACPI model.
3. This predates jm-glue: when `regs_enabled` is 0 (board unknown, `_PSC` not 0, PMU off), the
   ISR returns FALSE without masking. A line that is really asserted would storm. That is
   unlikely with the GPU unpowered, but it is not guarded.
4. The stub's `SYSTEM_TIMESTAMP` reads LO and then HI without a hi-lo-hi retry, so a read can
   tear. The cycle counter is not started, so this is cosmetic for Z1.
5. Cosmetic: the "AcpiPscCheck ignored" debug line fires only when the value is 0 on the
   RK3588, the opposite polarity from the Rk3588PmuCheck line.
