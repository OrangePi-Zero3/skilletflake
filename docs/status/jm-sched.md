# jm-sched: job-manager scheduling (Mali-G31, panfrost uAPI)

Owner key: `jm-sched`. Task list C in [`jm-arch.md`](jm-arch.md). Date: 2026-10-30.
Files: `kmd/core/pw_jm_sched.c`, additions to `kmd/core/pw_jm_sched.h`, `kmd/tests/test_jm_sched.c`,
one hook in `kmd/core/pw_jm_hw.c/.h` (contract change 1).

**Everything here is untested on hardware.** It has only run against the mock G31 (jm-hw's
`mock_jm.c` executor and jm-mem's BOs).

## Summary

Tasks C1-C8 are done: SUBMIT, JM_CTX_CREATE/DESTROY, per-slot queues, dependencies, completion,
context/file teardown, timeouts and reset, plus a stress suite. Behaviour follows Linux
`panfrost_drv.c:panfrost_ioctl_submit` and `panfrost_job.c`, with drm_sched replaced by a small
model. GPL code was read for behaviour and errnos only.

- **Objects.**
  - A **context** (`struct pw_jm_ctx`) has a priority and one **entity** per slot, the FIFO of its
    jobs (drm_sched_entity). It is refcounted by the file table and by every job.
  - Each **slot** has a hardware queue (current + NEXT), a requeue list (jobs to put back first,
    after a reset or while no AS is free) and one run queue per priority.
  - Handles are 0..64 (`XA_LIMIT(0, MAX_JM_CTX_PER_FILE)`). Handle 0 is the MEDIUM default context
    (`panfrost_jm_open`). A full table gives EBUSY, as `xa_alloc` does.
- **SUBMIT** checks, in Linux's order: pad, jc, requirements (FS | CYCLE_COUNT), out_sync (ENODEV),
  context (EINVAL), in_syncs (ENOMEM over 2^16, EFAULT, ENOENT for no syncobj, EINVAL for no
  fence), then `pw_jm_bos_lookup`.
  - Slot: FS → JS0, everything else → JS1. JS2 is unused, as in panfrost.
  - The job takes references on its BOs, the file VM, its context and, with CYCLE_COUNT, the cycle
    counter. It samples the flush id.
  - Then, under `pw_jm_resv_lock`, every BO fence becomes a dependency (all BOs count as written,
    `panfrost_acquire_object_fences`), the job is queued, and its fence becomes every BO's fence.
  - Finally out_sync gets the fence (`pw_syncobj_replace_fence`, binary only).
  - Nothing user-visible changes on failure.
  - The ioctl never waits for the GPU: it queues the job and, if the slot has room, programs it.
- **Dependencies.** Only an entity's head job is looked at. A callback is armed on its first
  unsignalled dependency; the callback only queues the work item, which then feeds the slots.
  - A dependency on a job already in the same slot's hardware queue counts as met. drm_sched does
    the same: it waits only for the "scheduled" fence of a same-scheduler dependency. So
    dependent jobs pipeline through NEXT.
  - That shortcut never lets a job that is cancelled signal before its dependency.
- **Slot feeding.**
  - Order: the requeue list first, then the highest-priority run queue, round robin between
    entities of the same priority.
  - Two chains per slot when the GPU has JOBCHAIN_DISAMBIGUATION (every Bifrost part), else one.
  - Each job gets a JOB_CHAIN_FLAG different from the current job's (seqno parity, as
    `panfrost_get_job_chain_flag`).
  - `pw_jm_as_get` returning EBUSY, or a NEXT register that is not free yet, puts the job at the
    front of the requeue list. It is retried at the next job event.
- **Completion** (`pw_jm_sched_job_irq`, from the job bottom half) applies `panfrost_jm_handle_irq`'s
  rules.
  - Done means fence error 0. A done bit with an idle slot completes the NEXT job too.
  - TERMINATED → -ECANCELED. A fault → -EINVAL. STOPPED → resume from JS_TAIL.
  - Other non-fault statuses → 0.
  - After a failure, the job the handler cancelled in NEXT is restarted, or cancelled if its
    context was destroyed.
  - The AS reference is dropped there. Fences are signalled outside every scheduler lock; BO, VM
    and context references are dropped later in the work item.
- **Context destroy and file close** follow `panfrost_jm_ctx_destroy`.
  - A job in NEXT is cancelled and hard-stopped with its own chain flag; the current job is
    hard-stopped with its flag. Another context's chain in the same slot keeps running.
  - Requeued and queued jobs never start. They fail with -ECANCELED once their dependencies
    signalled, which is drm_sched's teardown order, so the one-fence-per-BO model holds (jm-mem
    review concern 2).
  - JM_CTX_DESTROY returns at once.
  - File close waits until none of the file's jobs is the current job of a slot (see
    contract change 3).
- **Timeouts** default to 500 ms (`panfrost_job.c JOB_TIMEOUT_MS`), counted from when a job
  becomes the current job of its slot. `pw_jm_sched_set_job_timeout` sets it for later submits.
  - A delayed watchdog work item re-arms itself for the earliest deadline.
  - On expiry it waits for a running job bottom half and looks again (`panfrost_job_timedout`
    does the same), then resets.
- **Reset.** Feeding stops and `pw_jm_hw_reset_collect()` runs, with no scheduler lock held. Then
  for each slot:
  - completed chains complete;
  - the guilty job, and later jobs of its context in that slot's queue, get -ECANCELED
    (drm_sched karma and `drm_sched_resubmit_jobs`' guilty-context rule);
  - killed jobs get -ECANCELED and faulted ones -EINVAL;
  - innocent jobs go back to the front of the requeue list in order, resuming from JS_TAIL.

  If the reset fails, every later job ends with -EIO.
- **Locking.**
  - Paths outside the bottom half that program a slot take the job line's `bh_lock` before
    `s->lock`. This matches Linux holding `job_lock` across `panfrost_jm_handle_irq` and around
    `JS_COMMAND_NEXT = START`.
  - The job bottom half cancels NEXT, clears JOB_INT and samples JS_STATE *before* it calls
    us. A START issued in between corrupts that event. Stress testing found this (Known
    issues 1).
  - `pw_jm_hw_reset` is never called with a scheduler lock held.

## How to build/test

```sh
R=/home/e3xp0/Projects/panfrost-win/repo
export PW_FW_PATH=/home/e3xp0/Projects/panfrost-win/references/firmware/mali_csffw.bin
make -C $R/kmd/tests                         # gcc, ASan+UBSan, -Werror (shared tree: no 'make clean')
make -C $R/kmd/tests freestanding            # aarch64 -ffreestanding compile, undefined symbols pw_* only
# private copy for clang / repeated runs
W=/tmp/claude-1000/jmsched; mkdir -p $W && cd $W && rm -rf kmd include toolchain && \
  cp -r $R/kmd $R/include $R/toolchain . && rm -rf kmd/tests/obj kmd/tests/pw_tests
make -C kmd/tests CC=clang
cd kmd/tests && ./pw_tests jm_               # JM suites only; PW_TEST_VERBOSE=1 for the core log
for i in $(seq 10); do ./pw_tests jm_ > run_$i.log 2>&1 & done; wait; grep -h "tests," run_*.log
cd $W && meson setup b-real kmd --cross-file toolchain/meson/aarch64-w64-kmd.ini -Dcore=real && \
  ninja -C b-real && meson test -C b-real
make -C kmd/win/tests
```

## Verified (run here, 2026-10-30)

- **Shared tree, gcc:** `make -C repo/kmd/tests` gave 148 tests, 0 failed, 0 skips. The earlier jm-sched
  placeholders are now real tests, and every CSF test is unchanged.
- **Private copy, clang from clean:** `make CC=clang` gave 148 tests, 0 failed.
- **Freestanding:** `make freestanding` compiles every core file for aarch64; every undefined symbol
  is `pw_*`. `pw_jm_sched.c` also compiles with `x86_64-w64-mingw32-clang -Werror` (the libdrm-win core
  transport compiler). The full libdrm-win build was not run.
- **Windows builds:** meson `-Dcore=real` links `panwin.sys`; `meson test` OK 4, Fail 0.
  `make -C kmd/win/tests` gave 10 tests OK.
- **Stress:** `./pw_tests jm_` in 10 parallel copies × 5 rounds (52 JM tests each), all passing.
  - Before the bh_lock fix, 8 copies × 3 rounds failed 2 of 24 runs: a mock model error "START while
    NEXT is still queued", and lost writes in `jm_stress_threads`.
  - After the fix: 8 × 4 and then 10 × 5 rounds, no failures.
- **License:** a 7-word n-gram scan of my three files against Linux panfrost `*.c/*.h` found 0 matches.
- **The 18 tests in `test_jm_sched.c`** (all through `pw_file_ioctl` with `panfrost_drm.h` structs; job
  chains are v7.xml WRITE_VALUE / NULL / FRAGMENT descriptors written through the user mapping of a
  BO). Each one ends with `jenv_fini`'s leak check, 0 mock model errors and 0 unclaimed IRQs:
  - `jm_submit_validation`: every errno and the precedence between them. After failures nothing
    changed: out_sync has no fence, the BO has no fence, no job exists, the cycle counter is off.
    Then a valid CYCLE_COUNT submit with a duplicate BO runs, and the counter is released once
    the job is freed.
  - `jm_ctx`: create/destroy errnos (default context, pad, unknown, HIGH → EACCES, bad priority);
    handles 1..64 then EBUSY; lowest-handle reuse; submit to a destroyed handle → EINVAL; a job on
    a LOW context; file close with 64 contexts.
  - `jm_submit_write_value`: a 3-job chain writes IMM32/IMM64; the descriptors read DONE; out_sync
    is signalled (SYNCOBJ_WAIT); JS1 was used; WAIT_BO succeeds. Then panvk's in_sync == out_sync
    pattern.
  - `jm_slot_select`: FS → JS0 (WRITE_VALUE and FRAGMENT), non-FS → JS1; FRAGMENT without FS gives a
    JS1 config fault → -EINVAL.
  - `jm_in_sync_cross_file`: a fence from file 1's stalled job, imported into file 2's syncobj. File 2's
    job does not start (the ioctl returns, SYNCOBJ_WAIT polls ETIME) until the kick; signal order
    holds.
  - `jm_implicit_sync`: B on JS0 waits for A on JS1 through a shared BO. WAIT_BO gives EBUSY on a
    poll and ETIMEDOUT for a 5 ms deadline, then OK. B's value wins.
  - `jm_pipeline`: two dependent jobs on JS1. The second sits in NEXT behind the stalled first, a
    third is queued, and the chain flags differ.
  - `jm_job_faults`: an injected bus fault, a type-0 descriptor and an unhandled page fault (AS
    re-enabled by the next job) each give -EINVAL. A job that was promoted from NEXT and then
    failed, with the one behind it still completing.
  - `jm_timeout_reset`, phase 1: a hung job (300 ms timeout) gives -ECANCELED. The other context's
    job in NEXT is resubmitted and completes. A stalled JS0 job with a long timeout is
    terminated by the reset and resumed from JS_TAIL. Checks: the log line, one soft reset,
    stats timeouts=1 and resets=1.
  - `jm_timeout_reset`, phase 2: the guilty context's job in NEXT gets -ECANCELED and the other
    context's queued job runs. A later job runs.
  - `jm_ctx_destroy_inflight`, part 1: destroying a context with a running job and a job in NEXT
    gives -ECANCELED for both. The second never started.
  - `jm_ctx_destroy_inflight`, part 2: destroying a context whose job sits in NEXT behind another
    context's running chain cancels only NEXT; the chain-flag hard stop leaves the running chain
    alone. The job completes -ECANCELED with that slot's event.
  - `jm_ctx_destroy_inflight`, part 3: a queued job waiting on a dependency is cancelled only after
    that dependency signalled.
  - `jm_cancel_order` (`as_present = 0x1`): the same ordering for a destroyed context's job that
    entered the slot early through the scheduled-dependency shortcut, both in NEXT and in the
    requeue list (AS busy, stats `as_busy`). Without the fix in Known issues 2, it signalled
    early.
  - `jm_file_close_inflight`: at close, the running job and the job in NEXT are -ECANCELED. A job
    waiting on another file's stalled job stays queued and is cancelled once that job
    completes (it never runs). The closed file's memory is freed.
  - `jm_close_hung`: file close with a hung chain; the hard stop ends it, no reset.
  - `jm_submit_heap`: WRITE_VALUE into two unpopulated 2 MiB heap chunks: 2 faults, 2 retries, values
    read through the GPU view, no unhandled fault.
  - `jm_priority`: with the slot full, a MEDIUM job queued after a LOW one runs first (mock job log).
  - `jm_many_submits`: 400 jobs alternating JS0/JS1, all ordered by implicit sync; every value is
    written; submitted == completed == runs.
  - `jm_stress_threads`: 4 files × 2 threads × 130 jobs on 2 AS slots (LRU eviction, about 500 AS
    waits per run), random slots, contexts and in_syncs. Every value is written, no errors, no
    reset.
  - `jm_teardown_races`: two threads create, submit to and destroy contexts (EINVAL after destroy).
    Two threads open files, submit 6 jobs and close at once. completed == submitted, no reset.

## Not verified / needs hardware

- All of it. In particular:
  - Whether the NEXT registers read NOP right after a START once the slot is idle. We poll up to
    20 µs, then retry at the next event.
  - JS_TAIL after a *hard* stop as a resume point. The reset hard-stops (jm-hw); Linux soft-stops
    and resumes STOPPED chains from the tail. The mock reports the first unfinished job.
  - Whether JS_STATE and the done rule behave as modelled when two chains finish under one done
    bit.
  - That JOB_CHAIN_FLAG-selected hard stops spare the other chain on the G31.
  - Real IRQ latency against the 500 ms watchdog.

## Known issues & TODO (prioritised)

1. **The bh_lock coupling.** The scheduler takes `dev->irq[PW_IRQ_JOB].bh_lock`, which belongs to
   `pw_irq.c`, and relies on `pw_irq_bottom` holding it around the handler.
   - This is needed because jm-hw's handler cancels NEXT and clears JOB_INT before it calls us.
   - Cleaner: an explicit jm-hw contract, e.g. the handler calls the scheduler under a "slot lock"
     the scheduler provides.
   - Lead / jm-hw: keep the property, or replace it with such a contract.
2. **Fixed here:** dependencies met through the scheduled-fence shortcut were consumed. A job that
   was then cancelled (context destroyed while it waited in NEXT or in the requeue list) could
   signal before that dependency. Now only signalled fences are consumed (`jm_cancel_order`).
3. **Reset resumes innocent hard-stopped chains from JS_TAIL** (see above).
   - Re-running from the original jc instead would replay tiler jobs.
   - Better: jm-hw's TODO 4, soft-stop first as Linux does. Then STOPPED gives a defined resume
     point.
4. **A short timeout set while a long one is armed** relies on `pw_os_work_queue` overriding a
   pending delayed run, which the mock does. Where it is a no-op (the documented contract),
   detection is late, up to the longer timeout. Only `set_job_timeout` changes create this
   situation.
5. **A killed job cancelled in NEXT** behind another context's chain completes (-ECANCELED) only
   with that chain's event, as in Linux. File close does not wait for it.
6. **Ambiguous done-with-NEXT after a reset** is counted as two completions, like Linux (pw_jm_hw.h
   caveat).
7. **After a failed reset**, SUBMIT still succeeds and jobs end with -EIO. Linux has no such state.
   Not tested: the mock cannot fail a reset after probe.
8. **No starvation protection for LOW contexts** (the highest priority always wins, as drm_sched's
   run-queue order). A job requeued because no AS was free goes before higher-priority jobs.
9. **Not implemented:** fdinfo engine usage / profiling (`is_profiled`), JS2, perf counters,
   HIGH-priority contexts (EACCES, jm-arch open question 7).
10. **Userspace:** Mesa's `panfrost_kmod.c` is still not built on Windows (jm-arch open question 1).
    Nothing above has run under Mesa.

## Contract changes

All are extensions or documentation; CSF is untouched.

1. `pw_jm_hw.h` / `pw_jm_hw.c`: new `pw_jm_slot_depth(dev)`. It returns 2 with
   JOBCHAIN_DISAMBIGUATION, 1 without, 0 before init (`panfrost_jm_init` credit_limit). The
   feature flag is private to `pw_jm_hw.c`, and the scheduler needs it to decide whether to use
   NEXT.
2. `pw_jm_sched.h` additions:
   - `PW_JM_MAX_CTX_PER_FILE` (64; JM_CTX_CREATE on a full table → PW_EBUSY, as `xa_alloc`);
   - `PW_JM_MAX_IN_SYNCS` (2^16, more → PW_ENOMEM);
   - behaviour notes (fence errors, cancel ordering, scheduled dependencies, timeout start);
   - `struct pw_jm_sched_stats` and `pw_jm_sched_get_stats()` for tests and debugging.
3. `pw_jm_sched.h`, `pw_jm_sched_file_fini` comment, clarified: "cannot touch the hardware" means no
   job of the file is the current job of a slot. A job cancelled out of NEXT behind another
   context's chain, and queued jobs waiting on dependencies, are not waited for: they cannot
   start. Waiting for them would block a file close for another process's chain.
4. Not a header change, but an assumption: `pw_irq_bottom()` holds the line's `bh_lock` around
   the handler (Known issues 1).

## Review

Independent review, 2026-10-30. Everything below ran against the mock only; **untested on hardware**.

### Re-ran (private copy `/tmp/claude-1000/rev-jmsched`, built from clean)

- Before any change:
  - `make` (gcc, ASan+UBSan, -Werror): 148 tests, 0 failed, 0 skips.
  - `make CC=clang`: 148 tests, 0 failed.
  - `make freestanding`: every core file compiles, and every undefined symbol is `pw_*`.
  - `./pw_tests jm_` in 12 parallel copies × 3 rounds: 36/36 passed (52 tests each).
- After the fixes:
  - gcc: 149 tests, 0 failed. clang from clean: 149 tests, 0 failed.
  - freestanding OK, and `pw_jm_sched.c` compiles with `x86_64-w64-mingw32-clang -Werror`.
  - `./pw_tests jm_` (clang build) 12 × 3: 36/36 passed (53 tests each).
  - meson `-Dcore=real`: `panwin.sys` links, meson test OK 4 / Fail 0. `make -C kmd/win/tests`:
    10 tests OK.
  - In the shared tree (no clean): `make -C kmd/tests` gives 149 tests, 0 failed; `make freestanding`
    OK.
- License: a word-level 6-gram scan of `pw_jm_sched.c` and `test_jm_sched.c` against Linux
  panfrost `*.[ch]` found 0 matches.

### Checked against the references

The checks match Linux. These checked out:

- SUBMIT errno order (`panfrost_ioctl_submit`, `panfrost_copy_in_sync`).
- JM_CTX_CREATE/DESTROY: EACCES for HIGH, EINVAL otherwise, `xa_alloc` lowest handle in
  [0, 64], EBUSY when full. Handle 0 and pad give EINVAL.
- Slot choice (`panfrost_job_get_slot`), and every BO treated as written.
- The fence errors of `panfrost_job_handle_err` (TERMINATED gives -ECANCELED, a fault -EINVAL,
  STOPPED resumes from the tail).
- The done-with-idle-JS_STATE rule and the NEXT restart after a failure
  (`panfrost_jm_handle_irq`).
- The guilty-context cancel of `drm_sched_resubmit_jobs`, `JOB_TIMEOUT_MS` 500, and credit limit
  2 / 1.

Mesa's gallium `pan_jm.c` uses `jm_ctx_handle`, one in_sync and out_sync, as tested. The
Windows paths were also checked:

- The bottom half is a KMDF passive-level interrupt work item.
- `pw_os_mutex` is a FAST_MUTEX taken with the Unsafe variants, at passive level.
- User copies happen only in the ioctl, which runs in the caller's context; callbacks and
  work items never touch user memory.

The engineer's claims held up.

### Bugs found and fixed

1. **File close missed jobs of contexts destroyed earlier.** `file_busy_locked()` matched
   jobs by `ctx->idle_ev`, which is only set for contexts still in the table at close. A job
   of a context destroyed before with JM_CTX_DESTROY could still be the current job of a
   slot, and `pw_jm_sched_file_fini()` returned anyway. That breaks the header contract.
   Mesa's usual sequence (context destroy, then fd close) hits it.
   - Fix: every context records a never-reused file id (`ctx->owner`, from `s->next_owner`),
     and the close matches on that.
   - Such a context has no `idle_ev`, so its wake-up is the existing 100 ms poll.
   - Test: `jm_close_after_ctx_destroy` holds the job IRQ back so the hard stop's event stays
     pending. It failed before the fix (the close returned at once) and passes after it.
2. **A slot could stall forever on "NEXT busy on an idle slot".** `hw_submit()` gives
   `PW_EAGAIN` and requeues the job, and the retry waited for a job event. With nothing of
   that slot on the GPU, no event comes. A requeued job has no watchdog deadline, so only a
   later SUBMIT would ever retry it.
   - Fix: in that case the work item is queued with a 1 ms delay (`JM_NEXT_RETRY_MS`). The
     error is logged once per episode instead of at every retry.
   - The mock cannot produce this state, so there is no test for it.
3. **Racy checks in the tests.** `jm_file_close_inflight` and `jm_close_hung` checked
   `pw_fence_is_signalled()` right after `pw_file_close()`. The bottom half takes the job off
   the slot under `s->lock`, but signals its fence only after it drops the lock, so the close
   can return first. They now wait (`fence_result`) and check the slot is idle instead.

No header changed, so there are no new contract changes.

### Remaining concerns (ranked)

1. **The reset hard-stops, and innocent chains resume from JS_TAIL.**
   - Linux soft-stops and resumes only STOPPED chains. Where the soft stop fails, it reruns
     from the original jc.
   - What JS_TAIL means after a hard stop is unverified. A resumed half-done job could
     corrupt state.
   - Needs jm-hw's soft-stop-first reset (their TODO) and a board test.
2. **The `bh_lock` coupling (Known issues 1).** It is correct today, but it is an implicit
   contract on `pw_irq.c`. Make it explicit in `pw_jm_hw.h`/`pw_irq.h`.
3. **Done-with-NEXT after a reset.** A NEXT job that the reset cancelled before it started is
   reported as done, with fence error 0. Linux has the same flaw. The registers cannot tell
   the two cases apart.
4. **The failed-reset (-EIO) path and fix 2 are untested.** The mock has no runtime knob to
   fail a reset after probe (for example toggling `no_shader_ready`), and none to keep NEXT
   busy on an idle slot. Ask jm-hw for both.
5. **Fence signal order can invert.** Fences completed outside the bottom half (reset, work
   item, ioctl) are signalled after the scheduler lock is dropped, so a later job's fence can
   signal first. This only affects jobs that already finished on the GPU or never ran, and
   binary syncobjs have no ordering contract, so no memory hazard was found.
6. **`PW_JM_MAX_IN_SYNCS` gives ENOMEM**, where Linux would copy and give EFAULT/ENOENT.
   Mesa never comes near the bound.
