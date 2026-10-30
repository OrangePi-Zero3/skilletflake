# Status: libdrm-win (key `libdrm-win`)

## Summary

`userspace/libdrm-win` builds `libdrm.dll`, a Windows (ARM64 and x86_64) DLL that implements the
libdrm API subset Mesa panfrost/panvk, the Vulkan runtime and util use. It runs on top of panwin.sys
through `NtDeviceIoControlFile`, so it sees the raw NTSTATUS. It also provides:

- emulated DRM fds, synthetic `/dev/dri/renderD128+i` nodes, and mmap/munmap
- an in-process fake kernel for tests
- an import lib, headers, and a `libdrm.pc` (version 2.4.134; Mesa needs >= 2.4.109)

`tools/panwin-info` is a bring-up console tool built against the installed `libdrm.pc`.

Everything builds for both architectures. The x86_64 tests pass under wine against the fake kernel
(154 checks plus the ioctl-mapping test). **Untested on hardware, never run against a real
panwin.sys, and ARM64 binaries were never executed** (no ARM64 Windows here).

Upstream libdrm is cloned shallow at `references/libdrm`, commit `33fab8cbca7f`, version 2.4.134.

## What was built and how to build/test it

Layout:

```
userspace/libdrm-win/
  include/xf86drm.h      upstream libdrm header + 2 tiny "#if defined(_WIN32)" tweaks (marked "panwin:")
  include/libdrm_win.h   Windows extensions: panwin_open/close/dup/is_fd/fd_to_handle/stat_rdev,
                         panwin_mmap/munmap/is_mapping, panwin_syncobj_event, panwin_private_ioctl,
                         panwin_device_path, errno mappers, panwin_use_fake_kernel
  src/xf86drm_win.c      drm* API (wrapper semantics follow upstream xf86drm.c, MIT)
  src/fd.c               fd table (fds start at 0x4000, max 1024; SRWLOCK), refcounted shared kernel
                         file (dup + mappings keep it alive), mmap tracking, transport selection
  src/transport_win32.c  SetupDiGetClassDevs(PANWIN_INTERFACE_GUID) -> fallback \\.\PanWin0
                         (QueryDosDevice probe) -> env PANWIN_DEVICE override; CreateFileW;
                         NtDeviceIoControlFile
  src/errno_map.c        NTSTATUS / Win32 error -> CRT errno; decodes Linux errno carried in a
                         customer NTSTATUS (see Requested contract changes)
  src/fake_kernel.c      fake panwin.sys (only with -Dfake_kernel=true)
  libdrm.def             export list (63 symbols)
  cross/{aarch64,x86_64}-w64-mingw32.ini, meson.build, meson_options.txt, build.sh, LICENSE
  tests/test_ioctl_map.c    compile-time + runtime ioctl-number/struct-layout checks
  tests/test_libdrm_fake.c  runtime tests against the fake kernel
tools/panwin-info/{meson.build,panwin-info.c}
```

Exported drm API:

- **Core:** `drmIoctl`, `drmGetVersion`, `drmFreeVersion`, `drmGetLibVersion`, `drmGetCap`,
  `drmSetClientCap`, `drmIsMaster`, `drmCommand{None,Read,Write,WriteRead}`.
- **Open / close:** `drmAvailable`, `drmOpen`, `drmOpenWithType`, `drmOpenRender`, `drmClose`.
- **Devices:** `drmGetDevices2`, `drmGetDevices`, `drmGetDevice2`, `drmGetDevice`,
  `drmGetDeviceFromDevId`, `drmGetNodeTypeFromDevId`, `drmFreeDevice`, `drmFreeDevices`,
  `drmDevicesEqual`.
- **Names:** `drmGetDeviceNameFromFd2`, `drmGetRenderDeviceNameFromFd`,
  `drmGetPrimaryDeviceNameFromFd`, `drmGetNodeTypeFromFd`.
- **Buffers:** `drmCloseBufferHandle`, `drmPrimeHandleToFD` and `drmPrimeFDToHandle` (ENOSYS).
- **Syncobj:** `drmSyncobj{Create,Destroy,Wait,TimelineWait,Reset,Signal,TimelineSignal,Query,Query2,Transfer}`.
  `HandleToFD`, `FDToHandle`, `ImportSyncFile`, `ExportSyncFile` and `Eventfd` return ENOSYS.

Coverage was checked by grepping every `drm*()` call in the following Mesa trees against `libdrm.def`:

- `src/panfrost/{lib/kmod,vulkan}`
- `src/util`
- `src/vulkan/{runtime,wsi/wsi_common*.c}`
- `src/gallium/{drivers,winsys}/panfrost`
- `src/loader`

The only missing call is `drmGetMagic`, which is used only by Wayland/KMS paths.

Each device is synthesized as `DRM_BUS_PLATFORM`:

- fullname `/gpu@fb000000`
- compatible `rockchip,rk3588-mali` and `arm,mali-valhall-csf`
- render node only

Device numbers use `PANWIN_MAKEDEV(226, 128+i)`, which is glibc's `(maj<<8)|min`.

Build and install everything (arm64 + x86_64), then run the tests under wine:

```sh
/home/e3xp0/Projects/panfrost-win/repo/userspace/libdrm-win/build.sh        # or: build.sh arm64 | x86_64
```

The script installs into these prefixes. The Mesa cross build should use
`PKG_CONFIG_LIBDIR=<prefix>/lib/pkgconfig`:

- `/home/e3xp0/Projects/panfrost-win/repo/out/arm64/`: production build without the fake kernel.
- `/home/e3xp0/Projects/panfrost-win/repo/out/x86_64/`: built with `-Dfake_kernel=true` for dev/test.

Each prefix contains `bin/libdrm.dll`, `bin/panwin-info.exe`, `lib/libdrm.dll.a` and
`lib/pkgconfig/libdrm.pc` (`Cflags: -I${includedir} -I${includedir}/libdrm`, `Libs: -ldrm`). The
headers are laid out like upstream libdrm:

- `include/xf86drm.h` and `include/libdrm_win.h`
- `include/libdrm/{drm.h,drm_mode.h,panthor_drm.h,sys/ioccom.h}`, copied unchanged from the
  repo `include/`
- `include/panwin/panwin_ioctl.h`

Build trees go to `out/build/{libdrm-win,panwin-info}-<arch>/`. To run the steps by hand:

```sh
cd /home/e3xp0/Projects/panfrost-win/repo/userspace/libdrm-win
meson setup ../../out/build/libdrm-win-x86_64 --cross-file cross/x86_64-w64-mingw32.ini \
      --prefix ../../out/x86_64 --libdir lib -Dfake_kernel=true
ninja -C ../../out/build/libdrm-win-x86_64 && meson test -C ../../out/build/libdrm-win-x86_64
WINEDEBUG=-all ../../out/x86_64/bin/panwin-info.exe --fake --reg 0x0 --reg 0x100
```

Select the fake kernel with `PANWIN_FAKE_KERNEL=1`, `panwin_use_fake_kernel(1)` or `panwin-info --fake`.
`PANWIN_DEVICE=<win32 path>` forces the device used by the real transport.

## Verified

- **Builds:** both arches build with no warnings at `-Wall -Wextra`. `llvm-readobj` on
  `out/arm64/bin/libdrm.dll` reports `IMAGE_FILE_MACHINE_ARM64` and 63 exports. Its imports are only
  SETUPAPI, ntdll, KERNEL32 and the api-ms-win-crt-* (UCRT) DLLs.
- **pkg-config:** `panwin-info` finds libdrm through the installed `libdrm.pc` ("Run-time
  dependency libdrm found: YES 2.4.134").
- **Tests under wine 11.20 (x86_64), `meson test`: 2/2 OK.**
  - `ioctl_map` checks every DRM_IOCTL_* value and uAPI struct size it covers against values
    printed by a host program built on `references/linux/include/uapi`. The Windows build gives
    identical values, including `SYNCOBJ_WAIT=0xC02864C3` (with `deadline_nsec`) and
    `TIMELINE_WAIT=0xC03064CA`. It also round-trips `PANWIN_IOCTL_DRM(nr)` for all 256 values of nr.
  - `libdrm_fake` runs 154 checks:
    - errno mapping
    - device enumeration and devid round-trip
    - fd/dup sharing and EBADF/EMFILE
    - two-pass VERSION ("panthor" 1.5.0), GET_CAP, and DEV_QUERY GPU_INFO/CSIF_INFO (two-pass),
      plus ENOTTY/EINVAL paths
    - binary and timeline syncobj create/wait/-ETIME after a real ~50 ms wait/WAIT_ALL/reset/
      signal/query/transfer, a 4-thread timeline wait, and SYNCOBJ_EVENT
    - BO create, two mmaps aliasing the same pages, GEM_CLOSE with the mapping kept alive, a
      mapping outliving its fd, and the read-only flush-id page
    - PRIME returning ENOSYS
  - A deliberate mutation of the fake's version was caught by the tests.
- **panwin-info under wine:**
  - No device: prints "devices: 0". This exercised SetupAPI and QueryDosDevice under wine.
  - `--fake`: prints the full decode ("Mali-G610 r0p0 (arch 10.8.6...)", VA 48 bits, 4 cores,
    CSIF) and `--reg` reads.
  - `PANWIN_DEVICE` pointing at a regular file: the ioctl fails with EOPNOTSUPP. This is the real
    NtDeviceIoControlFile -> errno path.
- **Mesa:** `src/util/u_sync_provider.c` compiles for aarch64 against the installed headers, and
  every drm symbol it references is exported.

## Not verified / needs hardware

- Any interaction with a real panwin.sys:
  - SetupAPI interface enumeration with a device present
  - `CreateFileW` on the interface path
  - METHOD_BUFFERED round trips through a KMDF driver
  - NTSTATUS values a real driver returns
  - kernel-side user-pointer probing
  - MMAP/MUNMAP VA lifetime
  - SYNCOBJ_EVENT handle duplication
- ARM64 binaries were built but never run. Running them needs an ARM64 Windows machine or an
  ARM64 wine, and neither is available here.
- GPU_INFO values in the fake are plausible for G610 but **not measured** (GPU_ID 0xa8670005 matches
  what panthor reports on RK3588; the other values are illustrative).
- Mesa panvk built and linked against this DLL. That is the Mesa owner's work. `panthor_kmod.c`
  currently fails earlier on Mesa-side `os_mman.h` and `c11/time.h`, not on libdrm.

## Known issues & TODO (prioritised)

1. **No dma-buf or sync-file fds.** These return ENOSYS:
   - `drmPrime*`
   - `drmSyncobj{HandleToFD,FDToHandle,Import,ExportSyncFile}`

   As a result, external memory, external semaphores and WSI buffer sharing are unavailable. The
   plan is NT-handle based sharing, which needs a new private ioctl. panthor_kmod only needs these
   for shared BOs.
2. **Mesa must route its POSIX calls on DRM fds to libdrm-win.** The calls are `open`, `close`,
   `dup`/`os_dupfd_cloexec`, `stat`, `os_mmap` and `os_munmap`; the replacements are
   `panwin_open`, `panwin_close`, `panwin_dup`, `panwin_stat_rdev`, `panwin_mmap` and
   `panwin_munmap`. `panwin_is_fd()` and `panwin_munmap()` returning EINVAL let the wrappers fall
   back to the CRT or VirtualAlloc. CRT `read`, `fstat`, `poll` and `lseek` do not work on these
   fds.
3. **Mesa build gaps, for the Mesa owner:**
   - On Windows, Mesa's meson only looks up libdrm when `system_has_kms_drm` is true, which is
     false there. Mesa needs a patch to use `dependency('libdrm')` for panfrost.
   - `O_CLOEXEC` (and therefore `DRM_CLOEXEC`) is undefined on mingw.
   - UCRT `strerror()` has no text for ETIME (137), EOPNOTSUPP (130) and the other supplementary
     errnos.
4. **`panwin_munmap` always releases the whole mapping.** Partial munmap is unsupported.
5. **Fake-kernel simplifications:**
   - There is no GPU, so every fence is signalled at submit.
   - Transfer of an unsubmitted point returns EINVAL instead of chaining.
   - `deadline_nsec` is ignored.
   - There is one global lock.
   - `fake_kernel` is in the x86_64 install only.
6. **`drmGetDevices2` re-enumerates SetupAPI on every call.** This is cheap enough, but the order
   is whatever SetupAPI returns.
7. **64-bit only.** `drm_version` uses `size_t`/pointers and the code static-asserts LP64-compatible
   layout. ARM64EC, ARM64X and 32-bit clients are not handled.
8. **Not shipped:** `xf86drmMode.h`, `libsync.h` (Mesa uses its own `util/libsync.h`), `drmGetMagic`
   and `drmAuthMagic`. Add them if a consumer needs them.
9. **The arm64 build tree also builds `test_ioctl_map.exe`.** It cannot be run here.

## Requested contract changes

These are for `include/panwin/panwin_ioctl.h`. None of them were edited. libdrm-win already
behaves as described below.

1. **Error reporting.** The header says the errno is "also reported in the output's trailing
   panwin_ioctl_status", but no such struct exists. Proposal: add
   `#define PANWIN_NTSTATUS_FROM_LINUX_ERRNO(e) ((int32_t)(0xE0000000u | (0xD52u << 16) | (uint32_t)(e)))`.
   - The fields are severity=error, customer bit, facility 0xD52, and `e` a positive Linux errno
     (the core's `PW_E*` values, negated).
   - panwin.sys completes failing IRPs with that status. libdrm-win calls `NtDeviceIoControlFile`
     and translates Linux errno numbers to CRT errno numbers. Standard NTSTATUS codes are mapped
     too.
   - Never complete a timeout with `STATUS_TIMEOUT` (0x102, a success code): use the errno form
     with ETIME (62) or `STATUS_IO_TIMEOUT`.
2. **Syncobj timeouts.** Document that `timeout_nsec` (and `deadline_nsec`) are absolute
   nanoseconds on the QueryPerformanceCounter clock: `QPC * 1e9 / QPF`. In the kernel that is
   `KeQueryPerformanceCounter`. This is what Mesa's `os_time_get_nano()` returns on Windows, and
   `INT64_MAX` means infinite.
3. **User MMIO offset on LLP64.** `DRM_PANTHOR_USER_MMIO_OFFSET` is `1<<43` (the `_32BIT` variant)
   on Windows because `sizeof(unsigned long) == 4`. The kernel default must be the 32BIT offset,
   and SET_USER_MMIO_OFFSET should accept both.
4. **Buffer sizes.**
   - DRM ioctls are sent with in = out = `_IOC_SIZE(request)` (also for `_IOW` ioctls such as
     GEM_CLOSE). `_IO` ioctls are sent with no buffers.
   - MUNMAP is sent with out_size 0. The other private ioctls are sent with in = out =
     sizeof(struct).
   - The kernel should accept the struct sizes in the repo's drm.h (for example
     `drm_syncobj_wait` is 40 bytes, with `deadline_nsec`).
5. **VERSION semantics.** DRM_IOCTL_VERSION must follow Linux `drm_copy_field`: copy
   min(len, strlen) bytes and set len to strlen. libdrm-win does two passes.
6. **SYNCOBJ_EVENT ownership.** Document that the returned `event_handle` is owned by the caller,
   who must CloseHandle it.
7. **Mapping lifetime.** Mappings are torn down at IRP_MJ_CLEANUP. libdrm-win holds the kernel file
   open while any mapping exists, so Linux "mapping outlives fd" semantics hold.

## Review

Independent review, 2026-10-30. I did not take the original report on trust.

### What I re-ran (clean)

- I deleted `out/build/{libdrm-win,panwin-info}-*`, `out/build/pkgconfig-*.ini`, `out/arm64` and
  `out/x86_64`, then ran `userspace/libdrm-win/build.sh`.
  - Both architectures built and installed with no compiler warnings. `meson test` passed 2/2:
    `libdrm_fake` "OK: 154 checks, 0 failures" before my fixes, and `ioctl_map` OK.
- **ARM64 DLL:** `llvm-readobj` shows `IMAGE_FILE_MACHINE_ARM64` and 63 exports (47 `drm*` and 16
  `panwin_*`). Its imports are only SETUPAPI, ntdll, KERNEL32 and api-ms-win-crt-*.
- **pkg-config:** the installed `libdrm.pc` is version 2.4.134 with
  `Cflags: -I${includedir} -I${includedir}/libdrm`.
- **panwin-info under wine:**
  - With no driver it prints "devices: 0" and exits 1.
  - `--fake --reg 0x0 --reg 0x100` prints the full decode.
  - `PANWIN_DEVICE=Z:<regular file>` makes every ioctl fail with errno 130 (EOPNOTSUPP), which is
    the real NtDeviceIoControlFile path.
- **ioctl numbers and struct sizes:** I checked these independently. A host gcc program built on
  `references/linux/include/uapi/drm/{drm,panthor_drm}.h` printed values identical to the
  static asserts in `tests/test_ioctl_map.c` (for example `SYNCOBJ_WAIT=0xC02864C3` and
  `drm_panthor_gpu_info`=112).
- **Mesa coverage:** grepping Mesa's panfrost, util, vulkan/runtime, wsi_common*, gallium panfrost
  and loader code against `libdrm.def`, the only calls not covered are `drmMode*`, `drmCrtc*`,
  `drm{Set,Drop}Master`, `drm{Get,Auth}Magic` and `drmHandleEvent`. All of them are in
  `wsi_common_display.c` (KMS) or `util/libdrm.h` (stubs). The claim holds.
- **Header:** the diff of the shipped `xf86drm.h` against `references/libdrm` (33fab8c) contains
  only the two marked `_WIN32` tweaks.
- **GPU_ID decode:** the panwin-info product table matches Mesa `model/pan_model.c`.
- **Clock:** Mesa's `os_time_get_nano()` on Windows (`c11/impl/time.c`, `TIME_MONOTONIC`) is QPC
  based, as the timeout contract proposal says.
- **License hygiene:** every file is SPDX MIT. `xf86drm.h` is upstream MIT/X11. I found no GPL-only
  code.
- **After my fixes:** a full rebuild of both architectures gave no warnings. `meson test` passed 2/2
  with `libdrm_fake` at "OK: 161 checks, 0 failures".

**Verdict:** the report's claims held up. Every build and test result reproduced.

### Bugs found and fixed

1. **The errno table did not invert the KMD's actual mapping.** `kmd/win/pw_win_ioctl.c`
   (`pw_win_status_from_errno`) completes IRPs with standard NTSTATUS values, not the proposed
   customer code. Before the fix:
   - `STATUS_DEVICE_NOT_READY`, which the KMD uses for -ENODEV, became EAGAIN. That is wrong, and it
     would make any EAGAIN-retrying caller spin.
   - `STATUS_QUOTA_EXCEEDED` (-ENOSPC) and `STATUS_DEVICE_POWERED_OFF` (0x8000000F, a warning code
     the KMD uses when the GPU is not up) fell through to EIO.

   Fixed in `src/errno_map.c`:
   - DEVICE_NOT_READY now maps to ENODEV, and DEVICE_POWERED_OFF and INVALID_DEVICE_STATE map to
     ENODEV.
   - QUOTA_EXCEEDED maps to ENOSPC, and IO_DEVICE_ERROR maps to EIO explicitly.
   - I added Linux errnos ESRCH, EXDEV, ENFILE, EPIPE, EDEADLK, ENAMETOOLONG, EPROTO, EBADMSG and
     ENOBUFS.
   - I added 7 test checks.
2. **`STATUS_USER_APC` and `STATUS_ALERTED` were treated as success.** These are success-class
   codes, and `w32_ioctl` returned 0 for them, so an alerted wait would hand back an unfilled
   struct as success.
   - They now map to EINTR.
   - `drmIoctl` now restarts on EINTR, as upstream libdrm does. It still does not retry EAGAIN,
     so there is no busy loop.

### Remaining concerns (by severity)

1. **High (cross-component):** the KMD maps both -ETIMEDOUT and timeouts to `STATUS_IO_TIMEOUT`.
   libdrm-win turns that into ETIME, which is right for syncobj waits (Mesa `vk_drm_syncobj`
   checks `errno == ETIME`) but wrong for a genuine -ETIMEDOUT. ETIME and ETIMEDOUT can only be
   told apart if the KMD adopts the customer-code proposal (Requested contract change 1) or gives
   them distinct statuses. The lead should resolve this between kmd-win and libdrm-win.
2. **Medium:** errno is shared with Mesa only if Mesa also links the UCRT (llvm-mingw's default).
   A build against msvcrt.dll would silently lose every errno.
3. **Medium:** there is no dma-buf, sync-file or PRIME support (ENOSYS), so WSI, external memory
   and external semaphores are unavailable.
4. **Medium:** `drmGetDevices2` re-enumerates and rebuilds the path table on every call. If a
   device appears or disappears between enumeration and `panwin_open`, the index can point at a
   different device.
5. **Low:**
   - `panwin_munmap` ignores `size` (no partial unmap).
   - `/dev/dri/card<i>` paths open and stat successfully even though `available_nodes` lists only
     the render node.
   - The fake kernel computes QPC nanoseconds with a `double`; this is exact for about 100 days of
     uptime and affects tests only.
6. **Untested on hardware:** nothing was run against a real panwin.sys, and no ARM64 binary was
   executed.
