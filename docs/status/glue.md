# Status: glue (kmd/win + libdrm-win on the v2 contracts, M3-M5)

Date: 2026-10-30. Owner key: `glue`. Owned: `kmd/win/**`, `kmd/meson.build`,
`kmd/meson_options.txt`, `toolchain/**`, `userspace/libdrm-win/**`, `tools/panwin-info/**`,
`docs/bringup/**`, this file, and the "M3-M5 update" sections of `kmd-win.md` / `libdrm-win.md`.

**Nothing here has run on Windows or on hardware.** Everything that depends on Windows kernel
behaviour is **untested on hardware**. What was actually run is listed under "Verified".

## Summary

### kmd/win (panwin.sys)

- **New `pw_os.h` functions** (`pw_os_win.c`, `pw_win_user.c`):
  - `pw_os_event_wait_user`: `KeWaitForMultipleObjects(UserRequest, UserMode, Alertable=TRUE)`.
    - It waits on the core event plus the abort event of the file whose ioctl the thread is
      running.
    - The absolute QPC deadline is turned into a relative 100 ns timeout, recomputed after each
      wake-up. A timeout is only reported once `pw_os_time_ns()` has passed the deadline.
    - `STATUS_USER_APC`/`STATUS_ALERTED` and the abort event give `PW_EINTR`.
  - `pw_os_copy_{from,to}_user`: `ProbeForRead/Write` + ntoskrnl `memcpy` inside `__try`.
    - The mode comes from `ExGetPreviousMode()`. User pointers must fit in a `ULONG_PTR`.
    - **Why SEH:** a user VA can be unmapped by another thread at any time. Probe+copy under an
      exception handler is the only correct primitive, and it is what the I/O manager and every
      system service do.
    - **Rejected alternatives:**
      - `MmIsAddressValid`: racy, and not a protection check.
      - `MmCopyMemory(VIRTUAL)`: read-only and does no probing.
      - MDL + `MmProbeAndLockPages`: also raises, so it still needs `__try`.
    - **clang check:** `__try` works on aarch64-w64-mingw32 as table-based SEH, and the scope
      table was checked (see Verified). clang only catches faults in *calls* inside the
      `__try`, which is why the copy is an out-of-line `memcpy`.
  - `pw_os_work_*`: one `IO_WORKITEM` per work on `DelayedWorkQueue`, plus a `KTIMER`+`KDPC`
    for delayed runs.
    - State is `pending`/`delayed`/`queued`/`running` under a spin lock.
    - The worker loops while an immediate run is pending. So "queue while running" runs it once
      more, and never twice in parallel.
    - `queue` on a pending delayed run makes it immediate (same as the mock).
    - `flush` turns a pending delayed run into an immediate one, then waits on an `idle` event.
    - `destroy` cancels the timer, calls `KeFlushQueuedDpcs`, flushes, then frees.
    - `IoQueueWorkItem` keeps the device object referenced while the work is queued.
  - `pw_os_event_create` stores the `pw_os`. `pw_os_mem` now always has an MDL; for contiguous
    memory it is `IoAllocateMdl`+`MmBuildMdlForNonPagedPool`. Accessors:
    `pw_os_win_mem_mdl`/`_cache`.
  - The new `PW_MEM_ZEROED` flag (added by core-mem) needs no change: pages are always zeroed.
- **`timer_hz`:** read from `CNTFRQ_EL0` (`mrs`) in DeviceAdd and passed in `pw_device_params`.
- **Ioctl design** (`pw_win_ioctl.c`): every request is executed and completed in
  `EvtIoInCallerContext`, in the requesting thread at PASSIVE, and is never queued. So:
  - user copies, user waits and user mappings run in the caller;
  - a long syncobj WAIT blocks only its own thread, so it cannot deadlock against a SIGNAL from
    another thread (a sequential queue would);
  - there is no power-managed queue holding requests.

  A non-power-managed parallel default queue only rejects read/write/internal requests.
- **Ioctl handling:**
  - **DRM** `PANWIN_IOCTL_DRM(nr)`: the METHOD_BUFFERED system buffer goes to
    `pw_file_ioctl(file, nr, buf, in_len)`. The rule is out ≤ in ≤ 4096. On success `out_len`
    bytes are copied back; on error nothing is.
  - **Errors** complete with `PANWIN_STATUS_FROM_ERRNO(-err)`. Unknown private ioctls give
    `STATUS_INVALID_DEVICE_REQUEST`.
  - **Removed:** the glue's VERSION/GET_CAP/DEV_QUERY handling and its `PW_DRM_VERSION_*`
    constants.
  - **Kept:** GET_INFO, which now takes the DRM version from `pw_core_drm_version`, and
    DEBUG_REG_READ.
  - **32-bit callers:** refused for `DRM_IOCTL_VERSION` only (its struct holds pointers).
- **Files:**
  - `EvtDeviceFileCreate` calls `pw_file_open`, under `files_lock` so it cannot race with
    D0Exit. Without a core device the handle still opens, and DRM ioctls give ENODEV.
  - Each file has a rundown reference, held by every in-flight ioctl, and an abort event.
  - In-flight ioctls are registered by thread id in `pw_os.active_list`. The record is in the
    nonpaged request context, because a UserMode wait can page the stack out. This is how
    `pw_os_event_wait_user` finds the right abort event.
  - **Teardown order** (EvtFileCleanup, and for every open file in
    `EvtDeviceD0ExitPreInterruptsDisabled`):
    1. Set abort, which ends user waits with EINTR.
    2. Wait for the rundown.
    3. Cancel the SYNCOBJ_EVENT notifies.
    4. Unmap the user mappings in the owning process (`KeStackAttachProcess` if needed).
    5. `pw_file_close`.
  - **Device stop:** D0ExitPreInterruptsDisabled also drains the device-level users (GET_INFO,
    DEBUG_REG_READ) through `core_rundown`, while IRQs still work. D0Exit then destroys the
    core.
  - Revoked files stay open, and their ioctls give ENODEV ("device lost").
- **MMAP/MUNMAP:**
  - `pw_file_mmap_lookup` resolves the offset. The pages are mapped with
    `MmMapLockedPagesSpecifyCache(UserMode)` under `__try`, using `NormalPagePriority |
    MdlMappingNoExecute | (no PROT_WRITE ? MdlMappingNoWrite : 0)`.
  - **BO:** a partial MDL (`IoBuildPartialMdl`) of the `pw_os_mem` MDL, mapped with the cache
    type the pages were *allocated* with. ARM64 forbids mismatched aliases, and a disagreement
    with `t.cached` is logged.
  - **MMIO:** `IoAllocateMdl` over our own `MmMapIoSpaceEx` mapping of the window, then
    `MmBuildMdlForNonPagedPool` and `MmNonCached` (the same Device attribute as the kernel
    mapping), plus `MdlMappingNoWrite`. No `\Device\PhysicalMemory` section is used.
    `MmProtectMdlSystemAddress` only applies to system VAs, so it cannot make a *user* view
    read-only. The read-only protection therefore relies on `MdlMappingNoWrite`.
  - PROT_WRITE on a `read_only` target gives EINVAL, as in `panthor_device_mmap_io`
    (panthor_device.c:453).
  - Mappings are kept per file and only allowed from the process that opened the file. MUNMAP
    needs an exact (va, size) match. The BO reference is dropped after unmapping.
- **SYNCOBJ_EVENT:**
  - The handle is checked with `ObReferenceObjectByHandle(EVENT_MODIFY_STATE,
    *ExEventObjectType, RequestorMode)`, then passed to `pw_file_syncobj_notify`.
  - The callback does `KeSetEvent` + `ObDereferenceObject`, and its last access is a store to
    `fired`. It never touches the file, so the callback does not depend on the file's lifetime.
  - Fired records are freed by the next SYNCOBJ_EVENT on the file, or at cleanup. There is a
    cap of 16384 pending notifies per file (ENOSPC).
  - Cleanup cancels the rest. If cancel loses the race, it waits for `fired`.
- **`pw_core_stub.c`** implements the per-file API for `-Dcore=stub`:
  - VERSION (`panthor` 1.0.0) and GET_CAP, as the glue used to.
  - DEV_QUERY GPU_INFO, with panthor uobj semantics, through `pw_os_copy_to_user`.
  - CSIF_INFO gives ENOSYS.
  - `pw_file_mmap_lookup` returns the read-only user MMIO page (GPU+0x10000,
    `CSF_GPU_LATEST_FLUSH_ID`, panthor_fw_regs.h:24) for both MMIO offsets. This lets the MMIO
    mapping path be tested on hardware without the real core.
  - No BOs and no syncobjs.

### libdrm-win

- **Errors:** decoded with `PANWIN_STATUS_IS_ERRNO`/`TO_ERRNO`, then Linux errno → CRT errno.
  ETIME and ETIMEDOUT stay distinct. Other NTSTATUS codes keep the generic table.
- **Overlapped handle:** the device is now opened `FILE_FLAG_OVERLAPPED`, and each ioctl uses a
  per-thread auto-reset event (FLS, closed at thread exit) and waits if the request pends.
  - **Bug found:** on a synchronous handle the I/O manager serialises every request of the file
    object. A SYNCOBJ_WAIT in one thread would then block the SIGNAL from another thread on the
    same fd, which is the deadlock the kernel design avoids.
- **SYNCOBJ_EVENT:** `panwin_syncobj_event()` creates a manual-reset event with
  `CreateEventW(NULL, TRUE, FALSE, NULL)` and passes its handle (in-only, out_size 0). It closes
  the event on failure, and the caller owns it.
- **Timeouts:** passed through unchanged as absolute nanoseconds on the QPC clock. Mesa's
  Windows `timespec_get(TIME_MONOTONIC)` (`c11/impl/time.c:70-84`) computes `sec*1e9 +
  (rem*1e9)/freq` from `QueryPerformanceCounter`. The kernel's `pw_os_time_ns` uses the same
  formula on `KeQueryPerformanceCounter`. An EINTR restart reuses the same absolute deadline.
- **ABI v2 handshake:** `panwin_open` issues GET_INFO and fails with `EPROTO` unless
  `abi_version == PANWIN_ABI_VERSION` (2). `PANWIN_O_NOABICHECK` opts out; panwin-info uses it
  so it can print the mismatch.
- **Pluggable transports:** the new public header `include/panwin_transport.h`.
  - The versioned table's `ioctl` returns the raw NTSTATUS, so every transport goes through one
    decode path.
  - There is a registry with `panwin_register_transport`/`panwin_select_transport`.
  - `PANWIN_TRANSPORT=<name>` or `dll:<path>` selects one. The `dll:` form calls the export
    `panwin_transport_entry`. `PANWIN_FAKE_KERNEL=1` is kept as an alias.
  - This is the slot for the later in-process real-core transport. It is not built yet.
- **Fake kernel on ABI v2:**
  - errors use `PANWIN_STATUS_FROM_ERRNO`;
  - SYNCOBJ_EVENT `DuplicateHandle`s the caller's event (EBADF if invalid), and gives EINVAL
    without WAIT_FOR_SUBMIT when there is no fence;
  - MUNMAP needs an exact size;
  - `pad` is checked.
- **panwin-info:**
  - `--transport NAME`;
  - opens with NOABICHECK;
  - maps the user MMIO page and prints LATEST_FLUSH_ID.

## How to build/test

```sh
cd /home/e3xp0/Projects/panfrost-win/repo
# kmd (any build dir works; these are the ones used here)
meson setup build-kmd kmd --cross-file toolchain/meson/aarch64-w64-kmd.ini -Dcore=stub   # or -Dcore=real
ninja -C build-kmd && meson test -C build-kmd
meson setup build-kmd-rel kmd --cross-file toolchain/meson/aarch64-w64-kmd.ini -Dcore=real \
      -Dbuildtype=release -Ddebug_ioctls=true && ninja -C build-kmd-rel && meson test -C build-kmd-rel
# SEH scope-table check: prints each function's __try ranges; disassemble them with
# llvm-objdump --start-address/--stop-address and check the guarded calls are inside
python3 -I toolchain/scripts/seh_scopes.py build-kmd/panwin.sys build-kmd/panwin.map \
      pw_os_copy_from_user pw_os_copy_to_user pw_win_copy_from_user pw_win_copy_to_user pw_win_map_user
# libdrm-win + panwin-info, arm64 + x86_64, x86_64 tests under wine
userspace/libdrm-win/build.sh
cd out/x86_64/bin && WINEDEBUG=-all wine ./panwin-info.exe --fake
# core host tests (core owners' code)
make -C kmd/tests && make -C kmd/tests CC=clang && make -C kmd/tests freestanding
```

## Verified (run here, 2026-10-30 ~03:25)

- **kmd builds:** clean rebuilds with `ninja -t clean` of three configurations. All give **0
  warnings** at `-Wall -Wextra -Wmissing-prototypes -Wstrict-prototypes -Wshadow -Wvla
  -Wframe-larger-than=2048`, and `meson test` passes **2/2** in each:

  | Build | Image size |
  |---|---|
  | `core=stub` (debugoptimized) | 57344 B |
  | `core=real` (debugoptimized) | 110592 B |
  | `core=real` (release, debug_ioctls) | 118784 B |

  - The `core=real` build was rebuilt again at 03:23 against the core tree as it was then: it
    links with no undefined symbols. That core still has stub `pw_bo.c`/`pw_sched_stub.c` and a
    minimal `pw_file.c`.
  - `pw_os.h` gained `PW_MEM_ZEROED` during the session. Every function declared in `pw_os.h`
    is defined in `pw_os_win.c`/`pw_win_user.c`, checked by grep.
- **Imports:** `petool check-sys` reports "69 imports from 3 DLLs, 0 errors, 0 warnings". Every
  import is in `toolchain/lib/*.def`, and none is missing from the ARM32 reference lists.
  - New since M2: KeWaitForMultipleObjects, Io{Allocate,Queue,Free}WorkItem,
    KeInitializeTimerEx, KeSetTimer, KeCancelTimer, KeInitializeDpc, KeFlushQueuedDpcs,
    KeAcquireSpinLockRaiseToDpc, KeReleaseSpinLock, Ke{Acquire,Release}SpinLock{AtDpcLevel,
    FromDpcLevel}, Ex{Initialize,Acquire,Release,ReInitialize}RundownProtection,
    ExWaitForRundownProtectionRelease, IoAllocateMdl, IoFreeMdl, IoBuildPartialMdl,
    MmBuildMdlForNonPagedPool, KeStackAttachProcess, KeUnstackDetachProcess,
    IoGetCurrentProcess, ObfReferenceObject, ObfDereferenceObject, ObReferenceObjectByHandle,
    `ExEventObjectType` (DATA), ExGetPreviousMode, PsGetCurrentThreadId.
  - All of these were already in the approximate `ntoskrnl.def`, so **no .def change was
    needed**. They are still approximate (see TODO 1).
- **No x18:** `llvm-objdump -d | grep -wE 'x18|w18'` finds 0 matches.
- **SEH lowering (static):** I decoded the `.xdata` C_SCOPE_TABLEs with
  `toolchain/scripts/seh_scopes.py` (new).
  - `pw_os_copy_from_user` (core=stub image; core=real has the same shape at other
    addresses): the `__try` range `[0x1400080a4, 0x1400080cc)` contains `blr
    __imp_ProbeForRead` and `bl memcpy`, with filter 1 (EXECUTE_HANDLER). The landing pad
    returns -14 (EFAULT).
  - `pw_os_copy_to_user`, `pw_win_copy_{to,from}_user` and `pw_win_map_user` likewise. In
    `pw_win_map_user` the range covers `blr __imp_MmMapLockedPagesSpecifyCache`, and the
    landing pad returns NULL.
  - The handler is the `__C_specific_handler` import thunk.
  - The `__try` range ends exactly at the return address of the last call. That is correct only
    if the ARM64 dispatcher applies `ControlPcIsUnwound` (PC-4), as it does for MSVC code.
    Runtime behaviour is **untested on hardware**.
- **libdrm-win:**
  - `build.sh` builds arm64 + x86_64. After `ninja -t clean`, both libdrm.dll and both
    panwin-info.exe rebuild with **0 warnings**.
  - `meson test` under wine passes **2/2**: `libdrm_fake` "OK: 198 checks, 0 failures" (up from
    161), and `ioctl_map` OK.
  - The new checks cover:
    - errno-status decode (ETIME vs ETIMEDOUT, EINTR, ENODEV, EBADF, the 4095 boundary);
    - SYNCOBJ_EVENT: EINVAL without WAIT_FOR_SUBMIT, an already-signalled point, and later
      signalling;
    - transport registry: EINVAL/EEXIST/ENOENT/EBUSY;
    - the ABI handshake: EPROTO for ABI 1, and the NOABICHECK opt-out;
    - injected-transport ioctl routing;
    - the `dll:` loader, with a test transport DLL built by meson.
- **panwin-info under wine:**
  - `--fake` prints the full decode plus "user MMIO page: LATEST_FLUSH_ID = 0x00c0ffee (mapped
    read-only ...)".
  - `PANWIN_TRANSPORT=fake` and `--transport fake` select the fake. `PANWIN_TRANSPORT=bogus`
    warns and falls back to real.
  - `PANWIN_DEVICE=Z:<regular file>` goes through the real overlapped NtDeviceIoControlFile path
    with a per-thread event: each ioctl fails cleanly with errno 130 (EOPNOTSUPP), and nothing
    hangs.
- **Core host tests:** `make -C kmd/tests` (gcc and clang) **FAIL**. ASan reports a
  `global-buffer-overflow` in the core tests. This is core code that other owners are writing in
  parallel; I did not touch it.
  - `make freestanding` compiles every core source.
  - Its "undefined symbols" list shows the core's cross-module references plus the `pw_os_*`
    set, and all of the `pw_os_*` ones are provided by kmd/win.

## Not verified / needs hardware

Everything at run time, in particular:

- **Calling contexts:** that `EvtIoInCallerContext` runs at PASSIVE in the caller's thread for
  user DeviceIoControl, and that completing every request there is fine for KMDF 1.15 on ARM64.
- **SEH at run time:** whether clang's aarch64 SEH tables are actually honoured by the ARM64
  kernel dispatcher, including the ControlPc-4 point above.
- **Interrupted waits:** that thread termination or a user APC ends the UserMode alertable wait
  with `STATUS_USER_APC`/`STATUS_ALERTED`. This is my reading of the exit-APC re-queue
  behaviour, not documented API.
- **Read-only user mappings:** that `MdlMappingNoWrite` is honoured for **UserMode** mappings.
  If it is not, the user MMIO page and PROT_READ BOs would be writable. Test by writing to the
  flush-ID page.
- **MMIO MDL:** that `MmBuildMdlForNonPagedPool` accepts a range of an `MmMapIoSpaceEx` mapping
  (I/O space, not pool) and that `MmMapLockedPagesSpecifyCache(UserMode, MmNonCached)` maps it.
  This is the commonly used technique, but it is not documented for I/O space.
- **Partial MDLs:** `IoBuildPartialMdl` on `MmAllocatePagesForMdlEx` MDLs (VA base 0) and
  mapping the partial MDL into user space.
- **Attach and unmap:** `KeStackAttachProcess` + `MmUnmapLockedPages` when cleanup or D0Exit
  runs outside the owning process.
- **Work items:** the work-item and timer state machine under real concurrency. The mock has
  different internals, and no Windows-side test exists.
- **Overlapped handles:** that FILE_FLAG_OVERLAPPED plus completion inside the dispatch means
  no request pends, and that concurrent ioctls on one handle really run in parallel.

## Known issues & TODO (prioritised)

1. **Import list is approximate:** regenerate the `.def`s from the board's ntoskrnl/hal/WDFLDR
   on day one (toolchain/README.md). The new spin-lock-at-DPC, rundown, `ExEventObjectType` and
   `KeStackAttachProcess` imports should be checked on ARM64 specifically.
2. **Handle duplicated into another process:** if a device handle is duplicated into another
   process and the creator exits first, the creator's user mappings stay until the last handle
   is cleaned up, after its address space is gone. libdrm-win never does this, and MMAP is
   limited to the creating process. The fix is `PsSetCreateProcessNotifyRoutine` that unmaps a
   dying process's mappings, or refusing `DuplicateHandle` with an ObRegisterCallbacks filter.
3. **Core notify contract:** `pw_file_syncobj_notify_cancel()` must accept a cookie whose notify
   has already fired. The race is unavoidable for the OS layer; the cookie must stay valid until
   cancel or `pw_file_close`. `pw_file_syncobj_notify` must not call `fn` when it returns an
   error. The glue assumes both; see "Contract changes", requested.
4. **Long waits and CancelIo:** they cannot be cancelled by `CancelIoEx`, only by handle close,
   device stop, thread termination or a user APC. Consider `WdfRequestMarkCancelableEx` (it
   needs a driver-owned request from a queue, so it would need a redesign) or an
   `IRP_MJ_CLEANUP`-only model.
5. **`pw_file_ioctl` size handling:** the core requires `size == _IOC_SIZE` exactly. Linux
   `drm_ioctl` zero-extends shorter and truncates longer structs, so a Mesa built against an
   older or newer uAPI header fails with EINVAL here. This is the core's decision; the glue
   passes `in_len` unchanged.
6. **Timer resolution:** about 15.6 ms, and it also affects `pw_os_work_queue_delayed` and
   short user timeouts (see kmd-win.md TODO 2).
7. **SYNCOBJ_EVENT records:** fired records are swept only on the next SYNCOBJ_EVENT or at
   cleanup. That is bounded by the 16384 cap, but a file that stops calling SYNCOBJ_EVENT holds
   up to that many small records until close.
8. **Core host tests:** they currently fail under ASan. This is for the core owners.
9. **Revoked files:** the client sees ENODEV forever, and there is no "device lost" event or
   query. The core could also signal pending fences with an error at revoke.
10. **Older drivers:** libdrm-win's generic NTSTATUS table still contains the ABI v1 standard-code
    mapping. That is harmless, but it can go once no v1 driver exists.

## Contract changes

**None to the shared contracts.** `pw_os.h`, `pw_core.h`, `pw_file.h`, `panwin_ioctl.h` and the
drm-uapi headers were not edited. Component-local API added (owned by me):

- `userspace/libdrm-win/include/panwin_transport.h` (new, installed): the
  `struct panwin_transport` v1 table, `panwin_register_transport`, `panwin_select_transport`
  and `PANWIN_TRANSPORT_ENTRY`. `panwin_transport_name` moved here from `libdrm_win.h`, which
  includes it.
  - **Reason:** the injectable transport for the coming real-core test transport.
- `libdrm_win.h`: `PANWIN_O_NOABICHECK` for `panwin_open`.
  - **Reason:** the ABI v2 handshake, with diagnostics still possible.
- **Exports** (`libdrm.def`): added `panwin_register_transport` and `panwin_select_transport`.
- **Internal:** `struct pw_transport` became `struct panwin_transport`, and its `ioctl` now
  returns the NTSTATUS instead of `-errno`, so every transport shares one decode path.

Requested, for the core owners. These are clarifications or additions to `pw_core.h`/`pw_os.h`;
none of them was applied:

1. `pw_file_syncobj_notify_cancel()`: document that it may be called with the cookie of a
   notify that has already fired (it returns false), and that the cookie stays valid until
   cancel or `pw_file_close()`. Also state that `fn` is never called when
   `pw_file_syncobj_notify()` fails.
2. `pw_os.h`: add `PW_ENODEV (-19)` and `PW_EBADF (-9)`. The glue uses local `PW_WIN_ENODEV` /
   `PW_WIN_EBADF`.
3. `pw_os_event_wait_user`: document that `PW_EINTR` is also returned when the file is being
   closed or the device stops (the glue's abort event). The core should then unwind and
   return `PW_EINTR`.

## Review

Independent review, 2026-10-30 (~03:25-04:30). Everything below was re-run from clean by the
reviewer; nothing ran on Windows or hardware.

### Re-ran

| What | Result |
|---|---|
| `meson setup` + `ninja` + `meson test`: core=stub, core=real, core=real release+debug_ioctls (fresh build dirs, and again after `ninja -t clean` once the fixes below were in) | 0 warnings each, `meson test` 2/2 each; `petool`: 69 imports from 3 DLLs, 0 errors |
| `llvm-objdump -d \| grep -wE 'x18\|w18'` | 0 in all three images |
| `seh_scopes.py` on the core=stub image + disassembly of `pw_os_copy_from_user` | `__try` range covers `blr ProbeForRead` and `bl memcpy`, except target returns -14. Claim holds (runtime SEH still untested on hardware) |
| `userspace/libdrm-win/build.sh` after deleting `out/build/{libdrm-win,panwin-info}-*` | 0 warnings; wine `meson test` 2/2, `libdrm_fake` "OK: 198 checks, 0 failures"; `panwin-info --fake` prints the full decode |
| `make -C kmd/tests`, `CC=clang`, `freestanding` | **pass** now (72, later 78 tests, 0 failed); freestanding compiles. The ASan failure the engineer reported was fixed by the core owners in the meantime |
| New `make -C kmd/win/tests` (gcc), `CC=clang`, `SAN=thread CC=clang` | 9 tests, ~9-12k checks, 0 failures; TSan 0 reports (3 runs) |

### New test harness: `kmd/win/tests/` (Makefile, fake_wdk.c/.h, wdk_shim/, test_glue.c)

The real `pw_win_ioctl.c`, `pw_os_win.c`, `pw_win_user.c` and `pw_fmt.c` are compiled on the Linux
host against a pthread fake of the NT/KMDF subset they use (`wdk_shim/ntddk.h`, `wdf.h`;
dispatcher-lock model for KEVENT waits, alertable UserMode waits with injectable user APCs,
rundown refs, KTIMER+KDPC on a timer thread, IO_WORKITEMs as threads, MDL user mappings tracked
per fake process, SEH emulated with setjmp/longjmp) and linked with the REAL `kmd/core`.
The fake counts misuse (recursive FAST_MUTEX, fast mutex outside a critical region, double
completion, IO_WORKITEM double queue/free while queued, over-dereference, alertable UserMode
wait inside a critical region) and the run fails on any. Tests:
`dispatch_basics` (GET_INFO, unknown ioctls, out>in, ETIME on an absolute QPC deadline, EFAULT
through the SEH path, copy-back only on success), `wait_vs_signal` (WAIT in one thread, SIGNAL in
another, 20x: no deadlock), `wait_user_apc` (EINTR, then restart works), `cleanup_ends_waits`
(3 waiters -> EINTR on handle cleanup, then ENODEV, process reference balanced), `revoke`
(D0Exit path), `syncobj_event` (EINVAL/EBADF/ENOENT paths keep the event reference balanced,
pending -> set on signal, already signalled -> set synchronously, pending at close -> cancelled
and dereferenced), `syncobj_event_races` (400 registrations x 5 rounds against a signalling
thread, close while firing), `mmap` (argument checks, partial mappings, whole-mapping MUNMAP,
other-process EACCES, mapping keeps the BO alive past GEM_CLOSE, cleanup from another process
attaches and unmaps in the creator, LeakSanitizer sees the BO freed), `work` (queue/flush,
queue-while-running = one more run and never concurrent, self re-queue, delayed not-before,
flush promotes delayed, queue promotes delayed, destroy cancels delayed, destroy vs a self
re-arming callback). These are logic tests of the glue; they say nothing about Windows itself.

### Bugs found and fixed

1. **`pw_os_work_destroy` livelock** (`pw_os_win.c`). If the callback re-arms itself with
   `pw_os_work_queue_delayed` (a periodic tick) while destroy runs, destroy's flush promoted
   every re-armed delayed run to an immediate one, forever: the test saw 436 back-to-back runs
   in 1 s and destroy never returned. The mock (`kmd/tests/pw_os_mock.c`) cancels delayed runs
   once stopping. Fix: a `dying` flag set by destroy under the lock; `queue_delayed` is ignored
   from then on (immediate `queue` still runs, as in the mock). Latent today (the core does not
   use `pw_os_work_*` yet), deterministic test added.
2. **UB in MMAP of a BO** (`pw_win_ioctl.c`): `(char *)MmGetMdlVirtualAddress(src) + offset`
   with the NULL StartVa of an `MmAllocatePagesForMdlEx` MDL is pointer arithmetic on NULL
   (UBSan, clang). Now computed as an integer. No behaviour change on the target expected.

No contract was changed.

### Claims checked

- The report's claims about builds, warnings, imports, x18, SEH tables and libdrm-win tests
  **held up** when re-run.
- Stale by now (not the engineer's fault): "core host tests fail under ASan" (they pass), and
  open issue 4 "the core rejects any size other than `_IOC_SIZE`": `pw_file_ioctl` now
  zero-extends shorter and passes through longer structs, like `drm_ioctl`.
- The core already behaves as the three "requested" contract clarifications assume:
  `pw_file_syncobj_notify_cancel()` takes a 64-bit id cookie and returns false for a fired or
  unknown one (no use-after-free), and `fn` is never called when `pw_file_syncobj_notify()`
  fails (`pw_sync.c`). They still need writing into `pw_core.h` by its owners.

### Remaining concerns (ranked)

1. **Untested on hardware**, all of it: SEH at runtime (`ControlPcIsUnwound` assumption),
   `MdlMappingNoWrite` honoured for UserMode mappings (else the user MMIO page is writable),
   `MmBuildMdlForNonPagedPool` over an `MmMapIoSpace` mapping, the alertable UserMode wait /
   termination interplay, and the approximate ARM64 `.def` import lists.
2. **Thread termination needs the wait to be outside every lock.** Termination reaches an
   alertable UserMode wait as a normal kernel APC first; inside a critical region (any
   `pw_os_mutex` held) it is never delivered and the thread cannot be killed while it waits. The
   core is fine today (no lock across `pw_os_event_wait_user`), and the harness now flags it,
   but `pw_os.h` should say so for future core code.
3. **Teardown under the global `files_lock`.** Cleanup/revoke wait for a file's in-flight
   ioctls while holding `files_lock`, so one slow non-interruptible ioctl (a long kernel-mode
   core wait) stalls every open/close of other handles. Correct (it is what makes revoke vs a
   concurrent cleanup safe before `pw_device_destroy`), but worth a per-file "teardown done"
   event later.
4. **Error copy-back differs from Linux.** `drm_ioctl` copies the struct back even on error
   (panthor `VM_BIND` sets `ops.count` on a failed sync bind, `panthor_drv.c:1425`); ABI v2 and
   METHOD_BUFFERED with an error NTSTATUS cannot. panvk does not read it today.
5. Duplicated-handle lifetime and `CancelIoEx` (the engineer's open issues 2 and 3) stand.
6. `pw_os_copy_*` use `ExGetPreviousMode()`, not the IRP requestor mode: a kernel requestor on a
   user thread gets EFAULT (fails closed). The `PW_MAX_NOTIFIES`/`PW_MAX_MAPS` caps are checked
   unlocked (can overshoot slightly). Both low.
