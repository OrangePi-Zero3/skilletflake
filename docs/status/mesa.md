# Status: Mesa panvk for Windows (key `mesa`)

## Update 2026-10-30: hard fork, tarball + one diff, panvk v7 on the G31

This replaces the patch-series workflow described further down; the rest of the file is the
earlier record.

- **Fork.** The owner decided to treat Mesa as a hard fork locked at `MESA_COMMIT`
  (`userspace/mesa/AI-POLICY.md`). `build.sh` downloads the gitlab source tarball of that commit
  (sha256 `ee590de6…`, checked; its contents are identical to `git archive` of the commit), extracts
  it to `references/mesa-panwin` and applies `patches/panwin-mesa.diff`. The 14 `.patch` files
  are gone; `build.sh diff` exports branch `panwin` of `references/mesa-src` as the one diff, with
  the commit messages as a `#` header. The fork deletes `AGENTS.md` and `CLAUDE.md`.
- **Checked:** tarball + diff gives a tree identical to `git archive panwin` (`diff -r`, symlinks
  included); a changed diff is swapped in place (reverse old, apply new) and untouched files keep
  their timestamps.
- **New fork commits** (17 in all, +477/-111 in 33 files):
  - `pan/kmod: build the panfrost (JM) backend on Windows`: `panfrost_kmod.c` is built again;
    panthor's DRM-fd mmap helpers move to `pan_kmod.h` (`pan_kmod_fd_mmap/munmap`) and serve
    panfrost BOs too (bo_munmap op, 64-bit offsets); perf counters fail with ENOTSUP on Windows
    (the sampling thread needs eventfd/timerfd/poll; only the perf tools use them).
  - `panvk/jm: copy event fences with SYNCOBJ_TRANSFER on Windows`, replacing the sync_file
    export/import round trip. The kernel core accepts binary-to-binary TRANSFER on JM for this
    (`pw_file.c`); timeline points stay EOPNOTSUPP and DRM_CAP_SYNCOBJ_TIMELINE stays 0.
- **panvk v7 under wine on the mock G31** (`tests/e2e/run.sh g31`, `PANWIN_CORE_MODEL=g31`,
  `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1`): enumerates "Mali-G31 MC2" (Vulkan 1.3), creates a
  device, and `vk_e2e` passes 12 steps and skips 2 (no timestamp queries and no sparse binding on
  v7): submit + fence, an event set by the job chain and seen by the CPU, timeline semaphores, 1000
  resubmits, teardown with no leaks. The only failing ioctls are SYNCOBJ_WAIT timeouts from event
  polling. Shaders are not executed by the mock, so fill/copy results are not checked.
- Not run: the aarch64 DLL on hardware.

## Summary

Mesa's panvk now cross-compiles and links for Windows as `vulkan_panfrost.dll` plus an ICD json.
It links against the real libdrm-win `libdrm.dll`. The build includes:

- the CSF v10 path, plus v11-v14
- JM v6/v7 per-arch code (the JM kernel backend is not built on Windows)
- all precompiled helper shaders (`libpan_v*`)

The aarch64 DLL is ARM64, imports only system DLLs and `libdrm.dll`, and exports the three
`vk_icd*` entrypoints. The x86_64 build of the same series was run under wine with no loader:

- It negotiates ICD interface 7, lists `VK_KHR_win32_surface`, and creates an instance.
- With no device it enumerates 0 physical devices.
- Against libdrm-win's fake kernel it enumerates **1 physical device, "Mali-G610 MC4, Vulkan
  1.4.366"**. That is the full panthor_kmod -> libdrm-win -> DeviceIoControl-shaped path.

`vkCreateDevice` then stops at `DRM_IOCTL_PANTHOR_VM_BIND`, which the fake kernel doesn't
implement (ENOTTY).

The Mesa changes are 12 small commits on branch `panwin` in `references/mesa-src`. They are
exported to `userspace/mesa/patches/`: +419/-82 lines in 31 files, of which +219/-46 in `src/panfrost`.

The same series still builds natively on Linux (0 warnings). There, `vulkaninfo --summary` with
Mesa's panthor noop drm-shim still reports Mali-G610/panvk.

**Untested on hardware. The ARM64 DLL was never executed** (there is no ARM64 Windows here).

**Process issue: read this before anything goes upstream.** The Mesa tree's `AGENTS.md` (also
`CLAUDE.md`) says:

> "Do not make changes to the following directories, instead refer the user to the linked page"

for `src/panfrost`. The linked `docs/drivers/panfrost.rst` "AI policy" asks contributors "to
refrain from using AI to generate patches".

- These patches are AI-written.
- They are kept as a local downstream series, never submitted, and labelled
  `Generated-by: LLM` as `AGENTS.md` requires.
- 5 of the 12 commits (0008-0012) touch `src/panfrost`.
- If upstreaming is ever wanted, a human has to rewrite them. Use them as a map of what to change,
  not as submittable patches.
- The lead/user should decide whether keeping AI-generated `src/panfrost` changes, even
  downstream, is acceptable for this project.

## What was built and how to build/test it

Files, all under `userspace/mesa/`:

| Path | What |
|---|---|
| `MESA_COMMIT` | `b39d00df594f6cea276c9c17591f6900798ed80f` |
| `patches/0001..0012-*.patch` | the series (`git am` order), see below |
| `cross/{aarch64,x86_64}-w64-mingw32.ini` | meson cross files (llvm-mingw, `-mno-ms-bitfields`, `-I include/compat` for `sys/ioccom.h`, `needs_exe_wrapper`) |
| `build.sh` | idempotent driver: checkout + `git am`, host tools, libdrm lookup, meson/ninja/install, import/export checks |
| `stub-libdrm/` | no-device `libdrm.dll` stand-in (same prototypes as libdrm-win). Only used if libdrm-win isn't installed |
| `tests/icd_smoke.c`, `tests/run-wine-smoke.sh` | loader-less ICD smoke test (x86_64, wine) |
| `README.md` | short overview |

The series:

1. `util`: Windows `os_mman.h`. Anonymous mmap goes through VirtualAlloc, device mappings return
   ENODEV, and `os_mprotect` is added.
2. `util/libsync`: failing `sync_wait`/`sync_merge`/`sync_valid_fd` on Windows.
3. `vulkan/runtime`: `thrd_yield()` instead of `sched_yield()` in `vk_drm_syncobj.c`.
4. `vulkan/runtime`: keep `vk_image::drm_format_mod` on all platforms.
5. `vulkan/wsi`: `WSI_HAVE_DRM`. This keeps dma-buf/explicit-sync WSI and `wsi_common_drm.c` out
   of Windows builds.
6. `vulkan/wsi/win32`: don't fail `wsi_device_init` for HW drivers when DXGI/DComp are
   unavailable; fall back to the CPU path. Without this fix, panvk had no device under wine.
7. `meson`: `dependency('libdrm')` on Windows for panvk, and project-wide `-mno-ms-bitfields`
   (MinGW + panfrost).
8. `panfrost`: drop unused POSIX includes (midgard, decode.c); pandecode uses `os_mprotect`.
9. `pan/kmod`: new optional `bo_munmap` op plus `pan_kmod_bo_munmap()`, used at all 9 panvk BO
   unmap sites. There is no change on Linux.
10. `pan/kmod`: Windows via libdrm-win.
    - `pan_kmod_fd_{open,dup,close}` map to `panwin_open/dup/close`.
    - panthor maps BOs and the LATEST_FLUSH_ID page through `panwin_mmap`, unmaps through
      `panwin_munmap`, and uses `os_get_page_size()`.
    - The mmap offset is now `int64_t`, because `off_t` is 32-bit on LLP64 and DRM fake offsets
      are larger than 4 GiB.
    - The JM `panfrost_kmod.c` is not built on Windows (it needs eventfd/timerfd/poll).
11. `panvk`: when there is no ELF build-id (`HAVE_BUILD_ID == 0`), the driver/cache UUID comes
    from `disk_cache_get_function_identifier()`, as in dozen. On Windows that is the DLL's file
    time.
12. `panvk`: Windows support.
    - Adds `VK_KHR_win32_surface`, so `PANVK_USE_WSI_PLATFORM` covers Win32.
    - Device ids come from `panwin_stat_rdev`.
    - `VK_EXT_physical_device_drm`, `KHR_external_{memory,semaphore,fence}_fd`,
      `EXT_external_memory_dma_buf` and `EXT_map_memory_placed` are off on Windows.
    - The DLL is built with `vulkan_api.def` exports, no `lib` prefix, and a backslash ICD json
      (as for lavapipe).

Build (uses `references/mesa-src`, `references/host-tools`, `references/mesa-build-{host,aarch64,x86_64}`):

```sh
/home/e3xp0/Projects/panfrost-win/repo/userspace/libdrm-win/build.sh   # prerequisite: installs repo/out/{arm64,x86_64}
/home/e3xp0/Projects/panfrost-win/repo/userspace/mesa/build.sh all     # or: aarch64 (default) | x86_64 | checkout | host-tools
/home/e3xp0/Projects/panfrost-win/repo/userspace/mesa/tests/run-wine-smoke.sh
```

Outputs:

- `references/mesa-build-aarch64/install/bin/{vulkan_panfrost.dll,libdrm.dll}`
- `references/mesa-build-aarch64/install/share/vulkan/icd.d/panfrost_icd.aarch64.json`, with
  `"library_path": "..\\..\\..\\bin\\vulkan_panfrost.dll"`
- the same files under `mesa-build-x86_64` for x86_64

Useful overrides:

- `LIBDRM_WIN_PREFIX`
- `MESA_SRC`
- `PANWIN_MESA_NO_RESET=1`, to build the working tree as-is while developing
- `JOBS`
- `MESA_BUILDTYPE`

Host tools: `references/host-tools/prefix/bin/{mesa_clc,vtn_bindgen2,panfrost_compile,llvm-spirv}`.

- The system has LLVM/clang 23.1.4 and SPIRV-Tools, but no `spirv-llvm-translator` package.
  `build.sh` builds SPIRV-LLVM-Translator from source into the prefix: v23.1.2, matched to
  `llvm-config --version`, with SPIRV-Headers at the translator's pinned commit.
- libclc is **not** needed: Mesa only uses it for rusticl/microsoft-clc.
- To use system packages instead: `sudo pacman -S --needed spirv-llvm-translator`, with
  `llvm clang spirv-tools` already installed.

Developing: edit `references/mesa-src` (branch `panwin`) and commit with the
`Generated-by: LLM` trailer. Then run `git format-patch --zero-commit --no-signature -o
userspace/mesa/patches b39d00df..panwin` (delete the old patches first).

## Verified

All of the following were actually run here on 2026-10-30.

- **Fresh `build.sh all`** after wiping both build dirs: applies the series, then configures,
  builds and installs both arches, rc=0.
  - Compiler warnings: 1 per arch, `-Wmissing-braces` in the upstream `wsi_common_win32.cpp:584`.
  - Linker warnings: `lld: unsupported build id hashing: sha1, using default`.
- **aarch64 DLL.** `llvm-readobj` shows:
  - Machine `IMAGE_FILE_MACHINE_ARM64`, 15.6 MB stripped.
  - Imports: `libdrm.dll KERNEL32 USER32 GDI32 api-ms-win-core-synch-l1-2-0` plus 11
    `api-ms-win-crt-*` (UCRT). There is no libc++/winpthread/zlib DLL; zlib is a static
    subproject.
  - Exports: `vk_icdGetInstanceProcAddr`, `vk_icdGetPhysicalDeviceProcAddr`,
    `vk_icdNegotiateLoaderICDInterfaceVersion`.
- **x86_64 DLL:** same imports/exports, `IMAGE_FILE_MACHINE_AMD64`.
- **Precomp:** `libpan_shaders_v{6,7,10..14}.{c,h}` and `libpan_v*.{cpp,h,spv}` were generated by
  the host tools and compiled in. Nothing is stubbed.
- **Idempotency.**
  - A second `build.sh checkout` reports "series already applied".
  - A fresh worktree from the exported patches (`MESA_SRC=<tmp> build.sh checkout`) gives a tree
    identical to `panwin` (same tree hash). It was removed afterwards.
- **Wine 11.20 smoke test**, `tests/run-wine-smoke.sh`, x86_64:
  - No device: `vk_icdNegotiateLoaderICDInterfaceVersion` → 0, interface 7. 13 instance
    extensions, including `VK_KHR_win32_surface` rev 6. `vkCreateInstance` 0.
    `vkEnumeratePhysicalDevices` → 0 devices.
  - With `PANWIN_FAKE_KERNEL=1`:
    - `Found compatible device '/dev/dri/renderD128'`
    - `panvk (Mali-G610 MC4) Mesa 27.0.0-devel`
    - 1 device: Vulkan 1.4.366, vendor 0x13b5, device 0xa8670005
  - That run exercised these paths:
    - `drmGetDevices2`
    - `panwin_open`
    - VERSION / DEV_QUERY
    - `SET_USER_MMIO_OFFSET`
    - `panwin_mmap` of the flush-id page
    - the syncobj create/wait probe (timeline is mandatory on v10)
    - `panwin_stat_rdev`
    - win32 WSI init
  - With `--create-device`: `DRM_IOCTL_PANTHOR_VM_BIND failed (err=25)` →
    `VK_ERROR_OUT_OF_DEVICE_MEMORY`. That is a fake-kernel gap.
- **Linux regression check.** The native build (`references/mesa-build-host`, full panvk +
  `-Dtools=drm-shim`) with the series compiles with 0 warnings. Then this command prints
  `deviceName = Mali-G610 MC4`, `driverName = panvk`, `apiVersion = 1.4.366`:

  ```sh
  PAN_GPU_ID=a867 LD_PRELOAD=.../libpanfrost_noop_drm_shim.so \
  VK_DRIVER_FILES=.../panfrost_devenv_icd.x86_64.json vulkaninfo --summary
  ```

- **Vulkan-Loader behaviour.** From reading `loader/loader.c` `check_and_add_to_new_phys_devs`
  on main (not run): the loader merges devices from `vk_icdEnumerateAdapterPhysicalDevices` with
  those from `vkEnumeratePhysicalDevices`. So panvk devices without a LUID should still be visible
  through the registry-ICD path.

## Not verified / needs hardware

- Everything on real ARM64 Windows:
  - DLL load
  - ICD registration under `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` with the real loader
  - device enumeration against panwin.sys
- Logical device creation, VM_BIND, group/queue creation, submission, syncobj waits and BO
  mmaps. None of these have run yet, even against the fake kernel.
- **EL0 cache maintenance on Windows ARM64.** `util/cache_ops_aarch64.c` uses `mrs ctr_el0` and
  `dc cvac`/`dc civac` from user mode. Unknown whether Windows allows this. The fallback is
  `PANVK_DEBUG=no_user_mmap_sync` (BO_SYNC ioctl), or WC/uncached mappings from the KMD.
- Win32 WSI CPU present (GPU blit to host-visible buffer + GDI StretchBlt). Compiled, but no
  surface or swapchain was ever created.
- Driver/cache UUID stability: it is based on the DLL file time.

## Known issues & TODO (prioritised)

1. **AI-policy conflict** (see Summary): a decision is needed. Nothing may be submitted upstream
   as-is.
2. **Fake kernel stops at VM_BIND.** Ask libdrm-win to implement VM_BIND, GROUP_CREATE/DESTROY,
   TILER_HEAP_CREATE/DESTROY, GROUP_SUBMIT (signal at submit) and GROUP_GET_STATE in the fake.
   Then `icd_smoke --create-device` becomes a full CPU-only integration test of panvk device
   init.
3. **User-space cache maintenance on Windows ARM64 is unknown.** Default to
   `PAN_KMOD_DEV_FLAG_MMAP_SYNC_THROUGH_KERNEL` on Windows if EL0 `dc` traps (one-line change in
   `panvk_physical_device.c`).
4. **No external memory, semaphore or fence handles on Windows** (fd variants disabled). The
   follow-up is `VK_KHR_external_*_win32` via NT handles; this needs a KMD/libdrm-win design. No
   `VK_EXT_map_memory_placed`: KMD-created mappings can't replace a reservation.
5. **pandecode** (`PANVK_DEBUG=trace/dump`) calls `os_mprotect` → `VirtualProtect` on
   KMD-created mappings, which will probably fail. This is debug-only.
6. **JM:** v6/v7 per-arch code and shaders are built in (about 2-3 MB), but there is no JM kernel
   backend on Windows. Either drop JM per-arch libs on Windows, or port `panfrost_kmod.c` (perf
   thread → Win32 events) when a JM KMD exists.
7. **ARM64X/ARM64EC builds** (so that x64-emulated apps can load the ICD) are not attempted. There
   is no 32-bit build either.
8. **`-mno-ms-bitfields` project-wide.** Windows SDK structs with mixed-type bitfields would get a
   non-MS layout if Mesa passed them to Windows APIs. None were found on the WSI/GDI paths used,
   but this was not exhaustively audited.
9. **No disk shader cache on Windows** (upstream `disk_cache_os.c` TODO). The build-id lld warning
   is cosmetic. `ld_args_build_id` could be skipped for COFF.
10. **Phase 2:** fold the libdrm-win routing (`pan_kmod_fd_*`, panthor Windows mmap) into a real
    `pan_kmod` Windows backend with a `util_sync_provider`, per research §5 option B.

## Requested contract changes

None. No shared header had to change.

Constraints the KMD/core owners must honour (already listed by libdrm-win, confirmed from the
Mesa side):

- **User MMIO offset on Windows.** On Windows (LLP64), Mesa computes `DRM_PANTHOR_USER_MMIO_OFFSET`
  = `1ull<<43` (the `_32BIT` variant).
  - With driver version >= 1.5, panthor_kmod sends `SET_USER_MMIO_OFFSET` with that value.
  - Below 1.5 it mmaps offset `1<<43` directly.
  - The KMD must accept both.
- **Flush-id mmap is unconditional.** panthor_kmod always maps the LATEST_FLUSH_ID page,
  read-only, one 4 KiB page. If `PANWIN_IOCTL_MMAP` of it fails, device creation fails.
- **Timeline syncobjs are mandatory.** On v10, panvk asserts `VK_SYNC_FEATURE_TIMELINE`, so the
  KMD must implement `SYNCOBJ_TIMELINE_WAIT` and the related ioctls from day one. At device
  probe, `SYNCOBJ_CREATE(SIGNALED)` + `SYNCOBJ_WAIT(timeout 0, WAIT_ALL)` must succeed.
- **The DRM version name must be `"panthor"`.** Features are gated on minor 1.1 / 1.2 / 1.4 / 1.5
  / 1.6 / 1.7 / 1.9; see `docs/research/mesa-panvk.md` §5.

## Review

Independent review, 2026-10-30. The series is now **14 patches** (0013 and 0014 were added by the
review). The rest of this file above describes the original 12.

### What was re-run, and the results

- **Clean cross build.** `BUILD_ROOT=references/mesa-build-review build.sh all` ran in brand-new
  build dirs: rc=0 for both arches.
  - Warnings: only the upstream `-Wmissing-braces` (`wsi_common_win32.cpp:584`) and the lld
    build-id warning, as reported.
  - aarch64 DLL: ARM64, 15.6 MB.
    - Imports: `libdrm.dll`, KERNEL32, USER32, GDI32, `api-ms-win-core-synch` and 11
      `api-ms-win-crt-*`.
    - Exports: the three `vk_icd*` entrypoints.
    - ICD json: `..\\..\\..\\bin\\vulkan_panfrost.dll`.
    - Precomp: the `libpan_shaders_v*` objects were generated and compiled.
  - x86_64 DLL: same imports and exports, AMD64.
- **Host tools were *not* rebuilt from scratch.** Their stamp was reused, so SPIRV-LLVM-Translator
  and `mesa_clc` came from the earlier build. `mesa_clc` resolves `libLLVMSPIRVLib.so.23.1` from
  the prefix.
- **`tests/run-wine-smoke.sh`.** All the claims held:
  - no device: interface 7, 13 instance extensions including `VK_KHR_win32_surface`,
    `vkCreateInstance` 0, 0 devices.
  - fake kernel: 1 device, "Mali-G610 MC4, Vulkan 1.4.366, 0x13b5/0xa8670005".
  - `--create-device`: `VM_BIND` returns ENOTTY, so `VK_ERROR_OUT_OF_DEVICE_MEMORY`.
- **Fresh checkout from the exported 14 patches** (`MESA_SRC=<tmp> build.sh checkout`): the tree
  hash is identical to `panwin` (50c7951f). The temporary worktree was removed afterwards.
- **Linux regression.** `references/mesa-build-host` was rebuilt with the fixes (0 warnings).
  `vulkaninfo --summary` with the panthor noop drm-shim still reports Mali-G610 MC4 / panvk /
  1.4.366, and `VK_KHR_external_semaphore_fd` is still exposed on Linux.
- **Default outputs.** `references/mesa-build-{aarch64,x86_64}` were rebuilt from the 14-patch
  series (rc=0), and the smoke test was re-run on them with the same results.
- **Checked by reading against the references:**
  - `DRM_PANTHOR_USER_MMIO_OFFSET` is `1ull<<43` on LLP64. Confirmed in `panthor_drm.h`
    (`sizeof(unsigned long) < 8`).
  - `panwin_mmap` and `PROT_*` values match `libdrm_win.h`.
  - Every panvk BO unmap site goes through `pan_kmod_bo_munmap`. The remaining `os_mmap` users
    are the placed-map paths, which are disabled on Windows.
  - libc++'s `std::string` bitfields are same-type, so their layout is identical with
    `-mno-ms-bitfields`.
  - The AI-policy quotes match `AGENTS.md` and `docs/drivers/panfrost.rst`.
  - No GPL-only code is in the series or in `userspace/mesa`.

### Bugs found and fixed

1. **fd external handle types were still advertised on Windows.** The `*_fd` and dma-buf
   extensions were off, but the core 1.1 queries still reported fd handle types:
   - `vkGetPhysicalDeviceExternalSemaphoreProperties` and `...FenceProperties` reported
     OPAQUE_FD/SYNC_FD as exportable and importable (features 0x3), because the runtime sync type
     still had the opaque-fd/sync-file hooks.
   - WSI also put them into `semaphore_export_handle_types`.
   - `vkGetPhysicalDeviceExternalBufferProperties` and the external image-format query reported
     OPAQUE_FD/DMA_BUF (features 0x6).
   - There is no way to get such an fd on Windows. This was reproduced with a new check in
     `tests/icd_smoke.c`: the fake-kernel run now fails if any fd handle type is advertised.
   - Fixed by two patches:
     - **0013** `vulkan/runtime`: no opaque-fd/sync-file import/export hooks in the DRM syncobj
       type on Windows. The sync-file helpers stay, because `vk_drm_syncobj_move` uses them.
     - **0014** `panvk`: `PANVK_EXTERNAL_MEMORY_FD_HANDLE_TYPES` is 0 on Windows.
   - After the fix, all features are 0x0 and the smoke test passes.
   - 0014 touches `src/panfrost`, so 6 of the 14 patches are now under the AI-policy question. It
     is a standalone patch and can be dropped.

### Remaining concerns, most severe first

1. **AI policy (process).** Confirmed as reported, and a lead/user decision is still needed.
   Everything is AI-written, including 0013/0014. The series must not be submitted upstream as-is.
2. **No device-level testing at all.** Nothing has run past `VM_BIND`: the fake kernel has no
   VM_BIND, group or tiler-heap ioctls. The ARM64 DLL has never been loaded. The real panwin.sys
   path is untested on hardware.
3. **EL0 cache maintenance on Windows ARM64 is unverified.** `dc cvac`/`civac` only work at EL0 if
   the OS sets SCTLR_EL1.UCI. Consider defaulting to `PAN_KMOD_DEV_FLAG_MMAP_SYNC_THROUGH_KERNEL`
   on Windows until this is tested on hardware.
4. **The v10 timeline-syncobj requirement is only an `assert`.** A release build on a KMD without
   timeline waits would continue and misbehave instead of failing cleanly. This is a KMD
   contract item.
5. **Patch 0006 changes behaviour for dozen upstream.** With no DXGI, dozen now silently uses the
   CPU path. Surface caps still report the DXGI image count and present modes, because they key on
   `get_d3d12_command_queue`, not on the factory. It is harmless for panvk, but needs care before
   it goes upstream.
6. **`vkAllocateMemory` still accepts OPAQUE_FD/DMA_BUF in `VkExportMemoryAllocateInfo` on
   Windows.** This is now app-invalid usage, because those types are no longer advertised. It is
   not fixed.
7. **The JM per-arch code is dead weight on Windows**, about 2-3 MB with no backend.
   `-mno-ms-bitfields` is still not audited against the Windows SDK structs. `PANVK_DEBUG=trace`
   uses `VirtualProtect` on KMD mappings.
8. **Cosmetic:** `host-tools/prefix/bin/llvm-spirv` has no rpath (Mesa doesn't use it), and there
   is the lld build-id warning.
