# Status: end-to-end userspace on the real core (key `e2e`)

Date: 2026-10-30. **Hardware-free. Untested on hardware.** x86_64 under wine 11.20 only; no
ARM64 binary was run.

## Summary

The whole userspace stack now runs against the **real kernel core**, in one x86_64 process under
wine:

```
vk_e2e.exe / icd_smoke.exe
  -> vulkan_panfrost.dll    unmodified panvk from our 14-patch series (no new Mesa patch needed)
  -> libdrm.dll             libdrm-win, new transport "core" (PANWIN_TRANSPORT=core)
  -> kmd/core               the real OS-neutral core, ioctl layer modelled on kmd/win/pw_win_ioctl.c
  -> kmd/tests mock G610    pw_os_mock.c + mock_csf.c (emulated MCU and CS executor),
                            loading the real mali_csffw.bin
```

**How far the Vulkan flow gets: all 14 steps pass**, repeatably (more than 10 runs plus a
30 000-submit stress run):

1. instance
2. enumerate: one "Mali-G610 MC4", API 1.4.366
3. vkCreateDevice with every queue: family 0 GFX|COMPUTE|TRANSFER|SPARSE ×2, family 1 SPARSE ×1,
   with timelineSemaphore, sync2 and sparseBinding enabled
4. host-visible+coherent memory allocate, map and bind
5. vkCmdFillBuffer, barrier, vkCmdCopyBuffer and vkCmdSetEvent recorded
6. vkQueueSubmit with a fence, then vkWaitForFences
7. **CS-executed data**: vkGetEventStatus is RESET before and SET after. All three panvk subqueues
   ran SYNC_SET32 on the mock CSF.
8. **timestamp query** (CS only: STORE_STATE, CS loops, linked-list updates): the result is
   available and non-zero
9. sparse bind and unbind on the bind-only queue (async VM_BIND), each fenced
10. timeline semaphore: the GPU waits for host value 1, a host wait times out first,
    vkSignalSemaphore(1), then vkWaitSemaphores(2) and the counter reads 2
11. stress: 1000 resubmits alternating over both GFX queues, each with a timeline signal, a host
    wait and an event round trip
12. an empty submit plus fence on every queue
13. vkDeviceWaitIdle
14. destroy everything; the core reports the device torn down with **0 leaks** (allocs, mems,
    sync objects, mappings, events), 0 mock model errors, 0 unclaimed IRQs and 0 CS_FATAL

Fill and copy *data* are not checked: panvk implements them with compute shaders, and the mock
treats RUN_COMPUTE as a no-op. As expected, the destination reads 0.

Mesa's own `icd_smoke --require-device --create-device` also passes on the core transport. On the
fake kernel it still stops at VM_BIND (ENOTTY), as before.

No bug was found in kmd/core, kmd/win or Mesa on this path. I found and worked around one
**mock bug** (bug report 1 below). Without the workaround, panvk gets VK_ERROR_DEVICE_LOST.

## What was built

| Path | What |
|---|---|
| `userspace/libdrm-win/src/transport_core.c` | Transport "core". It plays kmd/win's role in-process (see below). |
| `userspace/libdrm-win/include/panwin_core.h` | Env knobs and `panwin_core_get_report()`. The report is exported only by `-Dcore_transport` builds, through `__declspec(dllexport)`; `libdrm.def` is unchanged. |
| `userspace/libdrm-win/meson.build`, `meson_options.txt` | New options `-Dcore_transport=true` (x86_64 only) and `-Dcore_fw=`. The core sources (same list as `kmd/core/meson.build`) and the mock go in a static lib with `link_whole`. winpthread is linked statically, so the DLL still imports only system DLLs. |
| `userspace/libdrm-win/src/{fd.c,pw_internal.h}` | Register `pw_transport_core` when it is built. |
| `userspace/libdrm-win/tests/test_libdrm_core.c` | 80 checks through the libdrm API against the core (meson test `libdrm_core`). |
| `tests/e2e/vk_e2e.c` | The Vulkan flow (loader-less, like icd_smoke). |
| `tests/e2e/run.sh` | Builds what is missing, then runs everything. |
| `userspace/mesa/tests/run-wine-smoke.sh` | Adds a core-transport `--create-device` run when `out/x86_64-core` exists. |
| `kmd/tests/pw_os_mock.c`, `mock_gpu.h` | Windows portability, plus user-mapping support (see Contract changes). |

The transport reproduces panwin.sys ABI v2 (`kmd/win/pw_win_ioctl.c`):

- **DRM ioctls**
  - in ≤ 4096 and out ≤ in.
  - The struct is copied into a "system buffer" and copied back on success only.
  - Errors complete with `PANWIN_STATUS_FROM_ERRNO`.
- **GET_INFO**
- **MMAP / MUNMAP**
  - Whole mappings only, and each mapping holds a BO reference.
  - Uncached BOs: every mapping is a *distinct* `MapViewOfFile` of the pagefile section that
    backs the mock's "DRAM", so the user view aliases exactly what the emulated GPU reads and
    writes. Use-after-unmap faults, and read-only mappings are enforced.
  - LATEST_FLUSH_ID is a static zero page with `PAGE_READONLY`, and writable mappings of it are
    refused.
- **SYNCOBJ_EVENT**: the caller's event is `DuplicateHandle`d, set once, then closed. A cancel at
  close behaves like kmd/win's.
- **Device lifetime**: the first open runs mock + core + firmware load; the last close tears it
  down and runs the leak check.

Env knobs:

- `PANWIN_CORE_FW`: firmware path.
- `PANWIN_CORE_REPORT=1`: print the health report and unexpected ioctl errors.
- `PANWIN_CORE_TRACE=1`: print every DRM ioctl.
- `PANWIN_CORE_POISON=1`: repro for bug report 1.
- `PW_TEST_VERBOSE=1`: print the core log.

## How to build/test

```sh
# everything (libdrm-win core build + its meson tests, Mesa e2e build if missing, wine runs):
/home/e3xp0/Projects/panfrost-win/repo/tests/e2e/run.sh            # --rebuild forces libdrm + Mesa
# pieces:
cd /home/e3xp0/Projects/panfrost-win/repo/userspace/libdrm-win
meson setup ../../out/build/libdrm-win-x86_64-core . --cross-file cross/x86_64-w64-mingw32.ini \
      --prefix /home/e3xp0/Projects/panfrost-win/repo/out/x86_64-core --libdir lib \
      -Dfake_kernel=true -Dcore_transport=true
ninja -C ../../out/build/libdrm-win-x86_64-core && meson install -C ../../out/build/libdrm-win-x86_64-core
meson test -C ../../out/build/libdrm-win-x86_64-core --print-errorlogs
LIBDRM_WIN_PREFIX=/home/e3xp0/Projects/panfrost-win/repo/out/x86_64-core \
  BUILD_ROOT=/home/e3xp0/Projects/panfrost-win/references/mesa-build-e2e MESA_BUILDTYPE=debugoptimized \
  /home/e3xp0/Projects/panfrost-win/repo/userspace/mesa/build.sh x86_64
cd /home/e3xp0/Projects/panfrost-win/repo/out/e2e-x86_64      # filled by run.sh
PANWIN_TRANSPORT=core PANWIN_CORE_REPORT=1 WINEDEBUG=-all wine ./vk_e2e.exe
PANWIN_E2E_STRESS=30000 PANWIN_TRANSPORT=core WINEDEBUG=-all wine ./vk_e2e.exe
/home/e3xp0/Projects/panfrost-win/repo/userspace/mesa/tests/run-wine-smoke.sh
make -C /home/e3xp0/Projects/panfrost-win/repo/kmd/tests            # + CC=clang, + freestanding
```

The Mesa e2e build lives in `references/mesa-build-e2e-x86_64`: debugoptimized, so asserts are on.
Any x86_64 `vulkan_panfrost.dll` works, though, because `libdrm.dll` is linked dynamically. The
release build in `references/mesa-build-x86_64` also passes all steps.

## Verified

All of the following were run here.

- **`make -C kmd/tests`** (gcc, ASan+UBSan), **`make CC=clang`** and **`make freestanding`**: 96
  tests, 0 failed, 0 skips for both compilers; freestanding rc=0. Run after every mock change,
  including the last.
- **`userspace/libdrm-win/build.sh`** (arm64 + x86_64, no core transport): no warnings; meson test
  2/2 OK.
- **Core build:** `meson test` 3/3 OK (`libdrm_fake`, `libdrm_core`, `ioctl_map`). `libdrm_core`
  reports "OK: 80 checks, 0 failures", covering:
  - VERSION 1.6.0, caps, GPU_ID 0xa8670000
  - ENOENT/EINVAL propagation
  - the flush page is PAGE_READONLY, and its writable mapping is refused with EINVAL
  - BOs read zero
  - two mappings at distinct VAs alias the same pages
  - a CS on the mock CSF loads a CPU-written value through the GPU MMU and stores value+1, which
    the CPU then reads through both mappings
  - binary and timeline signal, SYNCOBJ_EVENT
  - an ETIME wait lasts ≥ 45 ms
  - host signal
  - an INT64_MAX wait woken by another thread
  - a mapping keeps the device alive after both fds are closed
  - clean teardown with 0 leaks and one cancelled event
- **libdrm.dll (core):** it imports only KERNEL32, ntdll, SETUPAPI and api-ms-win-crt-*. It has
  66 exports, including `panwin_core_get_report`.
- **Mesa x86_64 debugoptimized** against `out/x86_64-core` built rc=0. Warnings: the known
  upstream ones plus the lld build-id warning.
- **`tests/e2e/run.sh`:** PASS. vk_e2e passed 14/14, and icd_smoke `--create-device` printed OK.
  - In a typical run the core reports 7218 DRM ioctls. The only 2 failures are expected ETIMEs:
    the deliberate early timeline wait and one runtime probe.
  - The mock CSF executed about 226k CS instructions with 0 CS_FATAL.
- **Repeatability:** vk_e2e passed in more than 10 separate runs across the iterations (the
  last 4 with the final 14-step binary, one of them on the release Mesa DLL).
  - With `PANWIN_E2E_STRESS=30000`: 30000/30000 submits in 3.3 s, 210k ioctls, then a clean
    teardown.
- **Mesa smoke test:** `run-wine-smoke.sh` gave the same results as before for no device and for
  the fake kernel (fake `--create-device` still ENOTTY at VM_BIND). The new core run printed OK.
- **Negative control:** with `PANWIN_CORE_POISON=1`, test_libdrm_core fails its zero check and
  vk_e2e fails at step 6 with VK_ERROR_DEVICE_LOST (-4). So the tests do detect broken memory
  semantics.
- **Core log:** the only warning is `fw: protected mode section ... not supported, ignoring`.

## Not verified / needs hardware

- **Everything on hardware.**
  - The mock *emulates* the MCU. The real `mali_csffw.bin` is parsed and its sections are mapped
    by the core, but its code never runs.
  - CS semantics are only as good as `mock_csf.c`.
  - Cache effects are not modelled for WC memory.
  - LATEST_FLUSH_ID is always 0.
  - Timing is unrealistic.
- **Compute/fragment/tiler work.** The RUN_* instructions are no-ops, so fill/copy/dispatch/draw
  results are unverified. The tiler heap and OOM paths are not exercised by this flow.
- **kmd/win itself.** The transport mirrors `pw_win_ioctl.c`; it does not run it. That means no
  IRP/METHOD_BUFFERED behaviour, no MDL mappings, no `ObReferenceObjectByHandle`, and no user-mode
  alertable waits.
- **Not exercised:**
  - ARM64 binaries
  - WSI and swapchains
  - external memory
  - multi-threaded submission from several app threads
  - device-lost recovery
  - cached (WB_MMAP) BOs: the core reports DRM 1.6, so panvk never asks for them

## Known issues & TODO (prioritised)

1. **Mock ignores `PW_MEM_ZEROED`** (bug report 1). Recommended fix for the mock owner: make
   `honor_zeroed` the default.
2. **Data checks for compute work** need either a mock that executes shaders (out of scope) or
   hardware. Until then only CS-only operations (events, timestamp queries, sync objects) prove
   execution.
3. **When the core moves to DRM 1.7 (WB_MMAP), the e2e path changes.** panvk will map BOs cached
   and do user-space cache maintenance. On x86_64 that is clflush, which the mock's CPU-cache
   shadow (`mock_mem_user_view`) does not see. Run with `PANVK_DEBUG=no_user_mmap_sync` (BO_SYNC
   through the core) or teach the transport about it. Cached user mappings also share one VA for
   every mmap.
4. **Wine strips `VK_*` environment variables** (`VK_FOO=1 wine cmd /c echo %VK_FOO%` prints
   nothing). Loader-based tests under wine can't use `VK_DRIVER_FILES`/`VK_LOADER_DEBUG` this way.
   vk_e2e uses `PANWIN_E2E_STRESS`.
5. **The e2e Mesa build takes about 700 MB** in `references/mesa-build-e2e-x86_64`. Delete it when
   not needed.
6. **Not covered yet:** multi-threaded vk_e2e (concurrent submit/wait from several threads), a
   CS_FATAL/device-lost scenario through Vulkan, and an ARM64 run.

### Bug report 1 (mock, kmd/tests owner): `PW_MEM_ZEROED` not honoured

- **What happens:** `pw_os_mock.c` `pw_os_mem_alloc()` fills every allocation with 0xa5, including
  `PW_MEM_ZEROED` ones.
- **What the contract says:** `pw_os.h` says such pages "must read as zero through every CPU
  mapping and from the GPU". The core relies on that for user BOs (`pw_bo.c`, BO_CREATE) and for
  kernel BOs (`pw_sched.c`, `pw_heap.c`).
- **Repro:**

  ```sh
  cd out/e2e-x86_64
  PANWIN_CORE_POISON=1 PANWIN_TRANSPORT=core WINEDEBUG=-all wine ./vk_e2e.exe
  ```

  This fails at step 6 with `vkWaitForFences = -4` (DEVICE_LOST). The 0xa5 poison breaks panvk's
  assumption, true on Linux, that new BOs are zero. `test_libdrm_core.exe` with the same env fails
  its zero check.
- **Workaround here:** a `mock_config.honor_zeroed` knob, off by default so Linux host tests keep
  their exact behaviour.
- **Hardware impact:** none. kmd/win always zeroes.

## Contract changes

None to the contracts: `pw_os.h`, `pw_core.h`, `pw_file.h`, `pw_fence.h`, `pw_sync.h`,
`pw_bo.h`, `pw_sched.h`, `panwin_ioctl.h`, `panwin_transport.h` and the drm-uapi headers are
untouched.

Additions to test-only and owned interfaces:

- **`kmd/tests/mock_gpu.h`** (additions only; the defaults keep Linux behaviour identical, and
  the 96 tests still pass under gcc and clang):
  - `mock_config.honor_zeroed`: bug report 1.
  - `mock_mem_user_view()`, `mock_mem_user_map()` and `mock_mem_user_unmap()`: CPU views backing
    user mmaps. Uncached memory gives the GPU bytes themselves; cached memory gives a CPU-cache
    shadow that `pw_os_mem_flush`/`invalidate` keep in sync.
- **`kmd/tests/pw_os_mock.c`** portability: all of the following is under `_WIN32` only.
  - The `pw_os_time_ns` clock is QPC-based and computed like Mesa's `os_time_get_nano()`.
  - Monotonic absolute deadlines become relative winpthreads waits. winpthreads ignores
    `pthread_condattr_setclock`, and its absolute waits use the realtime clock.
  - Waits are sliced at 1 h, and early ETIMEDOUTs are treated as spurious.
  - `_aligned_malloc` replaces `aligned_alloc`, and a local `memmem` is added.
  - "DRAM" is a pagefile section, so user mappings can be distinct views.

  On Linux the same calls are made, through small wrappers.
- **libdrm-win:** a new public header `panwin_core.h`, and the `panwin_core_get_report` export in
  `-Dcore_transport` builds only.
