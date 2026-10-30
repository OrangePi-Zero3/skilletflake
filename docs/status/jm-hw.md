# jm-hw status

Owner key: `jm-hw`. Task list A in [`jm-arch.md`](jm-arch.md). Date: 2026-10-30.
Files: `kmd/core/pw_jm_hw.c`, `kmd/core/pw_regs_jm.h`, additions to `kmd/core/pw_jm_hw.h`,
`kmd/tests/mock_jm.c`, `kmd/tests/test_jm_hw.c`, and the G31 parts of `kmd/tests/mock_gpu.h`,
`mock_internal.h` and `pw_os_mock.c`.

**None of this has run on hardware.** Every register sequence below is untested on hardware,
and was checked only against the mock G31 (which models what we believe the hardware does).

## Summary

The job-manager hardware layer of the G31 (Bifrost v7) backend is implemented (A1-A8), plus a
mock G31 job manager that runs job chains (A6).

- **Init/fini/release** (`pw_jm_hw_init`): reads the JM-only feature registers into
  `struct pw_jm_gpu_info` (`panfrost_gpu.c:panfrost_gpu_init_features`: JS_PRESENT,
  JS_FEATURES for hweight(JS_PRESENT) slots, STACK_PRESENT, THREAD_TLS_ALLOC, AFBC_FEATURES,
  nr_core_groups). A small table of the Bifrost parts Linux knows, with our own encoding of
  the features and errata that change what we program: JOBCHAIN_DISAMBIGUATION,
  FLUSH_REDUCTION, TLS_HASHING, IDVS_GROUP_SIZE, T76X_3953. G31 quirks: `SHADER_CONFIG =
  SC_TLS_HASH_ENABLE`, `TILER_CONFIG` read back unchanged, no `JM_CONFIG` write and no
  `L2_MMU_CONFIG` write. Linux defines 3BIT_EXT_RW_L2_MMU_CONFIG for the G31 but never writes
  that register. It logs the same lines panfrost does. fini hard-stops the slots, unbinds every
  AS, powers off the shader cores and tiler, and stops the cycle counter. fini is idempotent and
  copes with a partial init.
- **Power-up** (`panfrost_gpu.c:panfrost_gpu_power_on`): quirks, then the shader cores of the
  first core group, then the tiler. Each block waits for PWRTRANS to clear, writes PWRON, and
  polls READY (shader 20 ms, tiler 1 ms), with PW_ETIMEDOUT on failure. The shared
  `pw_gpu_l2_power_on` has already powered the L2. Core stacks are not touched, as in Linux.
- **GET_PARAM** (`panfrost_drv.c:panfrost_ioctl_get_param`): every `DRM_PANFROST_PARAM_*` of
  the uAPI, so every param Mesa's `panfrost_kmod.c` reads is covered.
  - `pad != 0` and unknown params return EINVAL.
  - JS_FEATURES past JS_PRESENT and TEXTURE_FEATURES1-3 read as 0.
  - SYSTEM_TIMESTAMP reads GPU_TIMESTAMP under the reset lock, with the cycle counter held
    around the read.
  - SYSTEM_TIMESTAMP_FREQUENCY is `fw.timer_hz` or 24 MHz.
  - ALLOWED_JM_CTX_PRIORITIES is LOW|MEDIUM, SELECTED_COHERENCY is NONE.
- **Slot helpers** (`panfrost_job.c:panfrost_job_hw_submit`):
  - HEAD/AFFINITY(=SHADER_PRESENT)/CONFIG/FLUSH_ID_NEXT, then a store barrier, then
    `JS_COMMAND_NEXT = START`.
  - CONFIG = AS | THREAD_PRI(8) | START/END_FLUSH_CLEAN_INVALIDATE | ENABLE_FLUSH_REDUCTION |
    the optional JOB_CHAIN_FLAG. START_MMU is not set (see contract change 1).
  - Also: next_free, stop (_0/_1 variants under JOBCHAIN_DISAMBIGUATION), cancel_next,
    status/head/tail, latest flush id, the cycle-counter refcount, and JM exception names.
- **Job IRQ** (`panfrost_job.c:panfrost_jm_handle_irq`): one handler pass repeats these steps
  until RAWSTAT is 0, then delivers one `pw_jm_irq_event` to `pw_jm_sched_job_irq`:
  1. For each failed slot, write `JS_COMMAND_NEXT = NOP` and read STATUS/HEAD/TAIL.
  2. Clear JOB_INT.
  3. Sample JS_STATE for the cleared slots.

  A test/bring-up hook can take the events instead of the scheduler.
- **Reset** (`panfrost_job.c:panfrost_reset` + `panfrost_device.c:panfrost_device_reset`):
  1. Mask the job and MMU lines and wait for running bottom halves.
  2. NOP the NEXT registers and hard-stop the slots, then wait up to 10 ms for JS_STATE to
     clear.
  3. Soft reset, then power the L2, shader cores and tiler back on, and re-apply the quirks.
  4. Restart the cycle counter if it has users.
  5. Forget every AS binding, keeping the users counts.
  6. Unmask the lines.

  A failed reset marks the device dead: later submits are dropped and logged.
- **AS LRU** (`panfrost_mmu.c:panfrost_mmu_as_get/put`): a table of up to 32 users, keyed by
  `pw_jm_vm`, sitting on `pw_mmu_as_bind/unbind_locked` under the VM's op_lock and then
  `slots_lock`.
  - An already-bound VM gets a new user and becomes most recently used; if a fault disabled its
    slot, it is re-enabled.
  - Otherwise it takes the lowest free slot, or evicts the least recently used slot whose users
    count is 0. If every slot has users, the result is PW_EBUSY.
  - `pw_jm_as_vm_get` returns a `pw_jm_vm_tryget` reference, for real `pw_jm_vm` users only.
- **Mock G31 job manager** (`mock_jm.c`, the full model is in its header):
  - START latches the NEXT registers. NEXT moves to current when the slot is idle and no error
    is pending.
  - An executor thread walks each chain through the slot's AS using the Mesa v7.xml Job Header,
    and writes exception status DONE back to each finished job.
  - WRITE_VALUE executes all 7 types. NULL and CACHE_FLUSH run; COMPUTE, VERTEX, GEOMETRY,
    TILER, FUSED, FRAGMENT and INDEXED_VERTEX are counted no-ops (logged with
    `PW_TEST_VERBOSE`).
  - Dependencies 1/2 are checked against the job index.
  - Config faults: an invalid type, Is 64b clear, a misaligned descriptor, a job type the
    slot's JS_FEATURES does not list, a bad WRITE_VALUE type, a dependency on a job later in the
    chain, or an AS that is not present. An affinity outside SHADER_PRESENT is an affinity
    fault.
  - Page faults: AS_FAULTSTATUS (exception type | access type) and AS_FAULTADDRESS are set, an
    MMU IRQ is raised, and the job stalls. It retries once the fault is cleared and the VA is
    mapped (heap growth). If the AS is disabled instead, the job fails with the fault code.
  - Soft and hard stops are modelled. JS_STATE is modelled. JS_CONFIG flush bits are
    bookkeeping only; LATEST_FLUSH_ID advances on each flush.
  - Injection knobs: hang (never completes), stall-until-kicked, fail-with-status.
  - DONE/ERR interrupts are raised from the executor thread, so they arrive asynchronously.

## How to build/test

```sh
make -C /home/e3xp0/Projects/panfrost-win/repo/kmd/tests                 # gcc, ASan+UBSan, -Werror
make -C /home/e3xp0/Projects/panfrost-win/repo/kmd/tests clean && \
  make -C /home/e3xp0/Projects/panfrost-win/repo/kmd/tests CC=clang      # (in a private copy if others test)
make -C /home/e3xp0/Projects/panfrost-win/repo/kmd/tests freestanding    # aarch64 kernel compile check
cd /home/e3xp0/Projects/panfrost-win/repo/kmd/tests && ./pw_tests jm_    # JM suites only
PW_TEST_VERBOSE=1 ./pw_tests jm_chain_deps                                  # core log + mock executor log
```

Private copy for repeated runs: `cp -r repo/kmd repo/include /tmp/claude-1000/jmhw/`, then
`PW_FW_PATH=/home/e3xp0/Projects/panfrost-win/references/firmware/mali_csffw.bin` (the default
firmware path is relative to the tree).

## Verified (run here, 2026-10-30)

- `make -C repo/kmd/tests` (shared tree, gcc, ASan+UBSan, -Werror): **130 tests, 0 failed,
  3 skips**. The 3 skips are jm-sched stubs (SUBMIT, JM_CTX, job flow).
- Private copy, `make CC=clang` from clean: 130 tests, 0 failed, 3 skips. Private copy, gcc
  from clean: same result.
- `make -C repo/kmd/tests freestanding`: every core file compiles for aarch64 with
  `-ffreestanding`, and `pw_jm_hw.c` references no libc symbols.
- Stress: `./pw_tests jm_` in 8 parallel copies × 4 rounds, all passing. An earlier round found
  two test races (see Known issues 7), which are fixed.
- Private copy: `meson setup … kmd --cross-file toolchain/meson/aarch64-w64-kmd.ini
  -Dcore=real && ninja && meson test`. panwin.sys links and the 4 tests pass.
  `make -C kmd/win/tests`: 9 tests OK.
- `pw_os_mock.c` and `mock_jm.c` compile with `x86_64-w64-mingw32-clang -Werror`, which is what
  the libdrm-win core transport build uses. The full libdrm-win build was not run.
- The new tests in `test_jm_hw.c`, all passing:
  - `jm_get_param`: every param against `mock_gpu.h`, the whole enum accepted, timestamp
    monotonic with the cycle counter released afterwards, pad and unknown params return EINVAL.
    Goes through `pw_file_ioctl`.
  - `jm_power_up`: L2/SHADER/TILER READY, the TLS-hash quirk, the job IRQ mask `0x70007`, the
    info struct, the probe log, and fini run twice (powered off, masked).
  - `jm_init_failures`: `no_shader_ready` and `no_tiler_ready` make probe return ETIMEDOUT,
    with no leaks.
  - `jm_write_value_job`: a 7-job kernel-built WRITE_VALUE chain on JS1 (all 7 value types),
    submitted through `pw_jm_as_get_vm` + `pw_jm_slot_submit`. The DONE event arrives, the
    memory written is checked through the GPU view, and every descriptor's status is DONE.
    Also checks CONFIG/AFFINITY/FLUSH_ID as programmed, flush-reduction bookkeeping and
    JOB_CHAIN_FLAG.
  - `jm_chain_deps`: a 7-job dependent chain (NULL, WRITE_VALUE, CACHE_FLUSH, COMPUTE, VERTEX,
    TILER, INDEXED_VERTEX) runs in order. A FRAGMENT job runs on JS0. Config faults: a
    dependency on a later job, FRAGMENT on JS1, a type-0 job. The event carries status 0x40 and
    the head; the slot is usable afterwards.
  - `jm_page_fault`: an unmapped write raises the MMU IRQ with the AS fault registers set; the
    unhandled path disables the AS and the job fails with a translation fault. A later
    `as_get` re-enables the AS and a job runs. A heap-growth style retry (map + MMU clear)
    completes the stalled job. An injected failure status reaches the event.
  - `jm_slots_and_stops`: next_free, JS_STATE bits, cancel_next, the wrong _0/_1 variant
    ignored, soft stop gives STOPPED with tail = the resume point, and the handler cancels the
    queued NEXT job (it never ran). Resume from the tail completes. A hung chain ignores soft
    stops; a matching hard stop gives TERMINATED. Slot 5 is refused.
  - `jm_irq_done_then_failed`: a single handler pass reports DONE and ERR on JS1 (the first
    job done, the second failed) in one event.
  - `jm_reset`: reset with a hung job holding the cycle counter and an AS. The GPU is
    re-powered, the quirk re-applied, all three masks re-armed, the cycle counter restarted, AS
    bindings forgotten, no event delivered, and putting the users back raises no warning. Jobs
    run afterwards. A second reset of an idle device also works.
  - `jm_as_lru` (`as_present = 0x3`, 4 users): free-slot order, reuse, EBUSY when everything is
    busy, eviction of idle slots only in LRU order, re-enable after an unhandled fault,
    `as_vm_get` NULL for bare VMs, put without get, and release.
  - `jm_as_lru_files`: the same over real file VMs (jm-mem B1 is now in the tree), including
    `pw_jm_as_vm_get` → `pw_jm_vm_tryget` and the unbind on file close.
  - `jm_hw_misc`: exception names, LATEST_FLUSH_ID advancing, the cycle-counter refcount.
- Every JM test ends with `check_no_leaks`, 0 model errors and 0 unclaimed IRQs (`jenv_fini`).

## Not verified / needs hardware

- Everything above is untested on hardware. These points in particular:
  - The G31's real feature register values (`mock_gpu.h` guesses; the Z1 `panwin-info` dump).
  - Whether READY follows PWRON on the H616 with only the L2, shader cores and tiler powered:
    we do not touch the core stacks, as in Linux.
  - Whether NEXT is held back while an ERR is pending. This is inferred from how Linux
    cancels and requeues NEXT, and the mock models it that way.
  - JS_HEAD and JS_TAIL contents after a fault or stop. The mock sets both to the job
    concerned, and TAIL to the resume point.
  - The JS_STATUS of a job killed by disabling its faulting AS: the mock reports the fault's
    exception type.
  - Whether a soft reset clears JOB_INT and MMU_INT RAWSTAT and stops the cycle counter. The
    mock clears both and stops it, and the core re-arms anyway.
  - The width of WRITE_VALUE ZERO: the mock writes 64 bits.
  - The SYSTEM_TIMESTAMP frequency: `pw_device_alloc` does not store
    `pw_device_params.timer_hz`, so on JM `fw.timer_hz` is 0 and we report 24 MHz. That is
    right for the H616, but see TODO 3.

## Known issues & TODO (prioritised)

1. **For jm-sched (C):** the event contract has these consequences (now in `pw_jm_hw.h`):
   - One event can have both `done` and `failed` for a slot (done first).
   - For a failed slot, NEXT is already cancelled: resubmit with `pw_jm_slot_submit`.
   - `js_state` is the raw register (bit js = current, bit js+16 = NEXT).
   - `pw_jm_hw_reset` waits for a running job bottom half, so `pw_jm_sched_job_irq` must never
     block on the lock held around the reset.
   - The reset delivers no events: the caller fails or resubmits the jobs and drops their AS
     and cycle-counter references.
   - Linux WARNs if the job in NEXT carries the same JOB_CHAIN_FLAG as the current job, so
     alternate the flag (e.g. the fence seqno parity, as `panfrost_get_job_chain_flag` does).
2. Reset forgets AS bindings by calling `pw_mmu_as_unbind_locked` under `slots_lock` without
   each VM's op_lock. A page-table update that races with a reset could issue an UNLOCK on an
   AS the reset already wiped. The mock would flag that as a model error; on hardware it is
   probably harmless. An op_lock-first walk needs VM references, which the table does not
   hold.
3. Lead: store `params->timer_hz` in `pw_device_alloc` (e.g. into `fw.timer_hz`) so that
   SYSTEM_TIMESTAMP_FREQUENCY follows CNTFRQ_EL0 on JM (task D2 passes it).
4. The reset hard-stops directly. Linux soft-stops, waits 10 ms and handles the remaining
   interrupts. Jobs that would have completed in that window are failed (and resubmitted by
   the caller) instead of being reported done.
5. Unknown Bifrost product ids are accepted with no optional features, where Linux refuses
   them. The G71 erratum TMIX_8463 (wait for L2 transitions before reset) is not handled; G71
   is not a target.
6. Mock leniencies:
   - A stalled job retries only once its VA translates (real hardware would fault again).
   - Dependencies are checked in chain order only.
   - XAFFINITY and THROTTLE are plain registers.
   - JS_STATE is live rather than sampled at JOB_INT_CLEAR.
7. Fixed in this task: `jm_unhandled_fault` (jm-arch's test, in my file) checked the AS right
   after the "Unhandled page fault" message, but the handler only disables the AS after that
   log line. Under load it failed about 1 run in 8. It now waits for the AS to go down.
8. Not tested: a race between `pw_jm_as_vm_get` and the final `pw_jm_vm_put` (that path is
   `pw_jm_vm_tryget`, owned by jm-mem), and the AS LRU table running full (32 entries; logged
   as EBUSY).
9. No perf counters (PERFCNT_* stay ENOSYS), and no DVFS.

## Contract changes

All are extensions or documentation fixes. CSF behaviour is unchanged: the CSF suites pass
unmodified.

1. `pw_jm_hw.h` `pw_jm_slot_submit` comment: **fix**. It said CONFIG includes `START_MMU`.
   Linux sets that bit only for HW_ISSUE_10649 (Midgard T60x), never on Bifrost, so we do not
   set it. Also documented: the store barrier before START, and that the call is a logged
   no-op for a bad slot, a bad AS or a dead device.
2. `pw_jm_hw.h` `struct pw_jm_irq_event`: documentation only, the layout is unchanged.
   `done` and `failed` can both be set for a slot; NEXT is already cancelled for failed slots;
   `js_state` is the raw JS_STATE of the slots that had events.
3. `pw_jm_hw.h` `pw_jm_hw_reset`: documented that it waits for running job and MMU bottom
   halves (synchronize_irq), delivers no events for killed jobs, and restarts a used cycle
   counter.
4. `pw_jm_hw.h` new: `pw_jm_as_get_vm()`, the LRU over a bare `pw_vm` with an opaque key, for
   kernel-built jobs (Z2 self test) and host tests. Also new: `pw_jm_irq_hook_t` and
   `pw_jm_hw_set_irq_hook()`, which route job events away from the scheduler for directly
   submitted jobs, and `pw_jm_slot_affinity()`.
5. `pw_jm_hw_init` can also return `PW_EOPNOTSUPP` (JS0 or JS1 not in JS_PRESENT).
6. `pw_regs_jm.h`: SHADER/TILER/JM_CONFIG quirk bits, `JOB_JS_STATE_SLOT_MASK`, and the
   documented JS_STATE layout.
7. Mock (`mock_gpu.h`, G31 only):
   - New `mock_config.no_shader_ready` and `no_tiler_ready` (zero default).
   - `struct mock_js_stats` extended. The old fields keep their meaning; `jobs` now counts
     completed descriptors.
   - New `mock_jm_inject`/`mock_jm_kick`, `mock_jm_job_log`/`_clear`, and the
     `enum mock_jm_inject` and `struct mock_jm_job_rec` types.
8. Mock internals (`mock_internal.h`): `mock_rawstat_locked`, `mock_as_enabled_locked`,
   `mock_gpu_translate_locked`, `mock_gpu_timestamp_locked`, `mock_gpu_cycles_locked`,
   `mock_kick_locked`, `mock_cond_wait_locked`, `mock_jm_start/stop`,
   `mock_jm_int_clear_locked`, `mock_jm_mmu_clear_locked`, `mock_jm_as_command_locked`.
9. `pw_os_mock.c`, G31-gated (`is_jm`) except where noted:
   - The executor thread is started in `mock_create` and stopped first in `mock_destroy`
     (a no-op on G610).
   - JOB_INT and MMU_INT clear hooks, and an AS-command hook.
   - A soft reset also clears JOB/MMU RAWSTAT and stops the cycle counter.
   - GPU_CYCLE_COUNT runs at a nominal 400 MHz while started, and GPU_TIMESTAMP at 24 MHz.
   - CLEAN(_INV)_CACHES advances LATEST_FLUSH_ID.
   - The power-up knobs.
10. `test_jm_hw.c`: the two jm-arch placeholders (`jm_get_param`, `jm_power_and_slots`) were
    replaced by the tests above, and the `jm_unhandled_fault` race was fixed.

## Review

Independent review, 2026-10-30. **Untested on hardware**, like the rest of this file.

### What I re-ran
I ran everything in private copies under `/tmp/claude-1000/rev-jmhw*`, each with `make clean` first:
- Before any fix:
  - gcc `make`: 130 tests, 0 failed, 3 skips.
  - `make CC=clang`: the same.
  - `make freestanding`: every core file compiles, and the only undefined symbols are `pw_*`.
- After the fixes:
  - gcc `make`: **133 tests, 0 failed, 3 skips**.
  - `make CC=clang`: the same.
  - `make freestanding`: OK.
  - `make -C kmd/win/tests`: 10 tests OK. jm-glue has added one test since the report.
  - Stress, `./pw_tests jm_`: 6 parallel copies × 2 rounds, all passing.
- Not re-run: the meson `-Dcore=real` build and the libdrm-win build.
- License: an 8-word n-gram scan against Linux panfrost (*.c/*.h) found only the probe log
  format strings ("mali-%s id 0x%x major …", "Features: L2:…"). These match on purpose, so the
  boot logs can be compared. No copied code or comments.

### Claims
They held up. Every claimed test exists and passes. The Linux references check out:
- the G31 quirk set and model table;
- the JS register offsets and CONFIG bits, including START_MMU for HW_ISSUE_10649 only;
- the GET_PARAM coverage of every param `panfrost_kmod.c` reads;
- the IRQ clear and re-read loop;
- the v7.xml Job Header and WRITE_VALUE layout in the mock.

The `timer_hz` note is also right: `pw_device_alloc` never stores it.

### Bugs found and fixed (all in `pw_jm_hw.c/.h`)
1. **A late job bottom half ran in the middle of a reset.** `pw_irq_suspend()` only masks the
   line, and `pw_irq_bottom()` does not check SUSPENDED. A work item that the ISR queued before
   the reset, but that starts after `jm_sync_bottom_half`, therefore processed JOB_INT during
   the reset. It handed the hard stop's TERMINATED to the scheduler and cleared JOB_INT under
   the reset.
   - Fix: a `quiesced` flag, set by reset and fini before they sync. The handler checks it
     under `bh_lock`. The reset clears stale JOB_INT bits before it unquiesces.
   - Test: `jm_reset_late_bottom_half`. Without the fix it failed in about 1 run in 4; with the
     fix it passed 20 of 20 runs.
2. **Completed chains were lost by a reset.** A DONE pending at reset time was wiped, so the
   scheduler would resubmit (re-execute) a finished chain. Linux handles the remaining
   interrupts first.
   - Fix: new `pw_jm_hw_reset_collect(dev, ev)`. It gathers the pending events after the stop,
     using the same code as the IRQ pass, without delivering them. `pw_jm_hw_reset` is
     `_collect(dev, NULL)`.
   - Test: `jm_reset_collect`.
3. **A tiler power-up timeout left the shader cores on.** `powered` was only set after the
   tiler came up.
   - Fix: it is now set once the shaders are up.
   - Test: `jm_init_failures` now checks SHADER_READY/TILER_READY == 0. It failed before the fix.

### Contract changes (review)
- New `pw_jm_hw_reset_collect()`.
- Documentation in `struct pw_jm_irq_event`:
  - one done bit can stand for two completed chains (the js_state rule);
  - no events are delivered while the line is quiesced.

### Remaining concerns (ranked)
1. **Lead: the same late-bottom-half race on the MMU line.** `pw_mmu_irq_handler` and the
   heap growth in `pw_jm_mmu_fault` can run during a JM reset (and on CSF). The clean fix is
   in `pw_irq_bottom`: stop calling the handler while the line is SUSPENDED.
2. **jm-sched: one case is ambiguous after a reset.** A slot that had a job in NEXT and shows
   only `done` either completed both jobs or never ran the second; the registers cannot tell
   which. This is documented in the header.
3. **Quirk order differs from Linux.** The quirks are written after the L2 is powered; Linux
   writes them before L2 power-up. This is probably harmless, but check it at Z2.
4. **Open items carried over from above:**
   - hard stop instead of soft stop in reset (TODO 4);
   - the reset unbinds AS slots without each VM's op_lock (TODO 2);
   - the guessed G31 register values.
