# core-sched status

Owner key: `core-sched`. Scope: CSF scheduling in `kmd/core` (panthor_sched.c, panthor_heap.c and
the GROUP_*/TILER_HEAP_* parts of panthor_drv.c), the CSF executor in the mock MCU, and the
scheduler host tests. Date: 2026-10-30.

**Everything here is untested on hardware.** The scheduler has only run against the mock MCU in
`kmd/tests`, which is our own model of the CSF interface, not Arm's firmware.

## Summary

### `kmd/core/pw_sched.c` (new; replaces `pw_sched_stub.c`, which is deleted)

Derived from Linux panthor (dual GPL-2.0/MIT, used under MIT). It covers the MVP of
docs/research/kernel-panthor.md 3.3.

- **GROUP_CREATE** (drv.c:1224, sched.c:3626). Argument checks and errnos follow panthor:
  - queue count; `group_priority_permit` gives EACCES for HIGH/REALTIME and EINVAL above REALTIME;
  - pad, core masks against `shader_present`/`tiler_present`, `max_*_cores` against the mask weights;
  - per queue: pad, `ringbuf_size` must be a power of two in [4K, 64K], priority ≤ 15.
  - Handles go up to 128 (MAX_GROUPS_PER_POOL); beyond that, EBUSY.
  - Each group allocates:
    - a per-queue ring buffer: kernel BO, write-combined KMAP, mapped NOEXEC|UNCACHED in the
      group's VM with `pw_uvm_map_kernel_bo`;
    - a per-queue 8 KiB ring-buffer input/output interface, mapped in the **MCU VM** through the
      new MCU kernel VA allocator. Input is at +0 and output at +4K (fw.c:489);
    - a per-group `panthor_syncobj_64b` array in the group's VM;
    - suspend and protm-suspend buffers sized from CSG 0's control interface, mapped NOEXEC in
      the MCU VM (fw.c:530).
  - A group is published (handle and device list) only once it is fully built.
- **GROUP_DESTROY** and **GET_STATE**: the TIMEDOUT, FATAL_FAULT and `fatal_queues` flags.
  INNOCENT is never set, because there is no reset.
- **GROUP_SUBMIT** follows the drv.c:1106 flow:
  1. Validate each job as `panthor_job_create` does: pad; stream addr/size both zero or both
     non-zero; 64-byte address and 8-byte size alignment; `latest_flush` bits 24-30; group can run;
     queue index.
  2. Collect the signal ops, resolving the syncobjs and preallocating the timeline points.
  3. For each job: resolve the wait ops (signals earlier in the same batch first, else
     `pw_syncobj_find_fence`), create its fence, update the signal list.
  4. Past the point of no return: queue the jobs and publish the fences with
     `pw_syncobj_add_point`/`replace_fence`.
  - **Jobs run asynchronously, like drm_sched.** A job waits for its in-fences through fence
    callbacks plus one device work item. The ioctl never blocks on a fence.
  - **Emission.** Once ready, `prepare_job_instrs()`'s exact sequence is written into the ring,
    padded to 64 bytes (11 instructions → 128 bytes):
    - MOV32 + FLUSH_CACHE2 with latest_flush (the GPU does the flush elision);
    - MOV48/MOV32 + WAIT + CALL into the user stream;
    - MOV48 sync_addr, MOV48 #1, WAIT(all), SYNC_ADD64, ERROR_BARRIER.

    The ring is then published: wmb, insert updated, queue doorbell rung.
  - **Ring space.** A job waits for ring space, the equivalent of drm_sched's credits.
  - **`stream_size == 0`** completes with the queue's previous job and carries its error, without
    binding the group (sched.c:3306).
- **CSG slot binding** (MVP, no rotation). On a group's first ready job with a stream:
  1. `pw_uvm_activate`, then take a free slot (doorbell = slot + 1).
  2. `cs_slot_prog_locked` (sched.c:1108) for every queue, then `csg_slot_prog_locked`
     (sched.c:1339): core masks, endpoint_req (or endpoint_req2 for iface ≥ 4.0), AS, suspend
     buffers, `ack_irq_mask`, `doorbell_req` toggle.
  3. CSG START plus an ENDPOINT_CONFIG toggle, then wait for the ack (100 ms).

  A group keeps its slot until it dies or is destroyed. CSG priority is `15 - slot`.
- **Job IRQ.** `pw_fw_job_irq_handler` now passes the CSG bits to `pw_sched_job_irq`, which does
  what `sched_process_csg_irq_locked` (sched.c:1788) does:
  - copy `cs_irq_ack` from `cs_irq_req`; ack SYNC_UPDATE, IDLE and PROGRESS_TIMER;
  - per CS: FATAL sets the queue in `fatal_queues`; FAULT sets -EINVAL on the in-flight jobs
    around `extract`; TILER_OOM is deferred to the work; FATAL/FAULT are then acked;
  - toggle the doorbell for FAULT/OOM queues and ring the CSG doorbell;
  - signal every in-flight job whose seqno is ≤ the queue's syncobj (sched.c:1682), on every CSG
    IRQ.
- **Termination** (group destroyed, fatal, timed out or VM faulted):
  - CSG TERMINATE, then the CS slots are set to STOP and the group is unbound.
  - `group_term_post_processing` (sched.c:2237): in-flight jobs fail with -EINVAL (fatal queue),
    -ETIMEDOUT or -ECANCELED. Jobs that never reached the ring get -ECANCELED. Syncobjs are set to
    the last seqno with status ~0.
  - A file close flags all its groups first, so jobs waiting on another of its groups get
    cancelled rather than started.
- **Job timeout**: 5 s per queue, re-armed on progress. Checked by the work item (`queue_delayed`),
  after a completion check. `pw_sched_set_job_timeout()` exists for tests.
- **TILER_OOM** (sched.c:1495). The work reads `heap_address`/`vt_start`/`vt_end`/`frag_end`;
  inconsistent counters give EINVAL and a fatal queue. It then calls `pw_heap_grow`; on ENOMEM it
  answers `heap_start = heap_end = 0`. Finally it writes `heap_start`/`heap_end`, acks TILER_OOM,
  toggles the CS doorbell and rings the CSG.
- **VM fault**: a bound group whose VM `pw_uvm_is_unusable()` gets every queue flagged fatal on
  the next work run (sched.c:2215).

### `kmd/core/pw_heap.c` (new)

- TILER_HEAP_CREATE/DESTROY and `pw_heap_grow` follow panthor_heap.c and drv.c:1263-1320, with
  the same checks and errnos:
  - initial count ≥ 1 and ≤ max; chunk size page-aligned in [128K, 8M];
  - 128 heaps per pool, EBUSY beyond that;
  - DESTROY: pad gives EINVAL, a VM without a pool gives ENOENT (mmu.c:2144), an unknown id
    gives EINVAL.
- The heap-context pool BO holds 128 contexts of `ALIGN(32, L2 line)`, mapped NOEXEC in the VM's
  kernel range.
- Chunks are kernel BOs mapped NOEXEC. Initial chunks are linked newest-first with
  `next = va | size >> 12`; grown chunks have `next = 0`.
- The heap handle is `vm_id << 16 | id`.

### Hooks in other files (allowed by the task)

- `pw_fw.c`: the MCU-VM kernel VA allocator, `pw_fw_map_kernel_mem`/`pw_fw_unmap_kernel_mem`.
  It is first fit in [0x04000000, 0x08000000) and skips the FW sections (fw.c:1525). Also
  `kva_lock` lifetime, and the job IRQ routing.
- `pw_device.c`: `pw_sched_init` after `pw_fw_init` (device.c:286). `pw_sched_fini` in
  `device_quiesce` before `pw_fw_fini`; it syncs with the job bottom half through `bh_lock`.
- `meson.build`: adds `pw_heap.c` and `pw_sched.c`; `pw_sched_stub.c` is removed.

### Mock CSF executor (`kmd/tests/mock_csf.c`, `mock_internal.h`; hooks in `pw_os_mock.c`)

- **CSG/CS requests.** On GLB doorbell bits the mock handles CSG STATE START/RESUME and
  TERMINATE/SUSPEND (all CSs go to STOP), ENDPOINT_CONFIG and STATUS_UPDATE toggles, then the
  `doorbell_req` → `doorbell_ack` toggles. Per CS it handles state, IDLE_* echoes and TILER_OOM
  resume.
- **Execution.** On a CS doorbell (doorbell id taken from CS config), the mock runs the ring from
  extract to insert through the group's AS, with a 128-register file per CS.
  - Data instructions: MOVE48/32, ADD_IMM32/64, LOAD/STORE_MULTIPLE, BRANCH.
  - CALL/JUMP, with a depth limit of 8 (CS_CALL_STACK_OVERFLOW beyond it).
  - SYNC_ADD/SET 32/64, which raise CSG SYNC_UPDATE.
  - SYNC_WAIT32/64: block the CS and fill `status_wait*`/`blocked_reason`. They are re-evaluated
    after every CS that writes memory, and on `mock_csf_kick()`.
  - HEAP_SET, STORE_STATE.
  - No-ops (counted): FLUSH_CACHE2, WAIT, ERROR_BARRIER, NOP, RUN_*, FINISH_*.
- **Faults and injection.**
  - Unknown opcodes and bad registers raise CS_FATAL 0x49; GPU faults raise 0x48.
  - TILER_OOM can be injected on RUN_TILING/IDVS.
  - `extract` is written back to the output interface. Encodings come from Mesa genxml v10.xml.

### Tests (`kmd/tests/test_sched.c`, 13 tests, all through `pw_file_ioctl`)

- `group_create_destroy`: every argument error, handles, the 128 limit and EBUSY, handle reuse.
- `submit_basic`:
  - every `job_create`/sync-op error;
  - user BO updated, out-syncobj signalled with error 0;
  - **the ring content checked word by word against panthor's sequence**;
  - CSG priority/endpoint, CS doorbell/priority;
  - the syncobj in the kernel VA range, NOEXEC.
- `ring_wrap`: 300 jobs through a 4K ring, timeline QUERY = 300.
- `sync_only`.
- `insync_deps`: across groups, within one batch, an unknown handle, a CPU-signalled fence.
- `user_stream`: nested CALL, STORE_MULTIPLE, ADD_IMM, SYNC_SET32, call-stack overflow.
- `cs_fatal`: bad opcode, bus fault, GET_STATE, later submits rejected.
- `timeout`: ETIMEDOUT/ECANCELED and timing; a busy queue that progresses does not time out.
- `vm_fault`: an injected MMU fault on the group's AS.
- `slots`: a 9th group waits for a slot; destroying a blocked group.
- `heap`: create/destroy errors, chunk links read through the GPU MMU, OOM grow, ENOMEM,
  inconsistent counters.
- `teardown_inflight`: file close with in-flight, pending and cross-group jobs; fences end
  -ECANCELED.
- `threads`: 4 submitters.
- `senv_fini` checks `check_no_leaks`, zero mock model errors and zero unclaimed IRQs.

## How to build/test

```
cd /home/e3xp0/Projects/panfrost-win/repo
make -C kmd/tests clean && make -C kmd/tests            # gcc, ASan+UBSan, -Werror, all suites
make -C kmd/tests clean && make -C kmd/tests CC=clang
make -C kmd/tests freestanding                          # aarch64-w64-mingw32-clang -ffreestanding
cd kmd/tests && ./pw_tests sched                        # only the 13 sched tests (needs the FW file)
PW_TEST_VERBOSE=1 ./pw_tests sched_heap                 # with core log lines
```

## Verified (actually run here, 2026-10-30)

| What | Result |
|---|---|
| `make -C kmd/tests` after `clean` (gcc, ASan+UBSan) | 91 tests, 0 failed, 0 skips (13 of them sched) |
| `make -C kmd/tests CC=clang` after `clean` | 91 tests, 0 failed, 0 skips |
| `make -C kmd/tests freestanding` | exit 0, no warnings; no memcpy/memset/memmove imports in any core object |
| `./pw_tests sched` in a private mirror, gcc+ASan | 10 sequential runs plus 24 runs as 6 parallel instances: all 13/13. The parallel runs found a real bug, since fixed: a ring-full stall. |
| clang `-fsanitize=thread` build, `sched`, 3 runs | 0 lock-order or data-race reports in core-only code, after fixing a lock-order inversion (see below). The remaining 8-12 reports per run are all "GPU memory" races: the mock MCU reads/writes DRAM under its own lock while the core accesses the same WC memory (ring, interfaces, syncobjs) or the test writes a BO. That is the hardware boundary, so they are expected. |
| `meson setup … -Dcore=real` + `ninja` + `meson test` (cross ARM64 `panwin.sys`, private build dir) | links, 0 warnings after `ninja -t clean`, `meson test` 2/2 |
| glue harness `kmd/win/tests` (real `pw_os_win.c` + real core incl. the scheduler, private mirror) | `OK: 9 tests, 9286 checks, 0 failures` |

Bugs found by my own testing and fixed before this report:
- **Jobs waiting for ring space were never restarted when the last submit had already returned.**
  A completion now queues the work (only under load; seen in the parallel runs).
- **Lock-order inversion** between the file's handle lock and `sched->lock`, found by TSan.
- **Mock: a slot reused after CSG TERMINATE never restarted its CSs.** Real FW leaves the CSs
  stopped, so the mock now reports CS state STOP after TERMINATE.

## Not verified / needs hardware

- **All of it on a real G610 with Arm's `mali_csffw.bin`:**
  - CSG/CS programming and START/TERMINATE ack timing;
  - doorbell ids, `endpoint_req` priorities, `cs_irq_req`/`ack` and SYNC_UPDATE behaviour;
  - TILER_OOM counter semantics, FLUSH_CACHE2 with `latest_flush`;
  - the suspend/protm suspend sizes, and whether 64 MiB of MCU kernel VA is enough;
  - real CS fault/fatal codes.
- **The mock MCU is my reading of panthor's side of the protocol.** Where panthor doesn't show
  what the FW does, the mock guesses:
  - CS state after CSG TERMINATE;
  - when SYNC_WAITs are re-evaluated;
  - extract staying on the CALL while the callee runs;
  - IRQ raising rules.
- **The Windows OS layer under real concurrency:**
  - fence signalling from the job bottom half (passive-level work item, see core-sync concern 1);
  - the self re-arming delayed work item (`pw_os_win.c` work state machine);
  - `pw_os_udelay` spin in `job_disarm`.
- **Mesa panvk end to end.** Nothing has been run against panvk's real command streams.
- **The iface ≥ 4.0 `endpoint_req2` path.** The mock FW is v1.1.

## Known issues & TODO (prioritised)

1. **No rotation/eviction.** A group keeps its CSG slot until it is destroyed or dies.
   - With all slots bound (8 in the mock, at most 16 on HW after the MAX_CSG_PRIO clamp), ready
     jobs of other groups wait indefinitely. Waiting jobs have no timeout: the timer only runs
     for jobs in a ring.
   - Group priority is not honoured between groups; CSG priority is per slot.
   - Next step: port the tick (sched.c:2524) with suspend/resume.
2. **No GPU/FW reset.** CS_UNRECOVERABLE, CSG request timeouts and UNKNOWN states only fail the
   group and log. The FW may stay wedged. INNOCENT is never reported.
3. **AS exhaustion.** `pw_uvm_activate` returns EBUSY (more than 7 active VMs, pw_mmu.c has no
   eviction). Jobs then wait and are only retried when a slot is released, a submit happens or a
   fence fires.
4. **MMU faults are not pushed to the scheduler.** `pw_mmu.c` (not mine) doesn't call it. A
   faulted group is found on the next work run, i.e. at the latest the job timeout. A hook from
   the MMU IRQ (`pw_sched_report_mmu_fault`) would make it immediate.
5. **In-fence errors are not propagated** into the job's fence; the job runs anyway. The drm_sched
   behaviour here was not checked. At termination, jobs whose in-fences are still pending are
   cancelled at once instead of after their dependencies.
6. **Heap pools live in the file, keyed by VM**, not in the VM. VM_DESTROY unmaps them; they are
   freed only at file close.
7. **A stale CS TILER_OOM on a slot whose group was unbound is never acked**, so the next group on
   that slot could see a spurious OOM. Low risk: there is no eviction while an OOM is pending.
8. **Termination runs synchronously** in GROUP_DESTROY and file close, waiting up to 100 ms per
   CSG ack. The work item serialises on `sched->lock` with all ioctls that emit jobs.
9. **No profiling** (`PANTHOR_DEVICE_PROFILING_*`), no fdinfo, no protected mode, no
   `group->innocent`.

## Contract changes

All additions. No existing semantics changed.

- **`kmd/core/pw_sched.h`:**
  - the header comment no longer mentions the stub;
  - new `pw_sched_set_job_timeout()`, needed to test timeouts in under 5 s;
  - new `pw_heap_file_init/fini`, `pw_sched_file_heaps`, `pw_heap_grow`. These are the internal
    interface between pw_sched.c and pw_heap.c.
- **`kmd/core/pw_priv.h`:**
  - `struct pw_fw` gets `kva_lock` and `kva_list`;
  - `struct pw_device` gets `struct pw_sched *sched`;
  - new `pw_fw_map_kernel_mem()`/`pw_fw_unmap_kernel_mem()`, the MCU-VM kernel VA allocator the
    task asked for.
- **`pw_fw_job_irq_handler`**: CSG bits now go to `pw_sched_job_irq` instead of a debug log.
- **Tests:**
  - `mock_gpu.h` adds `mock_gpu_write`, `struct mock_cs_stats` and `mock_csf_*`;
  - the shared-section layout macros moved from `pw_os_mock.c` into the new `mock_internal.h`;
  - `Makefile` builds `mock_csf.c`.
- **Deviations from the task text, following panthor:**
  - the ring-buffer interface page is mapped in the MCU VM only. panthor does not map it in the
    user VM (fw.c:489-520).
  - the suspend buffers are in the MCU VM (fw.c:530), not the group's VM as
    kernel-panthor.md 3.3 says.

## Review

Independent review, 2026-10-30. **Still untested on hardware**: everything below ran against the
mock MCU only.

### Re-run from clean (by the reviewer)

| What | Before fixes | After fixes |
|---|---|---|
| `make -C kmd/tests clean && make -C kmd/tests` (gcc, ASan+UBSan) | 91 tests, 0 failed | 96 tests, 0 failed |
| `make -C kmd/tests clean && make -C kmd/tests CC=clang` | 91 tests, 0 failed | 96 tests, 0 failed |
| `make -C kmd/tests freestanding` | exit 0, no non-`pw_*` imports | exit 0, no non-`pw_*` imports |
| clang `-fsanitize=thread` build of the suite (scratchpad), `./pw_tests sched_` x3 | – | 18/18 each run. 18-23 reports per run, all involving the mock's emulated DRAM (`gpu_rw_locked`/`mr32`/`mw32`/`bo_rw`), i.e. the hardware boundary. No core-only race left after the fix below. |
| glue harness `kmd/win/tests` (copied to the scratchpad, built against the real core) | – | `OK: 9 tests, 9020 checks, 0 failures` |

The engineer's claimed results held up: the 91 tests, clang and freestanding runs all reproduced.
I did not redo the meson cross-build of `panwin.sys`.

### New tests (`kmd/tests/test_sched.c`, 5 tests)

- `sched_close_other_busy`: closing file A while file B has a job in flight.
- `sched_destroy_armed_dep`: GROUP_DESTROY of a group whose jobs have an armed in-fence callback,
  then the in-fence signals (ASan checks the callback was removed).
- `sched_same_queue_threads`: 4 threads x 150 submits onto 2 queues of one group (ring wrap,
  ring-full waits, lock contention); counters and fences checked.
- `sched_race_submit_destroy`: 3 submitters race GROUP_DESTROY of their group (6 rounds, 2
  queues). Every submit gives either EINVAL, or a fence that signals with 0 or ECANCELED.
- `sched_vm_destroy_live`: VM_DESTROY under a live group, then TILER_HEAP_DESTROY/CREATE on the
  dead VM (EINVAL), a submit (its fence must signal), group destroy, teardown.

### Bugs found and fixed (`kmd/core/pw_sched.c`)

1. **File close waited for other files' jobs.** `pw_sched_file_fini()` flushed the device-wide
   work item, and that item re-arms itself while any queue has a job in flight.
   - Effect: closing file A blocked until every job of every other file finished or timed out.
     Under continuous load from another process, that is unbounded.
   - Seen: `sched_close_other_busy` took 5014 ms, and B's job ended in ETIMEDOUT.
   - Fix: the flush is removed. `group_destroy()` already unbinds, terminates and releases each
     group synchronously under `sched->lock`/`events_lock`, and the work only reaches groups
     through those. The test now passes in ~40 ms.
2. **One GROUP_SUBMIT could end up with two different groups.** `job_create()` looked the handle
   up once per job. A GROUP_DESTROY plus GROUP_CREATE reusing the handle mid-submit could put
   jobs on two groups, and only `jobs[0]`'s group was run, so the other group's jobs could sit
   in `pending` until some unrelated event.
   - Fix: the group is looked up once per submit; the errnos are unchanged.
   - Not covered by a deterministic test (the window is too narrow).
3. **Data race on `group->state` and `group->destroyed`.** `group_can_run()` reads them without
   a lock in `job_create()` (panthor does the same), while they are written under
   `sched->lock`.
   - Formally UB in C11; found by TSan in `sched_threads`/`sched_race_submit_destroy`.
   - Fix: atomic load/store via `group_set_state()`.
4. **Uninitialised `acked`** in `csg_slot_update_locked()` when `pw_fw_csg_wait_acks()` returns
   early with PW_EINVAL (e.g. ifaces not ready). Now initialised to 0.

There are no contract changes: all four fixes are internal to `pw_sched.c`, plus test code.

### Review notes (checked, no change needed)

- **Against panthor:** GROUP_CREATE errnos and order; `prepare_job_instrs` encodings; queue
  completion and timeout reset; term post-processing errnos; FAULT marking by extract;
  TILER_OOM flow; heap checks, chunk links and context stride.
  - Minor deviation: a CS_FAULT error recorded on a job wins over the termination error.
    panthor's `dma_fence_set_error` overwrites it.
- **Lock order** is consistent: `sched->lock` → `events_lock` → `queue->lock` → `fw.req_lock`;
  `sched->lock` → `sf->lock`; `sched->lock` → `hf->lock`. Fences are never signalled under
  `queue->lock`.
- **User memory** is copied only in the ioctl prologue, before the point of no return.
- **License:** SPDX MIT everywhere, the panthor derivation is noted, nothing taken from GPL-only
  files, and the mock encodings come from genxml (MIT).
- **Timeline error reporting:** `pw_syncobj_find_fence(h, point)` returns the chain fence, and
  its error stays 0 when the payload failed. This belongs to core-sync and may match
  dma_fence_chain; the tests read errors through binary syncobjs.

### Remaining concerns (ranked)

1. **Slots are never released by idle groups** (no rotation). panvk keeps a group per VkQueue
   alive, so a 9th group (mock) or 17th (HW) in the whole system, across processes, waits
   forever and has no timeout. This is a practical blocker for multi-app use; port the tick or
   at least evict idle groups.
2. **A CSG TERMINATE that times out still frees and reuses the slot.**
   - The group's ring, syncobj and suspend buffers are then unmapped and freed while the FW may
     still run that CSG.
   - The slot is reprogrammed for the next group although the FW never acked.
   - Until there is a reset, quarantine such slots (never rebind them) and log loudly.
3. **No GPU/FW reset** (CS_UNRECOVERABLE, ack timeouts).
4. **MMU faults reach the scheduler late.** `pw_mmu.c` doesn't notify it, so a faulted group is
   found at the next work run, at worst the job timeout.
5. **Stale CS TILER_OOM after unbind** (TODO 7), and in-fence errors not propagated (TODO 5).
6. **Per-file `pw_os_work_flush` is gone; nothing else changed in teardown.** If a future change
   gives the work transient group references, it needs a per-group drain instead of a
   device-wide flush.
