# core-sync status

Owner key: `core-sync`. Scope: kernel fences (`pw_fence.h`) and DRM syncobjs, binary and timeline (`pw_sync.h`), in `kmd/core/pw_sync.c`, with host tests in `kmd/tests/test_sync.c`.
Date: 2026-10-30.

## Summary

`pw_sync.c` replaces the stub and implements everything declared in `pw_fence.h` and `pw_sync.h`, plus the `pw_file_syncobj_notify` and `pw_file_syncobj_notify_cancel` functions from `pw_core.h`.

The semantics follow Linux `drm_syncobj.c` and `dma-fence-chain.c` (v7.2). I read those files for their semantics only. They are GPL-2.0, so no code was copied and the data structure is my own.

**Fences**
- Refcounted, signalled once with an error code, and timestamped.
- Callbacks run in registration order, with no lock held.
- If a callback is removed while it is running, removal returns `false`, as the header requires.
- `pw_fence_wait` takes an absolute deadline and waits through `pw_os_event_wait_user`. It returns `PW_ETIME` on timeout and passes `PW_EINTR` through.
- A fence created already signalled has no mutex, so it costs only one allocation.

**Timelines**
- Each timeline is an ordered list of nodes, `(point, payload, prev_seqno, context)`. This mirrors the `dma_fence_chain` links one-for-one, so the following match Linux:
  - `find_seqno`;
  - the QUERY walk, including `DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED`;
  - the unordered-point rule: a new context, with seqno = max(last, point) and prev_seqno = 0;
  - garbage collection of signalled points;
  - a timeline point added on top of a binary fence.
- A point that still has older pending points gets a "chain fence", which signals once that point and every earlier one have signalled. It is allocated lazily.
- `tl_advance()` signals chain fences and pops finished nodes in a loop, not by recursion. A test signals 2,000 pending points newest-first to exercise this.
- A timeline is refcounted separately from its syncobj. Chain fences that were handed out therefore still signal after a RESET, a SIGNAL or a DESTROY.

**ioctls**
- CREATE (with `CREATE_SIGNALED`), DESTROY, WAIT, TIMELINE_WAIT, RESET, SIGNAL, TIMELINE_SIGNAL, QUERY and TRANSFER (binary↔timeline in both directions) are all implemented.
- Flag checks, pad checks, count rules and return codes follow Linux:
  - `-ETIME` on timeout. A timeout of 0, a negative timeout or a time in the past means poll.
  - `-EINVAL` for a missing fence without `WAIT_FOR_SUBMIT`/`WAIT_AVAILABLE`.
  - `-ENOENT` for an unknown handle and `-EFAULT` for a bad user pointer.
  - WAIT with `count_handles` = 0 returns 0. The other ioctls return `-EINVAL` for a count of 0.
  - `first_signaled` follows Linux's pre-pass rule and is written only on success.
- `WAIT_FOR_SUBMIT` blocks until a fence is attached: every attach and replace wakes the syncobj's waiters.
- `WAIT_AVAILABLE` returns as soon as a fence exists for the point.
- `deadline_nsec` (`WAIT_DEADLINE`) is accepted and ignored.

**Kernel-internal API**
- `pw_syncobj_find_fence` and `pw_syncobj_attach_fence`, for VM_BIND and the scheduler.
- New object-level API with preallocated points. It follows panthor's pattern, where `drm_syncobj_add_point` cannot fail after the point of no return.

**Notify** (the PANWIN_IOCTL_SYNCOBJ_EVENT backend)
- Fires exactly once: immediately if the fence is already signalled, on signal otherwise, or on attach with `WAIT_AVAILABLE`.
- Supports `WAIT_FOR_SUBMIT`.
- Cancelling returns `true` only if the notify had not fired. Cancelling after it fired is safe and returns `false`.
- The cookie is an opaque 64-bit id, not a pointer, so a stale cookie can never alias a newer registration.
- Notifies still pending when the file closes are dropped without calling `fn`.

**Everything here is untested on hardware.** It contains no hardware-specific code, but nothing has run under the Windows kernel's `pw_os_win.c` either.

## How to build/test

```
make -C kmd/tests                       # gcc, ASan+UBSan, -Werror, all suites
make -C kmd/tests clean && make -C kmd/tests CC=clang
make -C kmd/tests freestanding          # aarch64-w64-mingw32-clang -ffreestanding -Werror
cd kmd/tests && ./pw_tests sync_        # only the 23 sync tests (~0.6 s)
```

`kmd/tests/obj/` and `pw_tests` are shared by every engineer. Concurrent `make clean` runs clobbered my binary mid-run, so I did the repeated stress runs in a private mirror of `kmd/tests` with the core and include directories symlinked, under my scratchpad.

## Verified (actually run here)

| What I ran | Result |
|---|---|
| `make -C kmd/tests` (gcc, ASan+UBSan) | 72 tests, 0 failed, 0 skips. This is all suites, including `test_mem.c`'s VM_BIND tests that call my find/attach. |
| `make CC=clang` (after `clean`) | 72 tests, 0 failed, 0 skips |
| `make freestanding` | Exit 0, no warnings. `pw_sync.o` imports only `pw_os_*`, `pw_handle_*` and `pw_memset`. |
| `./pw_tests sync_`, private mirror, gcc+ASan | 50 runs, 0 failures |
| Same, clang+ASan | 30 runs, 0 failures |
| ThreadSanitizer build (clang `-fsanitize=thread`) | 10 runs, 0 failures, 0 TSan reports |

What the 23 `sync_*` tests cover:
- **Fence basics:** basics, callback order and removal, removal while the callback is running, measured timeouts, and a 4-waiter / 4-callback / 2-signaller stress test.
- **Error paths:** the error path of every ioctl.
- **Waits:** WAIT_ALL vs any with `first_signaled` (both blocking and polling); measured absolute timeouts of 30–50 ms, with checks that the elapsed time is at least the timeout and under 1 s.
- **Submit-time waits:** `WAIT_FOR_SUBMIT` and `WAIT_AVAILABLE` with an attacher thread.
- **Timelines:** ordering with out-of-order completion, the unordered-point rule, and GC (allocations stay bounded over 2,000 points).
- **TRANSFER:** in all directions, including `WAIT_FOR_SUBMIT` with a late attacher.
- **Kernel API:** find, attach and the object API.
- **Notify:** fire/cancel semantics, plus 400 fire-vs-cancel races checking that exactly one of the two happens.
- **Destroy with pending waiters:** both the fence-waiter and the `WAIT_FOR_SUBMIT`-waiter cases, and chain fences surviving RESET/DESTROY.
- **Concurrency:** 4 timeline waiters, 2 signallers and a query thread that checks the reported point never decreases.
- **Leaks:** `check_no_leaks` (allocations, events and mutexes) runs at the end of every test.

## Not verified / needs hardware

- **The real Windows OS layer.** I only ran against `pw_os_mock.c`. Untested:
  - `pw_os_event_wait_user` on Windows, with its QPC deadline conversion and alertable waits;
  - `pw_os_copy_from_user`/`pw_os_copy_to_user` faulting.
- **`PW_EINTR` propagation.** The code passes it through in fence waits, array waits and find-with-`WAIT_FOR_SUBMIT`, but the mock's `pw_os_event_wait_user` never returns it, so no test exercises it.
- **Real fence producers.** There are no real scheduler or GPU fences yet, so I have not checked behaviour with them, including signalling from the IRQ bottom half.
- **Mesa `vk_drm_syncobj` through libdrm-win.** Not run end to end against this core.

## Known issues & TODO (prioritised)

1. **Callbacks under a syncobj lock.** An attach that makes a notify fire immediately calls the OS `fn` while holding that syncobj's mutex. `pw_core.h` already says `fn` must not block. In addition, it must not call back into the sync module. The same applies to scheduler fence callbacks reached through `pw_syncobj_add_point` and `pw_syncobj_replace_fence`.
2. **Never-signalled payloads keep a timeline alive.** A timeline whose payload fences never signal stays allocated after its syncobj is reset or destroyed, just as a `dma_fence_chain` holds its links. The scheduler must signal every fence, with an error if need be, on group or device teardown. Otherwise `check_no_leaks` will catch it.
3. **One mutex per unsignalled fence.** Each such fence allocates a `pw_os_mutex`. If profiling shows this to be expensive on Windows, a lock pool would be cheaper. Fences created signalled already allocate no mutex.
4. **`count_handles` limit.** It is capped at `PW_SYNC_MAX_HANDLES` (16384) and returns `PW_EINVAL` above that. Linux is bounded only by kmalloc, where failure gives `-ENOMEM`.
5. **Chain-fence errors.** A chain fence carries only its own point's payload error. Linux chains don't propagate errors either.
6. **Notify on allocation failure.** If allocating a chain fence fails during a `WAIT_FOR_SUBMIT` wake-up, the notify fires early and an array wait returns `PW_ENOMEM`. Both are very unlikely.
7. **TRANSFER timeout.** TRANSFER with `WAIT_FOR_SUBMIT` uses Linux's fixed 5 s submit timeout (`DRM_SYNCOBJ_WAIT_FOR_SUBMIT_TIMEOUT`).
8. **Test infrastructure.** The shared `kmd/tests` build directory races between engineers, so the lead may want per-owner `OBJDIR`s.

## Contract changes

Additions only. Nothing that existing code depends on was changed.

- **`kmd/core/pw_sync.h`, new declarations:**
  - `PW_SYNC_MAX_HANDLES` and an object-level API: `struct pw_syncobj`, `struct pw_syncobj_point`, `pw_syncobj_lookup/get/put`, `pw_syncobj_point_alloc/free`, `pw_syncobj_add_point`, `pw_syncobj_replace_fence`.
  - Reason: GROUP_SUBMIT and VM_BIND must resolve syncobjs and preallocate chain nodes before the point of no return, as panthor does (`panthor_drv.c:403`, then `drm_syncobj_add_point`). `pw_syncobj_attach_fence` allocates, so it can fail after a job has been queued.
- **Behaviour clarification (no declaration changed):** the notify cookie is an opaque id. Cancel after fire is allowed and returns `false`, so the OS layer does not need to track which notifies have fired.

**Proposed but not applied.** For a missing syncobj, `pw_syncobj_find_fence` returns `PW_EINVAL`, as the header says. Linux `drm_syncobj_find_fence` returns `-ENOENT`, and panthor passes that through from GROUP_SUBMIT/VM_BIND waits (`panthor_drv.c:633`). I tried switching to `PW_ENOENT`, but `test_mem.c:1170` (core-mem) depends on `PW_EINVAL`, so I reverted it. Changing it needs the core-mem owner (and later the scheduler owner) to agree. `pw_syncobj_attach_fence` also returns `PW_EINVAL` for a missing syncobj. That matches panthor's signal path (`panthor_drv.c:403-405`). The SYNCOBJ ioctls themselves return `-ENOENT` exactly as Linux does.

## Review

Independent review, 2026-10-30. All results below are from runs I did myself, against the mock OS only. Everything is still untested on hardware and under the real Windows OS layer.

**What I re-ran**
- Before any change, from clean: `make -C kmd/tests` (gcc, ASan+UBSan) gave 72 tests, 0 failed, 0 skips. `make -C kmd/tests CC=clang` gave the same. `make -C kmd/tests freestanding` exited 0. The engineer's reported numbers held.
- After the fixes, from clean: `make -C kmd/tests` gave 78 tests, 0 failed, 0 skips. `make CC=clang` gave the same. `make freestanding` exited 0, and `pw_sync.o` still imports only `pw_os_*`, `pw_handle_*` and `pw_memset`. (Other owners also added tests in the meantime.)
- Stress runs in a private mirror, to avoid the shared `obj/`:
  - `./pw_tests sync_` 30 times with gcc+ASan: 0 failures.
  - A clang ThreadSanitizer build of the whole suite, `sync_` run 10 times: 0 failures, 0 TSan warnings.

**Bugs found and fixed (`pw_sync.c`)**
1. **Unbounded kernel-stack recursion that userspace controls.** TRANSFER `h:k -> h:k+1`, repeated, makes each point's chain fence the payload of the next point. Signalling the bottom of that chain then recursed `pw_fence_signal -> tl_node_cb -> tl_advance -> pw_fence_signal` once per link. The new test `sync_review_transfer_chain_depth` uses 20,000 links on a 256 KiB thread, and the original code hit an ASan stack overflow there. A Windows kernel stack would bugcheck after a few dozen links.
   - Fix: when `fence_signal()` is called from `tl_advance`, it queues the timeline's own payload callbacks instead of running them. `tl_advance()` then completes the queued nodes in a loop, across timelines too, so the stack depth is constant. Linux avoids the same recursion by sending `dma_fence_chain_cb` through `irq_work`.
2. **Use after free in `pw_file_syncobj_notify`.** After `so->lock` was dropped, the function read `n->id` to build the cookie. Another thread could already have fired the notify and freed it. TSan confirmed this against the original code.
   - Fix: the id is now read under `reg->lock`.
3. **Notify refcount race, which leaked the notify or freed it while the lock was held.** The W reference was taken up front and then dropped with a plain `fetch_sub` after `notify_arm_locked()`. By then the fence callback could already have dropped R and C on another thread, because neither needs `so->lock`. The count then reached 0 without anything freeing the notify.
   - Fix: W is now taken only when the notify is linked as a waiter.
4. **Syncobj freed while its own lock was held.** `n->so` consumed the lookup reference. A concurrent DESTROY plus a fence signal could therefore free the notify, and with it the last syncobj reference, while the registering thread still held `so->lock`.
   - Fix: registration keeps its own reference until it has unlocked.

**Tests added (`test_sync.c`)**
- `sync_review_transfer_chain_depth`: single-timeline and two-timeline alternating chains, signalled from a small-stack thread.
- `sync_review_notify_register_race`: notify registration racing a signal, an attach, and a DESTROY plus signal. The ASan build alone never caught bugs 2 and 3. The TSan build of the original code reported 34 data races over 6 runs; the fixed code reported none.
- `sync_review_semantics_misc`:
  - a fence error does not fail WAIT;
  - a binary syncobj with point > 0 gives EINVAL, and times out under WAIT_FOR_SUBMIT;
  - TIMELINE_WAIT accepts a NULL points array;
  - the same handle can appear twice in TIMELINE_SIGNAL;
  - RESET does not satisfy WAIT_FOR_SUBMIT;
  - the count cap is enforced.

**Checked and found consistent with Linux 7.2 `drm_syncobj.c` / `dma-fence-chain.c`** (read for semantics only)
- Flag, pad and count checks, and the errnos for every ioctl (ENOENT, EINVAL, EFAULT, ETIME).
- The poll rule for `timeout_nsec`; `first_signaled` from the pre-pass, including under WAIT_ALL.
- `find_seqno` and the QUERY walk, including the binary-base and unordered-point cases.
- TRANSFER order: dst is looked up before src.
- The 5 s submit timeout.

I found no copied GPL code. SPDX headers are present.

**Remaining concerns, ranked**
1. **Every fence, timeline and syncobj lock is a `pw_os_mutex`, which needs PASSIVE_LEVEL.** Whoever calls `pw_fence_signal()` (the scheduler or the IRQ bottom half) must therefore run at passive level, never from a DPC. On Windows this needs a work item, not a DPC, for job completion.
2. **Cross-timeline recursion is now bounded, but other callback types are not covered.** If a scheduler fence callback signals another fence inline, it could still nest. The scheduler owner should keep fence callbacks non-signalling, or defer them.
3. **No GC behind a stuck point.** Linux's chain walk frees signalled links that sit behind an unsignalled one. Here they stay in the list until the older point signals, so QUERY and find cost O(n) and memory grows. It matters only with a long-pending point under heavy traffic.
4. **The `PW_EINTR` path is still untested**, because the mock cannot inject it. Linux would restart the call after `-ERESTARTSYS`, so libdrm-win must retry on EINTR as `drmIoctl` does.
5. **Notify `fn` still runs under `so->lock` when an attach fires it.** This is acceptable only while `fn` stays non-blocking (the existing known issue 1).
6. **Unknown handle in `pw_syncobj_find_fence`.** It returns EINVAL where Linux and panthor return ENOENT (the existing proposal). I left the decision to the lead and core-mem.

**Contract changes by the reviewer:** none.
