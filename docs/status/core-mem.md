# core-mem status

Owner key: `core-mem`. Scope: buffer objects, user VMs, the per-file layer and DRM ioctl dispatch in
`kmd/core` (panthor_gem.c, the VM side of panthor_mmu.c, panthor_drv.c). Date: 2026-10-30.

## Summary

- **`kmd/core/pw_file.c`** (rewritten)
  - `pw_file_open`/`pw_file_close`: init is bo, sync, sched; fini runs in the reverse order.
  - **Dispatch table.** Covers every DRM ioctl panvk/libdrm can issue:
    - DRM core: VERSION, GET_CAP, GEM_CLOSE and all nine SYNCOBJ_* handlers from pw_sync.h.
    - SYNCOBJ_HANDLE_TO_FD/FD_TO_HANDLE/EVENTFD and PRIME_* return `PW_EOPNOTSUPP`.
    - All 17 DRM_PANTHOR_* ioctls.
    - Unknown numbers, and ioctls newer than the reported version, return `PW_ENOTTY`.
  - **Struct sizes are handled generically, the way `drm_ioctl()` does it.**
    - A smaller (older) struct is zero-extended to the current one; only the caller's bytes are copied back.
    - A larger struct is passed through.
  - **VERSION:** two-pass, `drm_copy_field` semantics. Name `panthor`, date `"0"`, desc `Panthor DRM driver (panwin)`.
  - **GET_CAP:** SYNCOBJ=1, SYNCOBJ_TIMELINE=1, TIMESTAMP_MONOTONIC=1, PRIME=0, everything else EOPNOTSUPP.
  - **DEV_QUERY:** GPU_INFO, CSIF_INFO, TIMESTAMP_INFO (including the 1.8 flags), GROUP_PRIORITIES_INFO (LOW|MEDIUM) and MMU_INFO.
    - Size negotiation matches panthor exactly: `pointer==0` returns the size; then the UOBJ_MIN_SIZE check, truncate/zero-fill, and `copy_struct_from_user` for TIMESTAMP.
  - `SET_USER_MMIO_OFFSET`.
  - **`pw_file_mmap_lookup`:**
    - BO offsets.
    - The LATEST_FLUSH_ID page (`mmio_offset 0x10000`, one page, read-only) at both the 32-bit and 64-bit offsets.
  - **`pw_copy_obj_array`** follows PANTHOR_UOBJ_GET_ARRAY, with a 256 MiB cap.
- **`kmd/core/pw_bo.c`** (rewritten)
  - **BOs**
    - Refcounting: handle, VM mappings and user mappings each hold a reference.
    - Page-aligned sizes; GEM handles start at 1.
    - Fake mmap offsets are first-fit in `[4 GiB, 1<<43)`, so they can't collide with either user-MMIO offset. GEM_CLOSE revokes the offset, as in Linux.
    - BO_SET_LABEL (E2BIG past 4096 bytes); BO_SYNC; BO_QUERY_INFO; GEM_CLOSE.
    - Kernel BOs.
    - Memory: write-combined by default (`PW_MEM_UNCACHED`, no KMAP), cached only for WB_MMAP. User BOs ask for `PW_MEM_ZEROED`.
  - **VMs**
    - VM_CREATE follows panthor's `user_va_range` rules. Windows TASK_SIZE is 128 TiB, so `0` gives `1<<47`. The kernel range is rounded down to a power of two, and there are 32 VMs per file (EBUSY beyond that).
    - VM_DESTROY unmaps everything, kernel range included.
    - VM_GET_STATE.
  - **VM_BIND**
    - MAP/UNMAP/SYNC_ONLY with READONLY/NOEXEC/UNCACHED and bo_offset.
    - A MAP over an existing range replaces it (drm_gpuvm `sm_map` semantics; panthor does **not** return EBUSY).
    - Partial unmaps split VMAs and 2 MiB blocks.
    - A synchronous failure sets `ops.count` to the failing index.
    - VA bookkeeping is a sorted VMA list, each entry holding a BO reference.
  - **ASYNC binds** (panvk uses them in `panvk_async_bind.c`) run synchronously inside the ioctl, against the pw_fence/pw_sync contract:
    - Validate everything and resolve the wait fences first.
    - Then, per op: wait, execute, signal the op's fence, attach it to the signal syncobjs.
  - Kernel mappings (`pw_uvm_map_kernel_bo`): first-fit allocator in the kernel range, 2 MiB aligned for objects of 2 MiB or more.
  - `pw_uvm_activate`.
  - A sticky UNUSABLE state, set on a map/unmap page-table failure, an async-bind failure, or an MMU fault.
- **Emulated version: 1.6.**
  - 1.1, 1.2, 1.4 and 1.5 are implemented here.
  - 1.3 (INNOCENT) is only set after a GPU reset, which doesn't exist yet.
  - 1.6 (GLB_COUNTER_EN) is in pw_fw.c.
  - 1.7 is implemented in the core but not reported. WB_MMAP makes panvk use cached mappings plus EL0 `dc cvac/civac`, which is unverified on Windows ARM64 (see mesa.md), and the OS layer has to honour `pw_mmap_target.cached`. When both are confirmed, change `PW_DRM_MINOR` to 7 in pw_file.c.
  - 1.9 (sparse) is not implemented.
- 18 host tests in `kmd/tests/test_mem.c` (`mem_tests`).
- **Untested on hardware.**

## How to build/test

```
make -C kmd/tests                       # gcc, ASan+UBSan, -Werror; all suites
make -C kmd/tests clean && make -C kmd/tests CC=clang
make -C kmd/tests freestanding          # aarch64-w64-mingw32-clang -ffreestanding -Werror
cd kmd/tests && ./pw_tests mem          # only the mem_* tests (needs references/firmware/mali_csffw.bin)
```

## Verified (run here, 2026-10-30)

- **`make -C kmd/tests`** (gcc): 72 tests, 0 failed, 0 skips. That count includes the other suites as they currently are in the tree, including the in-progress pw_sync.c.
- **Clang:** after `make clean`, `make CC=clang` gives 72 tests, 0 failed, 0 skips.
- **`make freestanding`:** every core file compiles with `-Werror`. The objects import no memcpy/memset.
- **`./pw_tests mem`:** run 5 more times, 18/18 each time.
- **What the tests cover**, all through `pw_file_open` + `pw_file_ioctl` with real uAPI structs on the booted mock G610:
  - **Version and caps**
    - VERSION two-pass and truncated buffers; GET_CAP.
    - EOPNOTSUPP/ENOTTY paths, and 1.7 ioctls not reachable at 1.6.
  - **DEV_QUERY**
    - The size query for all five types.
    - GPU_INFO at exact, 1.0-minimum, below-minimum (EINVAL) and larger (zero tail) sizes.
    - TIMESTAMP: default flags, CPU/duration/cycle flags, bad flags, the 24-byte 1.1 struct, and E2BIG for a non-zero unknown tail.
    - PRIORITIES and MMU_INFO.
  - **Struct size extension:** smaller and larger structs, and NULL arg.
  - **`pw_copy_obj_array`:** same, larger and smaller strides; non-zero tail giving E2BIG; minimum stride; EFAULT; absurd count.
  - **BOs**
    - Create rules and every error.
    - mmap offset range, stability, first-fit reuse, EPERM/ENOENT.
    - mmap lookup with partial ranges and bounds.
    - GEM_CLOSE with a live mapping: the BO stays alive, the offset is revoked, and it is freed on the last put.
  - **User MMIO:** both offsets, size and offset errors, SET_USER_MMIO_OFFSET.
  - **Labels, WB_MMAP, BO_SYNC and QUERY_INFO.** The 1.7 parts are called directly.
  - **VM_CREATE:** `0`, explicit, rounding, and maximum `user_va_range`; flags; 32-VM limit; DESTROY and GET_STATE errors.
  - **VM_BIND, checked with `mock_gpu_translate`** (GPU view)
    - Per-page PAs.
    - NOEXEC (exec gives 0xcb), READONLY (write gives 0xcb), UNCACHED (NC MAIR index) versus the default WB.
    - bo_offset; replace-on-overlap; partial unmap across two BOs; hole unmaps.
    - BO refcounts at each step; VM_DESTROY dropping references.
  - **2 MiB:** level-2 blocks, a one-page unmap that splits a block, an unmap straddling two blocks, a remap into the hole, and VMA refcounts.
  - **Bind errors:** alignment, bounds, kernel-range overlap, SPARSE, unknown flags, UNMAP/SYNC_ONLY field rules, syncs without ASYNC (`ops.count` gives the failing index), small/large op stride, exclusive-VM BOs (including VM id reuse).
  - **Kernel mappings:** VA placement and alignment, attributes, unmap/no-op, reuse, VM_DESTROY.
  - **MMU fault:** an injected fault makes the VM UNUSABLE; maps then fail, unmaps still work, and activate gives EINVAL.
  - **Async binds:** without syncs; validation errors; with the real pw_sync.c, a wait on a signalled binary syncobj plus a signal on a second one that SYNCOBJ_WAIT then sees.
  - **File close with live objects:** BOs with offsets, mappings in two VMs, an active AS, a kernel mapping. Everything is freed: `check_no_leaks` passes and the AS is disabled.
  - **Threads:** 4 threads bind, unbind, mmap and close concurrently.

## Not verified / needs hardware

- **Everything in this file on a real G610 / Windows.** In particular:
  - the WC user mappings of `PW_MEM_UNCACHED` BOs;
  - the LATEST_FLUSH_ID page mapping;
  - real GPU_TIMESTAMP, CYCLE_COUNT and TIMESTAMP_OFFSET values (in the mock they read 0);
  - TLB and cache behaviour of binds on an active AS.
- `PW_MEM_ZEROED` is ignored by the mock, which fills allocations with 0xa5. BO zeroing therefore relies on kmd/win, which always zeroes, and is not tested.
- No test covers the kmd/win glue calling `pw_file_ioctl` and `pw_file_mmap_lookup`. That is kmd-win's integration work.
- No test covers async binds that actually block on an unsignalled fence. Only already-signalled fences were tested.

## Known issues & TODO (prioritised)

1. **Async VM_BIND blocks the calling thread** until its wait fences signal. panthor queues the work instead.
   - (Fixed in review: missing signal syncobjs are now rejected up front, and an interrupted wait returns a restartable PW_EINTR without marking the VM unusable.)
   - A real per-VM bind queue (a work item) is needed if panvk's sparse/async paths stall.
2. **The UNUSABLE-on-MMU-fault rule follows the pw_bo.h contract, not panthor.** In panthor a fault only kills the group, and VM_GET_STATE only reports `vm->unusable` (map failures). Revisit with the scheduler owner: with this rule a faulted VM can never be re-activated.
3. **The MAP replace path is not atomic.** It unmaps then maps, each under its own lock-region, whereas panthor takes one lock-region for the whole op. The GPU can see a transient hole in a range that is being rebound.
4. **VMs share AS slots without LRU eviction** (pw_mmu.c TODO). `pw_uvm_activate` returns EBUSY after 7 active VMs.
5. **Possible move to 1.7**, once EL0 cache maintenance on Windows ARM64 and the cached user mappings in kmd-win are verified. Mesa could also default to `PAN_KMOD_DEV_FLAG_MMAP_SYNC_THROUGH_KERNEL`, which would route all cache maintenance through BO_SYNC.
6. **BO_SYNC on Windows** uses `pw_os_mem_flush/invalidate`. Without a KMAP, kmd/win maps the whole MDL for each call, which is slow for large BOs. A ranged temporary mapping would fix that.
7. **The exclusive VM is identified by a global serial number.** panthor shares the VM's resv object instead. There is no implicit-sync or resv model here.
8. **`pw_syncobj_find_fence` on a missing syncobj.** Linux returns ENOENT and panthor propagates it from VM_BIND. The contract says EINVAL, and pw_sync.c keeps EINVAL citing pw_bo.c. **pw_bo.c does not depend on it:** it propagates whatever is returned, and the test accepts both. ENOENT would match Linux.
9. **The VM_BIND `args->flags` are not validated**, same as panthor. Unknown bits are ignored.

## Contract changes

All of these are additions; no existing semantics changed.

- **`pw_os.h`: `PW_E2BIG` (-7).** Needed for panthor's `copy_struct_from_user`/`strndup_user` error codes (DEV_QUERY TIMESTAMP, obj arrays, BO labels). kmd/win maps any errno generically via PANWIN_STATUS_FROM_ERRNO.
- **`pw_os.h`: `PW_MEM_ZEROED` flag.** User BOs must not leak stale memory. kmd/win already always zeroes, so it needs no change. The mock (kmd-core) could honour it so the guarantee becomes testable.
- **`pw_file.h`: `struct pw_bo_file *bo_file` in `struct pw_file`.** Per-file BO state (the mmap-offset map, the label lock) owned by pw_bo.c.
- **`pw_mmu.h`: `uint32_t fault_count` in `struct pw_vm`; `pw_mmu.c`: the IRQ handler increments it atomically** (minimal edit, one hunk). This is the fault hook. It is sticky, unlike `unhandled_fault`, which `pw_vm_activate` clears. `pw_uvm_is_unusable` reads it.
- **`pw_bo.h` additions:**
  - `pw_bo_create_user()`, so tests can exercise WB_MMAP below 1.7;
  - `pw_bo_cpu_cached()`;
  - `pw_uvm_kernel_va_range()`;
  - a documented rule: `pw_uvm_activate` returns PW_EINVAL once the VM is unusable.
- **Not a contract change:** SET_USER_MMIO_OFFSET is handled in pw_file.c (static), although pw_file.h's ownership comment lists it under pw_bo.c. pw_bo.h never declared a handler for it.

## Review

Independent review, 2026-10-30. Reviewer owns the same files; no contract (header) changes were made.

### Re-ran (from clean)

- `make -C kmd/tests clean && make -C kmd/tests` (gcc, ASan+UBSan): **72 tests, 0 failed** before any change, matching the report. After the fixes: **78 tests, 0 failed**. The other suites grew by 3 in the meantime; that is parallel work.
- `./pw_tests mem`: 18/18 before the fixes. After them, 21/21 eight times in a row with gcc and three times with clang.
- `make -C kmd/tests clean && make -C kmd/tests CC=clang`: 78 tests, 0 failed.
- `make -C kmd/tests freestanding`: clean. The core imports only `pw_*` symbols, with no memcpy/memset.
- **The new tests catch the old bugs.** Run against the original pw_bo.c, `mem_vm_bind_async_syncs` fails 4 checks.

### Claims checked

These match panthor (drv.c, mmu.c v7.3) and the DRM core semantics:

- **Ioctl handling:** VERSION (`drm_copy_field`, where a NULL buffer is OK), GET_CAP, and struct-size extension (drm_ioctl always copies out, so the `ops.count` update reaches the user).
- **DEV_QUERY and object arrays:** DEV_QUERY size negotiation and `PANTHOR_UOBJ_MIN_SIZE`; `pw_copy_obj_array` against `panthor_get_uobj_array`.
- **BO ioctls:** BO_CREATE errnos and the NO_MMAP+WB_MMAP check; MMAP_OFFSET EPERM/ENOENT; SET_LABEL E2BIG (strndup_user bound).
- **VM ioctls:** VM_CREATE `user_va_range` (TASK_SIZE replaced by 128 TiB); 32 VMs with EBUSY; VM_DESTROY/GET_STATE EINVAL.
- **Binds:** bind-op validation, no EBUSY on overlap, and the sync-bind `ops.count`.
- **Mmap:** the LATEST_FLUSH_ID page at 0x10000, one read-only page.
- **Version:** reporting 1.6 is honest given every gate in Mesa's `panthor_kmod.c`: 1.1, 1.2, 1.4, 1.5 and 1.6 (`timestamp_device_coherent`, `GLB_COUNTER_EN` in pw_fw.c:671), with 1.7 and 1.9 gated off.
- **License hygiene:** OK. Attribution is present, and no drm_syncobj, dma-fence or io-pgtable code is used.

### Bugs found and fixed (pw_bo.c)

1. **An interrupted async VM_BIND made the VM permanently UNUSABLE.**
   - On Windows, `PW_EINTR` comes from any user APC as well as teardown (pw_os_win.c:480), and libdrm-win restarts on EINTR. The retry then got EINVAL, so the device was lost.
   - Fix: return `PW_EINTR` with nothing published and the VM left usable. Replaying the batch is idempotent, because each op sets a VA range to a fixed state.
   - **Untested:** the mock (`pw_os_mock.c`, not ours) cannot inject EINTR.
2. **A missing signal syncobj in an async bind was detected only after the ops had run.** They had already changed the VM, yet the call returned EINVAL.
   - Fix: signal syncobjs are now resolved up front with `pw_syncobj_lookup` plus `pw_syncobj_point_alloc`, as in drv.c:396. Publication happens after all ops, using the no-fail `add_point`/`replace_fence`.
   - Duplicate (handle, point) signals are now published once, with the last op's fence (panthor's signal list). Before, they added duplicate timeline points.
   - A wait-fence failure other than EINTR now fails only that op: the error goes through its fence and the VM becomes unusable.
3. **An empty async batch on an unusable VM returned EINVAL.** panthor returns 0, because no job is created. Fixed.
4. **A map failure reported after its PTEs were written left PTEs with no VMA or BO reference.** This happens when the flush/UNLOCK in `vm_unlock_region` fails. The GPU could then reach freed pages. Fix: a best-effort `pw_vm_unmap` of the range on any map failure.
   - **Untested:** the mock can't fail UNLOCK.

### New tests (test_mem.c)

- **`mem_vm_bind_async_blocking`:** a real cross-thread wait on an unsignalled fence.
- **`mem_vm_bind_async_syncs`:**
  - a missing signal syncobj causes no side effects;
  - same-batch timeline dependencies;
  - a wait on a point that only a later op signals gives EINVAL;
  - duplicate signals;
  - an empty batch on an unusable VM.
- **`mem_destroy_races`:**
  - VM_DESTROY racing user binds and kernel map/unmap; afterwards nothing is mapped and the BO refcounts are exact;
  - GEM_CLOSE racing MMAP_OFFSET + `mmap_lookup`.

### Remaining concerns (ranked)

1. **A sticky UNUSABLE on any MMU fault** (contract) diverges from panthor. One shader fault kills the file's VM for good. This needs agreement with core-sched.
2. **Async binds still block the calling thread.** A per-VM work queue is needed eventually.
3. **Splitting a 2 MiB block on unmap allocates page tables mid-operation** (ENOMEM leaves the VM unusable). panthor preallocates. The same applies to the non-atomic MAP replace (transient hole).
4. **`pw_bo.exclusive_vm` can dangle** once the VM is freed. Nothing dereferences it today; future users must use the serial instead.
5. **The signal/wait matching is O(n²)**, as in panthor. There is no cap beyond `pw_copy_obj_array`'s 256 MiB.
6. **Untested on hardware**, as before.
