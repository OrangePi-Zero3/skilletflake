# Mesa userspace side: panvk / panfrost on Windows on ARM

Researcher scope: Mesa userspace (panvk, pan_kmod, compiler, Vulkan runtime, win32 WSI) and a real
cross-compile attempt for `aarch64-w64-mingw32`.

- Mesa clone: `/home/e3xp0/Projects/panfrost-win/references/mesa` (shallow, blob-less, `main` @
  `b39d00df594f` "ac/llvm: pass an LLVMContext to PassBuilder for LLVM 24", 2026-10-29, `VERSION` =
  `27.0.0-devel`). All paths below are relative to that tree unless absolute.
- Build experiments: `/home/e3xp0/Projects/panfrost-win/references/mesa-build-try/`
  - `cross-aarch64-w64-mingw32.ini` (meson cross file)
  - `stubs/` (fake `mesa_clc`, `vtn_bindgen2`, `panfrost_compile` so configure passes)
  - `build/` + `build-k0.log`: pass 1, pristine tree
  - `mesa-src/` (git worktree, branch `win-probe`, a one-line `os_mman.h` probe patch) +
    `compat-include/` (stand-in POSIX/libdrm headers, for probing only) + `build2/`, `build2.log`,
    `build3.log`: passes 2 and 3

Legend: **[V]** = I read the code or ran it myself. **[I]** = inference or unverified.

---

## 1. Layout of `src/panfrost` and the CSF vs JM split

### 1.1 Directory map [V]

| Path | What it is |
|---|---|
| `src/panfrost/lib/` | `libpanfrost_lib`, per-arch `libpan-arch-vN` (`pan_desc.c`, `pan_fb.c`, `pan_texture.c`, `pan_blend.c`, `pan_mod.c`, …) and per-arch `libpan-format-vN`. Image layout, AFBC/AFRC, descriptors, tiler, props (`pan_props.c`), and the model table. |
| `src/panfrost/lib/kmod/` | **pan_kmod**: the kernel-mode-driver abstraction. `pan_kmod.{c,h}`, `pan_kmod_backend.h`, `panfrost_kmod.c` (JM KMD plus the virtio/vpipe variants), `panthor_kmod.c` (CSF KMD), `virtio/panfrost_proto.h`. |
| `src/panfrost/compiler/` | NIR lowering passes shared across gens (`pan_nir_*.c`, `pan_compiler.c`), `bifrost/` (Bifrost v6/v7 **and** Valhall v9+ backend; `bifrost/valhall/` holds the Valhall ISA, packer and disassembler), `midgard/` (v4/v5), `kraid/` (**new Rust compiler**, `isa-v9-v14.xml`, `isa-v15.xml`, optional with `-Dpanfrost-rust`). |
| `src/panfrost/genxml/` | XML descriptor definitions `v4.xml` … `v14.xml` (no v8), `gen_pack.py`, `cs_builder.h` (CSF command-stream builder), `decode*.c` (pandecode; JM and CSF decoders). |
| `src/panfrost/libpan/` | OpenCL C helper kernels (`libpan_copy.cl`, `query_pool.cl`, `draw_helper.cl`, `indirect_dispatch.cl`), precompiled at build time with `mesa_clc` → `vtn_bindgen2` → `panfrost_compile`. |
| `src/panfrost/clc/` | `pan_compile.c` = the `panfrost_compile` precomp tool. |
| `src/panfrost/vulkan/` | **panvk**: common `panvk_*.c`, per-arch `panvk_vX_*.c`, and per-family subdirs `jm/`, `csf/`, `bifrost/`, `valhall/` (empty), `fifthgen/` (empty). |
| `src/panfrost/shared/`, `model/`, `perf/`, `ds/` (perfetto), `drm-shim/`, `tools/`, `ci/` | Tiling helpers, GPU model table (`pan_model.c`), perf counters, tracing, the noop DRM shim, tools. |
| `src/gallium/drivers/panfrost/` | Gallium GL/GLES driver: `pan_jm.c` (JM submit), `pan_csf.c` (2217 lines, CSF submit), `pan_cmdstream.c`, `pan_screen.c`, `pan_resource.c`, `pan_bo.c`, … |
| `src/gallium/winsys/panfrost/drm/` | `panfrost_drm_winsys.c` (kmsro/renderonly, dumb-buffer allocation). |

### 1.2 CSF (v10+) vs JM [V]

`src/panfrost/vulkan/meson.build:75-165`:
- `bifrost_archs = [6, 7]`, `valhall_archs = [9, 10, 11]`, `fifthgen_archs = [12, 13, 14]`
- `jm_archs = [6, 7]` → `jm/*.c`; `csf_archs = [10, 11, 12, 13, 14]` → `csf/*.c`
- panvk builds per-arch libs for **6, 7, 10, 11, 12, 13, 14**. v9 (Valhall-JM, G57/G77/G78) is **not**
  supported by panvk.
- CSF-only files: `csf/panvk_vX_{cmd_buffer,cmd_cond_render,cmd_dispatch,cmd_draw,cmd_event,cmd_query,cmd_precomp,event,exception_handler,gpu_queue,instr,utrace}.c`.
- JM-only files: `jm/panvk_vX_{cmd_buffer,cmd_dispatch,cmd_draw,cmd_event,cmd_query,cmd_precomp,event,gpu_queue}.c`.

Arch gating in `src/panfrost/vulkan/panvk_physical_device.c:455-509`:
- v10, v12 and v13 are enabled by default.
- v6, v7, v11 and v14 need `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1`.
- `if (arch != 10) vk_warn_non_conformant_implementation("panvk")`, so **v10 is the only arch reported
  conformant**.
- `get_api_version()` (`panvk_vX_physical_device.c:808-818`) returns **Vulkan 1.4 for PAN_ARCH >= 10**
  and 1.3 otherwise.

This strongly supports the RK3588 / Mali-G610 (v10, CSF) hypothesis: it is panvk's best-supported
target. [V for code; that G610 is v10 is standard knowledge.]

In Gallium, `pan_jm.c` covers v4-v9 and `pan_csf.c` covers v10+. Both call kernel ioctls directly (§2.3).

---

## 2. The pan_kmod abstraction

### 2.1 Ops table [V] (`src/panfrost/lib/kmod/pan_kmod.h:456-553`)

```
dev_create(int fd, flags, allocator)      dev_destroy
dev_query_user_va_range (optional)
bo_alloc(dev, exclusive_vm, size, flags)  bo_free
bo_import(dev, int fd)                    bo_export(bo) -> int fd
bo_mmap(bo, prot, flags, host_addr)       flush_bo_map_syncs(dev)
bo_wait(bo, timeout_ns, ro)               bo_make_evictable / bo_make_unevictable (optional)
vm_create(dev, flags, va_start, va_range) vm_destroy
vm_bind(vm, mode, ops[], count)           vm_query_state (optional)
query_timestamp                           bo_set_label
perf_create / perf_enable / perf_disable / perf_dump / perf_destroy (optional; panthor does not set them)
```

Other facts:
- `struct pan_kmod_dev` (`pan_kmod.h:582-624`) carries `int fd`, `props`, `ops`, and
  **`struct util_sync_provider *sync_ops`**. All `pan_kmod_sync_*()` helpers (`pan_kmod.h:1042-1131`)
  go through `sync_ops`, not straight to libdrm.
- `pan_kmod_ioctl(fd, op, arg)` is a macro for `drmIoctl()` (`pan_kmod.h:626-630`).
- The backends are tried in order (`pan_kmod.c:21-42`): `panfrost`, `panthor`, then under
  `HAVE_PANFROST_VDRM` `panfrost_virtio_gpu` and `panfrost_vpipe`. The env var
  `PAN_KMOD_RESTRICT_TO_BACKENDS` filters them. Each backend's `dev_create` probes with `drmGetVersion()`
  name matching (`pan_kmod_backend.h:78`).
- `pan_kmod_dev_init()` (`pan_kmod_backend.h:101`) **fails if `sync_ops == NULL`**. A new backend must
  therefore supply a `util_sync_provider`.
- `pan_kmod.h` itself includes `<fcntl.h>`, `<unistd.h>`, `<xf86drm.h>`, `drm-uapi/drm.h` and
  `util/os_mman.h`, so every panvk TU pulls in libdrm and mman headers.
- Virtio is precedent for a non-native backend: `panfrost_kmod.c:1490` `panfrost_virtio_gpu_ops` and
  `:1592` `panfrost_vpipe_ops`. Both are **JM only**. There is no panthor virtio backend.
  `src/virtio/vdrm/vdrm_vpipe.c:856+` implements a *non-DRM* `util_sync_provider` (vpipe_drm_sync_*).
  That is a direct template for a Windows provider.

### 2.2 `util_sync_provider` [V] (`src/util/u_sync_provider.h`)

It has 15 ops: `create, destroy, handle_to_fd, fd_to_handle, import_sync_file, export_sync_file, wait,
reset, signal, timeline_signal, timeline_wait, query, transfer, finalize, clone`.
`util_sync_provider_drm(fd)` is the libdrm implementation (`src/util/u_sync_provider.c`). It is a
`static inline` that returns NULL without `HAVE_LIBDRM`.

Users: panthor/panfrost kmod, `vk_device.sync` (`vk_device.h:241`, set by `vk_device_set_drm_fd()`
`vk_device.h:363`), radv amdgpu, turnip virtio, intel virtio, venus.

**Key finding:** `vk_drm_syncobj.c` already goes only through `device->sync->…` (40 call sites, no
direct `drmSyncobj*` calls). `vk_drm_syncobj_get_type_from_provider()` (`vk_drm_syncobj.c:659-704`) builds
the vk_sync type from *any* provider, and panvk uses exactly that:
`panvk_physical_device.c:373 vk_drm_syncobj_get_type_from_provider(device->kmod.dev->sync_ops)`.
A Windows KMD that exposes DRM-syncobj-like objects (u32 handles plus timeline points), with a Windows
`util_sync_provider`, therefore plugs into the existing panvk + Vulkan-runtime sync stack. Neither panvk
nor `vk_sync` would need to be rewritten.

### 2.3 Every place panvk/panfrost goes *around* pan_kmod [V]

Found by grepping for `drmIoctl|pan_kmod_ioctl|drm[A-Z]…(|DRM_IOCTL|syncobj|dma_buf|mmap|poll|close|dup|open|/dev/`.

**panvk CSF (what matters for RK3588).** All use `dev->drm_fd` (= `kmod.dev->fd`,
`panvk_vX_device.c:628`):
| File:line | Call |
|---|---|
| `vulkan/csf/panvk_vX_gpu_queue.c:557` | `DRM_IOCTL_PANTHOR_GROUP_SUBMIT` (init/sync submit) |
| `:686` | `DRM_IOCTL_PANTHOR_GROUP_CREATE` (struct `drm_panthor_group_create`/`queue_create`, `:641-674`) |
| `:704` | `DRM_IOCTL_PANTHOR_GROUP_DESTROY` |
| `:756` / `:791` | `DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE` / `_DESTROY` |
| `:1230` | `DRM_IOCTL_PANTHOR_GROUP_SUBMIT` (main queue submit) |
| `:1514` | `DRM_IOCTL_PANTHOR_GROUP_GET_STATE` (device-lost check) |
| `:336`, `:1057-1065`, `:1258` | `vk_sync_as_drm_syncobj()`. Assumes the vk_sync **is** a `vk_drm_syncobj` and takes its u32 handle to put in `drm_panthor_sync_op`. `:336` asserts `TIMELINE`. |
| `panvk_physical_device.c:382-383` | `assert(features & VK_SYNC_FEATURE_TIMELINE)` for arch >= 10. **Timeline syncobjs are mandatory on CSF.** |

Panthor-specific helpers called outside kmod (`lib/kmod/panthor_kmod.h`):
- `panthor_kmod_get_csif_props()`, called from `csf/panvk_vX_cmd_buffer.c:830`,
  `csf/panvk_vX_exception_handler.c:186`, `csf/panvk_vX_gpu_queue.c:402`, `csf/panvk_vX_cmd_draw.c:108`,
  `csf/panvk_vX_utrace.c:276` and `panvk_vX_device.c:551`.
- `panthor_kmod_get_flush_id()`, called from `csf/panvk_vX_gpu_queue.c:545,1033,1091,1108`. It reads a
  **user-mapped GPU register page** (LATEST_FLUSH_ID), which `panthor_kmod.c:310` maps with
  `os_mmap(…, fd, DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET)`.
- The Gallium `pan_csf.c` also uses `panthor_kmod_bo_{get,attach}_sync_point`, `vm_sync_{handle,lock,unlock}`.

**panvk JM (Bifrost v6/v7, not needed for RK3588):**
- `jm/panvk_vX_gpu_queue.c:81,125` `DRM_IOCTL_PANFROST_SUBMIT`
- `:168,173` `DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD/FD_TO_HANDLE`
- `:219`
- `jm/panvk_vX_event.c:28,50,69,99,115`: raw `DRM_IOCTL_SYNCOBJ_{CREATE,DESTROY,WAIT,SIGNAL,RESET}`.
  These bypass even the sync provider.

**Common panvk (all arches):**
- `panvk_physical_device.c:76-105` `create_kmod_dev()`: `open(drm_device->nodes[DRM_NODE_RENDER], O_RDWR|O_CLOEXEC)`.
- `:109-130` `get_drm_device_ids()`: `stat()` on render/primary nodes. `st_rdev` is used for
  `VK_EXT_physical_device_drm`.
- `panvk_vX_physical_device.c:12,1382-1388`: `<sys/sysmacros.h>`, `major()/minor()`.
- `panvk_instance.c:136,258`: enumeration only via `try_create_for_drm` (takes `drmDevicePtr`), so
  `vk_instance.c:413-462` calls `drmGetDevices2()`. The runtime also offers
  `instance->physical_devices.enumerate` (`vk_instance.h:158`), which is the hook a Windows build should use.
- `panvk_instance.c:218-268`: the pipeline-cache UUID comes from the **ELF build-id**
  (`build_id_find_nhdr_for_addr`). Windows has no ELF, so use `disk_cache_get_function_identifier()`, which
  has a `DETECT_OS_WINDOWS` branch and is what dozen does (`dzn_device.c:364`).
- `panvk_vX_device.c:509` `copy_sync_payloads = vk_drm_syncobj_copy_payloads`.
- `:516-517` `pan_kmod_dev_create(os_dupfd_cloexec(physical_device->kmod.dev->fd), …)`. The logical device
  dups the fd and relies on dup sharing the same DRM file, so GEM handles stay valid.
- `panvk_device_memory.c:197` `close(fd_info->fd)` (dma-buf import).
- `:369` `os_mmap(…MAP_FIXED|MAP_ANONYMOUS…)` and `os_munmap` (map_memory_placed / unmap-reserve).
- `:441-470`: DMA_BUF handle-type paths.
- `panvk_vX_physical_device.c:171`: `EXT_external_memory_dma_buf = true` (needs `#ifdef`).
- `panvk_android.c`: dma-buf (Android only).
- `panvk_wsi.c:31-38`: `drmGetDevice2()` / `drmFreeDevice()` in `can_present_on_device`.
- `panvk_image.c`, `panvk_meta.h:53,59` and about 26 sites use `vk_image.drm_format_mod`. panvk models
  *all* layouts (linear, u-interleaved, AFBC, AFRC) as DRM format modifiers.
  `vulkan/runtime/vk_image.h:83` compiles that field **only `#if DETECT_OS_LINUX || DETECT_OS_BSD`**.
- `panvk_bind_queue.c` and `panvk_async_bind.c:57-74`: `vk_sync_as_drm_syncobj()` → `pan_kmod_vm_bind`
  sync ops. These go through kmod, but with drm-syncobj handles.

**Inside `panthor_kmod.c` (would need porting or a shim):**
- `drmIoctl` for `DEV_QUERY` ×6, `SET_USER_MMIO_OFFSET`, `BO_CREATE`, `BO_QUERY_INFO`, `BO_MMAP_OFFSET`,
  `BO_SYNC`, `VM_CREATE`, `VM_DESTROY`, `VM_BIND`, `VM_GET_STATE` and `BO_SET_LABEL`.
- `drmSyncobj{Create,Destroy,Reset,TimelineWait,Transfer,ImportSyncFile,ExportSyncFile}`.
- `drmPrime{HandleToFD,FDToHandle}`, `drmCloseBufferHandle`, `drmGetVersion`.
- `os_mmap` of BOs (`:662`, offset from BO_MMAP_OFFSET) and of the flush-id page (`:310`, `getpagesize()`).
- `lseek(dmabuf_fd)` (`:548`).
- dma-buf implicit-sync dance: `DMA_BUF_IOCTL_{EXPORT,IMPORT}_SYNC_FILE` + `sync_wait()` (`:582-603`,
  `:680-703`, `:784-880`). This only runs for **shared (imported/exported) BOs**.
- Includes `util/libsync.h`, which pulls in `poll.h` and `sys/ioctl.h`.

**Total libdrm surface on the CSF path** (panthor_kmod + backend.h + u_sync_provider + vk_drm_syncobj +
vk_instance + panvk), about 25 functions:
`drmIoctl, drmGetVersion, drmFreeVersion, drmGetCap, drmGetDevices2, drmFreeDevices, drmGetDevice2,
drmFreeDevice, drmSyncobj{Create,Destroy,Wait,TimelineWait,Reset,Signal,TimelineSignal,Query2,Transfer,
HandleToFD,FDToHandle,ImportSyncFile,ExportSyncFile}, drmPrime{HandleToFD,FDToHandle}, drmCloseBufferHandle`.

**Gallium panfrost**, for completeness:
- `pan_csf.c:562,797,1981,2002,2116,2119,2144,2153`: GROUP_SUBMIT/CREATE/DESTROY, TILER_HEAP_*.
- `pan_jm.c:1128,1154,1163`: PANFROST_SUBMIT, `drmSyncobjWait`.
- `pan_context.c` (`poll.h`, `libsync.h`), `pan_bo.c`/`pan_mempool.c` (`sys/mman.h`), `pan_resource.c`
  (`xf86drm.h`), `winsys/panfrost/drm/panfrost_drm_winsys.c:72-120` (dumb buffers, `drmPrimeHandleToFD`).

---

## 3. Existing Windows support in Mesa

### 3.1 What builds on Windows [V]

- `meson.build:287-289`: with `vulkan-drivers=auto`, Windows/macOS/haiku get **no** Vulkan drivers
  ("No vulkan driver supports windows or macOS currently"). They must be selected explicitly.
- `system_has_kms_drm` is false for Windows (`meson.build:161`), so `dep_libdrm = null_dep`
  (`meson.build:804-818`). As a result:
  - `vk_drm_syncobj.c` (`vulkan/runtime/meson.build:58-60`) and `wsi_common_drm.c`
    (`vulkan/wsi/meson.build:8-9`) are not built.
  - `wsi_common_win32.cpp` is built (`vulkan/wsi/meson.build:30-31`).
- Drivers known to work on Windows:
  - **lavapipe** (`swrast` Vulkan, LLVM): `lvp_device.c:120 .KHR_win32_surface`.
  - **dozen** (`microsoft-experimental`, Vulkan on D3D12).
  - **gallium d3d12**, **llvmpipe**/**softpipe** and **zink** in the **WGL** target
    (`src/gallium/targets/wgl/meson.build:26-27` links `driver_swrast, driver_d3d12, driver_zink`).
  - Zink uses kopper with `VK_KHR_win32_surface` on Windows (`zink_kopper.c:78-80,115`). OpenGL via
    Zink-on-panvk on Windows is therefore already an existing code path. [V for code; not run.]
- RADV can be built on Windows only with its null winsys (the `with_amd_vk and not with_platform_windows`
  libdrm check at `meson.build:784`). No Mesa Vulkan HW driver talks to a Windows KMD today.

### 3.2 Win32 WSI [V] (`src/vulkan/wsi/wsi_common_win32.cpp`, 1161 lines)

There are two image paths:
- **DXGI path**, used only if `wsi->dxgi.factory && wsi->dxgi.dcomp &&
  wsi->wsi->win32.get_d3d12_command_queue` (`:980-985`). Swapchain images are `ID3D12Resource`s imported
  into the driver. Only dozen supplies `get_d3d12_command_queue`.
- **CPU path** (`WSI_IMAGE_TYPE_CPU`): GDI `CreateDIBSection` (`:581-601`). Present copies rows from
  `image->base.cpu_map` into the DIB and calls `StretchBlt` (`:825-856`).
  - For a non-`sw` device, `get_blit_type()` → `wsi_cpu_image_needs_buffer_blit()` selects
    `WSI_SWAPCHAIN_BUFFER_BLIT` (`wsi_common.c:405-416`, `wsi_configure_cpu_image` `:3637-3664`). The GPU
    blits the swapchain image into a host-visible linear buffer, which the CPU then copies to GDI.
  - **This works for panvk with no new WSI code.** The only needs are host-visible memory and
    `.KHR_win32_surface = true` under `#ifdef VK_USE_PLATFORM_WIN32_KHR` in `panvk_instance.c`. Today it
    only has the `#ifndef VK_USE_PLATFORM_WIN32_KHR` guard around `EXT_headless_surface` at `:130`.
  - Cost: one GPU copy plus one CPU memcpy plus a GDI blit per frame. That is fine for bring-up on unified
    memory. [I]
  - A later zero-copy path needs a D3D12/DXGI-importable handle. That is not possible without a WDDM
    driver. [I]

### 3.3 Sync on Windows [V]

- `vk_sync_timeline` (`vk_sync_timeline.c`) emulates timelines on top of a binary type.
  `vk_sync_binary` emulates binary on top of a timeline type.
- Dozen uses `dzn_sync_type` (native timeline, ID3D12Fence; `dzn_sync.c:311-319`) plus
  `vk_sync_binary_get_type(&dzn_sync_type)` (`dzn_device.c:1139-1144,1869`).
- Lavapipe uses CPU `lvp_pipe_sync_type` plus `vk_sync_timeline_get_type()` (`lvp_device.c:1533-1537`).
- The `vk_sync_type` vtable has `import_win32_handle/export_win32_handle` (`vk_sync.h:296-302`).
  `vk_semaphore.c`/`vk_fence.c` already implement `VK_KHR_external_{semaphore,fence}_win32` plumbing under
  `_WIN32`.
- Turnip's KGSL backend (`freedreno/vulkan/tu_knl_kgsl.cc:1371,1992`) shows a Mesa HW Vulkan driver with a
  non-DRM KMD and its own `vk_sync_type`. Its `tu_knl` vtable (`tu_knl.h:126+`) also abstracts
  submitqueue and submit, which `pan_kmod` does not.

**Does the Vulkan runtime assume DRM?** No, it does not assume DRM. [V]
- DRM-specific pieces are optional: `vk_drm_syncobj.c`, `wsi_common_drm.c`,
  `enumerate_drm_physical_devices_locked`, and `vk_image.drm_format_mod`.
- The runtime has Windows branches in `vk_device.c:866` (QPC time domain), `vk_semaphore.c`,
  `vk_fence.c` and `vk_instance.c:334,503` (`vk_icdEnumerateAdapterPhysicalDevices`).
- `vk_drm_syncobj.c` would compile on Windows with small changes. It includes `<xf86drm.h>`, `<sched.h>`
  and `util/libsync.h`, and calls `close()` on sync-file fds in the binary copy path. It is fed by a Windows
  `util_sync_provider`.

---

## 4. Compile blockers for aarch64-w64-mingw32: the actual attempt

### 4.1 Setup [V]

Cross file `references/mesa-build-try/cross-aarch64-w64-mingw32.ini`:
- c/cpp = `/opt/llvm-mingw/bin/aarch64-w64-mingw32-clang{,++}`
- `pkg-config=/usr/bin/false`
- host `windows/aarch64`

```
meson setup build ../mesa --cross-file cross-aarch64-w64-mingw32.ini \
 -Dvulkan-drivers=panfrost -Dgallium-drivers= -Dplatforms=windows -Dopengl=false -Degl=disabled \
 -Dgles1=disabled -Dgles2=disabled -Dglx=disabled -Dgbm=disabled -Dllvm=disabled -Dshared-glapi=disabled \
 -Dmesa-clc=system -Dprecomp-compiler=system -Dpanfrost-rust=false -Dvideo-codecs= -Dzstd=disabled \
 -Dexpat=disabled -Dbuild-tests=false -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
 -Dmicrosoft-clc=disabled -Dspirv-tools=disabled
```

- **Configure succeeds** with the stub tools in `PATH`. DirectX-Headers and zlib come in as subprojects.
  libdrm is not required by meson for panvk on Windows. `dep_libdrm` is just `null_dep`, and the failure
  only shows up at compile time.
- Meson's sanity check ran the cross `.exe`, which means **wine binfmt is active on this host**, so
  `meson.can_run_host_binaries()` is true. With `precomp-compiler=enabled`, `panfrost_compile` would be
  built as a Windows exe and run under wine.

### 4.2 Pass 1, pristine tree (`build-k0.log`, `ninja -k 0`): 148 failed edges [V]

These build cleanly for aarch64-w64-mingw32:
- `libmesa_util`, `libnir`, `libvtn`, `libcompiler`, `libxmlconfig` (with expat disabled)
- **`libvulkan_runtime`, `libvulkan_lite_runtime`, `libvulkan_instance`, `libvulkan_wsi` (including
  `wsi_common_win32.cpp`)**, `libvulkan_util`
- panfrost `shared`, `model`

Unique errors:
1. `include/drm-uapi/drm.h:51: fatal error: 'sys/ioccom.h' file not found` (62×).
   - Path: `pan_format.h` → `drm-uapi/drm_fourcc.h` → `drm.h`. The non-Linux branch of the kernel's drm.h
     wants BSD `sys/ioccom.h`.
   - Fix: provide `_IOC/_IOW/_IOWR` (a ~20-line compat header), or a `#elif defined(_WIN32)` branch.
2. `pan_kmod.h:25: fatal error: 'xf86drm.h' file not found` (21×, every panvk TU, plus `pan_kmod.c`,
   `panthor_kmod.c` and `perf/pan_perf.c`).
3. `genxml/decode.c:17`, `decode_common.c:13`: `'sys/mman.h' file not found`. pandecode uses
   `mmap/mprotect`.
4. `panfrost_kmod.c:8`: `'poll.h'` (JM KMD; later also `sys/eventfd.h`).
5. `midgard/midgard_compile.c:7`: `'err.h' file not found`.
6. **ABI issue:** static-assert failures from **MS bitfield layout**. llvm-mingw defaults to
   `-mms-bitfields` on Windows targets.
   - `compiler/pan_nir.h:282 sizeof(struct pan_bi_tex_flags) == 4`, `:323 sizeof(struct pan_va_tex_flags)
     == 4` (with `-Werror=padded`), `pan_nir_lower_tex.c` `sizeof(flags)==4`,
     `bifrost_compile.c:115 sizeof(bi_index) <= sizeof(uint64_t)`, and
     `sizeof(tex->backend_flags)==sizeof(flags)`.
   - Cause: mixed `bool:1` / `unsigned:3` / `enum:2` bitfields start new storage units under the MS ABI.
   - **Fix verified:** adding `-mno-ms-bitfields` makes `bifrost_compile.c` compile clean.
   - This flag should be set project-wide for panfrost. Mesa's other Windows builds are MSVC-ABI and have
     the same issue in principle, but those drivers don't static-assert this. [I]
   - Hardware descriptors use genxml pack functions, not C bitfields, so GPU-visible layouts are not
     affected. [V for genxml using pack helpers; I for "no other bitfield structs go to HW".]
7. Stub failures (expected): `src/panfrost/libpan/libpan_v{6,7,10..14}.spv`.

### 4.3 Passes 2/3 (`build2.log`, `build3.log`) with `-mno-ms-bitfields` + `compat-include/` [V]

- `compat-include/` holds `xf86drm.h` copied from host libdrm 2.4.134 plus a `uid_t/gid_t` typedef,
  `drm.h`/`drm_mode.h`, `sys/ioccom.h`, `sys/ioctl.h`, `sys/mman.h` (decls only), `poll.h` and `err.h`.
- Worktree patch: `src/util/os_mman.h:43` must accept `DETECT_OS_WINDOWS` (it has
  `#error Unsupported OS` otherwise).

**Results.** These all compile and archive:
- `libpanfrost_compiler.a`, `libpanfrost_bifrost.a` (Bifrost+Valhall backend),
  `libpanfrost_valhall_disasm.a`, `libpanfrost_midgard.a`
- all `libpandecode-arch-v*.a`, all `libpan-arch-v*.a`, `libpan-format-v*.a`, `libpanfrost_lib.a`,
  `libpanfrost_perf.a`

That means **the whole shader compiler and descriptor/layout library compile for Windows ARM64**. In
pass 2, `decode_common.c` needs `mprotect` (→ `VirtualProtect`).

Remaining real errors (pass 3):
| File | Error | Fix |
|---|---|---|
| `panvk_image.c`, `panvk_host_copy.c`, `panvk_async_bind.c`, `panvk_meta.h:53,59` | `no member named 'drm_format_mod' in 'struct vk_image'` | Widen the guard in `vk_image.h:83` (always keep the field), or have panvk keep its own modifier. |
| `panvk_instance.c:218-268` | `build_id_find_nhdr_for_addr`, `build_id_length`, `copy_build_id_to_sha1`, `BUILD_ID_EXPECTED_HASH_LENGTH` undeclared | Use `disk_cache_get_function_identifier()` on Windows (like dzn). |
| `panvk_physical_device.c` | `O_CLOEXEC` undeclared | `#ifdef`, or use the compat `open`. |
| `panvk_vX_physical_device.c:12` | `'sys/sysmacros.h' file not found` (`major/minor` for VK_EXT_physical_device_drm) | `#ifdef` the extension out on Windows. |
| `panthor_kmod.c` | `O_CLOEXEC` ×4, `getpagesize` ×2 | Small `#ifdef`s or a compat layer. |
| `panfrost_kmod.c` | `'sys/eventfd.h' file not found` | Drop the JM KMD from Windows builds, or `#ifdef` it. |
| all `panvk_vX_*` that include `libpan/libpan.h` | `'libpan_v10.h' file not found` | Generated by `vtn_bindgen2` from `mesa_clc` output (§4.4). |

Direct compilation of individual **v10** per-arch panvk TUs (`build2`, command from `ninja -t commands`):
- **Compile with 0 errors:** `csf/panvk_vX_gpu_queue.c`, `csf/panvk_vX_exception_handler.c`,
  `csf/panvk_vX_utrace.c`, `panvk_vX_shader.c`
- Blocked only by `drm_format_mod` or the generated `libpan_v10.h`: `csf/panvk_vX_cmd_buffer.c`,
  `csf/panvk_vX_cmd_draw.c`, `panvk_vX_device.c`, `panvk_vX_cmd_meta.c`
- Blocked by `sys/sysmacros.h`: `panvk_vX_physical_device.c`

No link was attempted. Linking will need real implementations of the ~25 libdrm functions plus
`mmap/munmap/mprotect/poll`.

### 4.4 Build-time host tool requirement (precomp) [V]

Per arch, `src/panfrost/libpan/meson.build` runs:
`mesa_clc` (OpenCL C → SPIR-V; needs host LLVM + **SPIRV-LLVM-Translator** + **libclc**) →
`vtn_bindgen2` → `panfrost_compile` (runs the Bifrost/Valhall compiler on the host).

The outputs (`libpan_shaders_vN.{c,h}`, `libpan_vN.{cpp,h}`) are GPU binaries and NIR builders, so they
are target-OS independent. On this machine LLVM 23.1.4 and SPIRV-Tools are present, but **LLVMSPIRVLib
and libclc are missing**, so `mesa_clc` cannot be built natively yet.

Recommended approach:
1. Build the three tools natively once (`-Dmesa-clc=enabled -Dinstall-mesa-clc=true
   -Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true`, `-Dvulkan-drivers=panfrost`). This needs
   spirv-llvm-translator and libclc, built from source into a user prefix or installed by the user.
2. Cross-build with `-Dmesa-clc=system -Dprecomp-compiler=system`.

`-Dpanfrost-rust=false` works: kraid is optional (`meson.options:927`, default **true**). If it is
wanted later, it needs a Rust `aarch64-pc-windows-gnullvm` target. Only `x86_64-unknown-linux-gnu` is
installed now.

### 4.5 Other Windows-specific risks [I unless marked]

- **User-mode cache maintenance:** `util/cache_ops_aarch64.c` uses `mrs ctr_el0`, `dc cvac` and
  `dc civac` from EL0 [V]. Whether Windows ARM64 sets SCTLR_EL1.UCT/UCI is unverified.
  - Fallback exists: `PAN_KMOD_DEV_FLAG_MMAP_SYNC_THROUGH_KERNEL` (`PANVK_DEBUG=no_user_mmap_sync`,
    `panvk_physical_device.c:88`) routes syncs through `DRM_IOCTL_PANTHOR_BO_SYNC` [V].
  - The question goes away if the KMD maps BOs uncached or write-combined, or the GPU is IO-coherent.
- **fd semantics:** Mesa uses `int fd` everywhere: `os_dupfd_cloexec` → `dup()` on Windows
  (`util/os_file.c:48-56` [V]), and `close()`.
  - With mingw/UCRT, `_open_osfhandle(HANDLE)` turns a device `HANDLE` into a CRT fd. `dup()` →
    `DuplicateHandle` keeps the same FILE_OBJECT, which matches Linux dup semantics where the GEM handle
    namespace is shared. [I, but standard Win32 behavior.]
- **mmap:**
  - BO CPU mapping becomes a KMD ioctl that maps pages into the caller's address space (MDL →
    `MmMapLockedPagesSpecifyCache(UserMode)`), or a section object plus `MapViewOfFile3`.
  - `VK_EXT_map_memory_placed` (`MAP_FIXED` over a reservation) maps to Win10 1803+ placeholder APIs
    (`VirtualAlloc2(MEM_RESERVE_PLACEHOLDER)` + `MapViewOfFile3(MEM_REPLACE_PLACEHOLDER)`), which only
    work with section-backed memory. The simplest choice is to not expose map_memory_placed on Windows
    at first.
- **LATEST_FLUSH_ID page:** either the KMD maps the register page read-only into user space, or the shim
  returns a constant. The latter loses flush elision; check panthor semantics before using 0. [I]
- **External memory / dma-buf:** turn it off on Windows. Expose `VK_KHR_external_memory_win32` later via
  NT handles. The dma-buf implicit-sync dance in panthor_kmod only runs for shared BOs, so it is not
  needed for bring-up.

---

## 5. Recommendation: the least-invasive way to run panvk on a custom Windows KMD

### Options compared

**A. libdrm shim ("libdrm-win") + Windows `os_mman` + DeviceIoControl transport.**
- Implement the ~25 xf86drm functions listed in §2.3:
  - `drmIoctl(fd, req, arg)` → `DeviceIoControl(_get_osfhandle(fd), MAP(req), arg, _IOC_SIZE(req), …)`.
  - `drmGetDevices2` → `CM_Get_Device_Interface_List(our GUID)`.
  - `drmSyncobj*` → our ioctls.
  - `drmPrime*` → `-ENOSYS` at first, NT handles later.
  - Plus `mmap`/`munmap`/`mprotect`/`poll`/`getpagesize`/`O_CLOEXEC` compat.
- KMD side: it implements `DRM_IOCTL_PANTHOR_*` with **byte-identical `drm_panthor_*` structs**, plus
  `DRM_IOCTL_SYNCOBJ_*` and `GEM_CLOSE`. Embedded user pointers (`drm_panthor_obj_array`) are probed in
  caller context, like `copy_from_user`.
- Mesa delta: about 10 small `#ifdef`s (§4.3) plus `vk_image.h` and build-id. `panthor_kmod.c`, the CSF
  queue code, `u_sync_provider.c` and `vk_drm_syncobj.c` stay essentially unmodified.
- Pros: fastest path to a working panvk, and it keeps the userspace that the KMD is validated against
  identical to Linux. Every panvk/panthor bug fix upstream applies directly. Mesa's own `drm-shim`
  (`src/panfrost/drm-shim/panfrost_noop.c`) is prior art for impersonating a DRM device.
- Cons: not upstreamable as is (Mesa won't take a fake libdrm on Windows). It ties the KMD to Linux ioctl
  numbering. It does not fit a future WDDM model, where submissions go through D3DKMT.

**B. New `pan_kmod` backend (`panthor_win` / `pan_kmod_wddm`) + Windows `util_sync_provider`.**
- Needs the 16 ops in §2.1, a sync provider (15 ops; `vdrm_vpipe.c:856+` is a template), and
  `pan_kmod_drm_drv_match` replaced by a DeviceIoControl version query.
- It must also lift the **CSF direct ioctls** into `pan_kmod_ops`: group create/destroy/submit/get_state,
  tiler heap create/destroy, `get_csif_props`, `get_flush_id`. JM would also need submit and raw syncobj
  ops, if JM is wanted at all.
- Pros: upstreamable shape, and it would also enable a future panthor-virtio backend. Clean for WDDM:
  - Escape / `D3DKMTSubmitCommand`, with panthor structs as private driver data.
  - Monitored fences map well onto timeline syncobjs.
- Cons: more panvk churn (the gpu_queue.c and device.c call sites). Having Mesa adopt it needs maintainer
  buy-in.

**C. vk_sync-level replacement** (own `vk_sync_type` like dzn/kgsl). This is **not recommended**. panvk
CSF code casts to `vk_drm_syncobj` (`vk_sync_as_drm_syncobj`) and asserts it in several places. Going
through `util_sync_provider` avoids touching any of that.

### Recommendation

**Do both, in order, with the same KMD wire format:**
1. **Phase 1 (bring-up):** Option A. A Windows-only `libdrm-win` static library plus a Mesa patch series
   of about 10-15 small `#ifdef _WIN32` hunks:
   - `-mno-ms-bitfields`
   - drm.h ioctl macros
   - `os_mman.h` Windows implementation
   - `vk_image.drm_format_mod` guard
   - build-id → function identifier
   - `KHR_win32_surface`
   - enumerate via `physical_devices.enumerate`, or the shim's `drmGetDevices2`
   - `#ifdef` out the dma-buf, physical_device_drm and JM KMD

   The KMD speaks panthor ioctls (same structs and semantics) plus DRM syncobj semantics, with timeline
   support **mandatory**. Present through the existing win32 WSI CPU path. GL comes from the Zink WGL
   target and D3D from DXVK/vkd3d-proton, all on top.
2. **Phase 2 (cleanup/upstream):** Fold the shim into a real `pan_kmod` Windows backend and a
   `util_sync_provider_win`. Move the CSF direct ioctls behind `pan_kmod_ops`, as an upstream refactor
   that is useful to Mesa regardless. If/when the KMD becomes WDDM, only this backend changes.

The KMD ABI should freeze on the **panthor uAPI as shipped in Mesa's `include/drm-uapi/panthor_drm.h`**
(1329 lines; ioctls DEV_QUERY … BO_QUERY_INFO, `panthor_drm.h:87-156`). The KMD reports a
`drmGetVersion` name of `"panthor"` plus a major.minor version. Feature gates in `panthor_kmod.c` [V]:

| Version | What it unlocks | Location |
|---|---|---|
| 1.1 | TIMESTAMP_INFO query | `:283`, `:1439` |
| 1.2 | GROUP_PRIORITIES_INFO | `:317` |
| 1.4 | BO_SET_LABEL | `:1464` |
| 1.5 | SET_USER_MMIO_OFFSET | `:298` |
| 1.6 | timestamp_device_coherent | `:204` |
| 1.7 | `is_io_coherent` from selected_coherency, `WB_MMAP`, BO_QUERY_INFO | `:207`, `:489` |
| 1.9 | MMU_INFO query, MAP_SPARSE | `:213`, `:267` |

The flush-id page mmap (`:310`) is **unconditional**. A KMD can start at 1.0-ish semantics and grow.

### Open questions

- Is EL0 `dc civac`/`mrs ctr_el0` allowed on Windows ARM64? This decides whether user-space cache
  maintenance is possible.
- Is RK3588's Mali IO-coherent (`DRM_PANTHOR_GPU_COHERENCY_*`)? If not, every host-visible mapping must be
  WC/uncached or rely on explicit syncs.
- Which panthor_drm.h version Mesa main requires at minimum, versus which features can be faked: the
  timestamp query, group priorities, and the BO_SYNC fallback.
- Setting up spirv-llvm-translator and libclc on this host for `mesa_clc`. The user may need to install
  them (for example the Arch `spirv-llvm-translator` and `libclc` packages), or we build them from source.
- Does the Vulkan loader on Windows pick up a non-WDDM ICD that is registered only under
  `HKLM\SOFTWARE\Khronos\Vulkan\Drivers`? Expected yes, since lavapipe works that way [I]. Per-adapter
  `vk_icdEnumerateAdapterPhysicalDevices` only matters for WDDM.
