# jm-arch: job-manager backend architecture (Mali-G31, panfrost uAPI)

Owner key: `jm-arch`. Scope: the contracts and scaffolding that let the job-manager (JM) backend of
`kmd/core` be built in parallel by three engineers, plus a small Windows glue / libdrm-win task.
Decision D4 in [`PLAN.md`](../PLAN.md); hardware facts in
[`research/zero3-g31.md`](../research/zero3-g31.md). Date: 2026-10-30.

**Nothing here touches hardware yet.** The JM modules are stubs; the shared changes are tested
against the mock only.

## Design in one page

```
                       pw_file_ioctl(nr)
                             │
          ┌──────────────────┴──────────────────┐
   nr 0x00-0x3f, 0xa0+                   nr 0x40-0x9f (driver)
   DRM core table (shared)               table of dev->backend
   VERSION  "panthor"|"panfrost"         ┌───────────────┴───────────────┐
   GET_CAP  (TIMELINE: CSF only)         panthor (CSF)                   panfrost (JM)
   GEM_CLOSE, SYNCOBJ_* (timeline        pw_bo.c / pw_sched.c / ...      pw_jm_mem.c  BO ioctls
   ones EOPNOTSUPP on JM)                (unchanged)                     pw_jm_sched.c SUBMIT, JM_CTX_*
                                                                         pw_jm_hw.c   GET_PARAM

 probe: pw_gpu_init (GPU_ID -> backend) -> soft reset -> L2 on ->
        CSF: [boot_firmware] MMU -> FW -> sched                       (as before)
        JM:  MMU -> pw_jm_hw_init -> pw_jm_mem_init -> pw_jm_sched_init (no firmware)
```

- **Backend selection** (`pw_core.h`, `enum pw_backend`): `pw_gpu_init()` reads GPU_ID and picks
  `PW_BACKEND_JM` for arch major 6/7 (Bifrost), `PW_BACKEND_CSF` for 10-13, otherwise fails with
  `PW_EOPNOTSUPP` and the old "unsupported GPU arch N" message (Midgard, Valhall-JM arch 9 and
  arch 14+ stay out of scope). `pw_device_backend()` exposes it; `pw_backend_for_gpu_id()` is the
  pure rule. Firmware is only ever loaded on CSF; `boot_firmware` is ignored on JM.
- **Shared code made backend-aware** (done here, small): GPU_CMD encodings (JM soft reset `0x01`,
  cache flush `CLEAN_CACHES 0x07` / `CLEAN_INV_CACHES 0x08` instead of CSF `FLUSH_CACHES`), the GPU
  IRQ mask (no PROTM_FAULT, MULTIPLE_FAULT instead), CSF-only GPU_INFO fields left 0 on JM
  (`0x1C` is JS_PRESENT, `0x60` the perf-counter base), Bifrost model names (from Mesa
  `pan_model.c`, MIT), `pw_device_describe()`.
- **Dispatch** (`pw_file.c`): one DRM core table and one driver table per backend, the generic
  struct-size handling unchanged, each driver table gated on its own emulated version. JM reports
  **panfrost 1.5** (`PW_JM_DRM_MINOR`, `pw_file.h`); SYNC_BO and QUERY_BO_INFO (1.6) are in the
  table but return ENOTTY until the minor is raised, for the same reason panthor is held at 1.6
  (1.6 makes Mesa use WB_MMAP cached mappings with user-mode cache maintenance, unverified on
  Windows ARM64). PERFCNT_* return ENOSYS, as Linux does without `unstable_ioctls`. GET_CAP reports
  `SYNCOBJ_TIMELINE = 0` and the four timeline ioctls fail with EOPNOTSUPP on JM, because panfrost
  is `DRIVER_SYNCOBJ` without `DRIVER_SYNCOBJ_TIMELINE`; Mesa then emulates timelines as it does on
  Linux. There is no user-MMIO page on JM (`pw_file_mmap_lookup` rejects offsets ≥ 1 << 43).

### Reuse decisions

| Piece | Decision |
|---|---|
| GEM handles | **Reused**: panfrost BOs live in the shared `file->bos` table, so GEM_CLOSE, `pw_file_bo_get()` and the handle rules are unchanged. `file->vms` stays empty on JM. |
| `struct pw_bo` | **Reused** with two new fields: `ext` (JM state, `struct pw_jm_bo`) and `release_ext` (runs once from the final `pw_bo_put()`, before the memory is freed). `bo->flags` keeps its DRM_PANTHOR_BO_* meaning on both backends (HEAP BOs set NO_MMAP, WB_MMAP BOs set WB_MMAP), so `pw_file_mmap_lookup()` and the mmap offsets work unchanged. `bo->mem` may be NULL for unbacked (HEAP) BOs. |
| BO creation, mmap offsets, labels | **Reused** via new exported pieces of `pw_bo.c`: `pw_bo_alloc()` (with `backed = false` for HEAP), `pw_bo_get_mmap_offset()`, `pw_bo_set_label_from_user()`. The panthor ioctls now call the same helpers (no behaviour change). |
| GPU VA lifetime | A BO's VA lives as long as the BO (released by `release_ext`). Linux unmaps at GEM_CLOSE unless a job still holds the mapping; with no PRIME on Windows a BO has one file, and jobs hold BO references, so this is equivalent. User CPU mappings also keep the VA (harmless). |
| Address space | One `struct pw_vm` per file (the existing AArch64 page tables), wrapped by `pw_jm_mem.c` in a refcounted `struct pw_jm_vm` with a VA allocator for `[32 MiB, 4 GiB)` and panfrost's alignment rules (`pw_jm_mem.h`). `pw_uvm`/VM_BIND are not used. |
| AS slots | New primitives `pw_mmu_as_bind_locked()` / `pw_mmu_as_unbind_locked()` in `pw_mmu.c` (implemented and tested here); the LRU policy over them is `pw_jm_as_get/put` in `pw_jm_hw.c`. `pw_vm_activate()` stays the CSF policy (AS0 = MCU, no eviction). |
| Fault handling | `pw_mmu_irq_handler()` calls `pw_jm_mmu_fault()` first on JM (without `slots_lock`, since growing a heap maps pages); `PW_OK` clears the fault and keeps the AS, anything else takes the existing unhandled path (log, disable AS). |
| Implicit sync | One fence per BO (the last job's), because panfrost treats every BO of a job as written, so jobs on a BO are totally ordered. A device-wide `pw_jm_resv_lock` makes "collect dependencies + publish the fence" atomic across a job's BO list. WAIT_BO waits on that fence. |
| Fences / syncobjs | **Reused** unchanged (`pw_fence.h`, `pw_sync.h`), binary only on JM. |
| CSF scheduler | **Not reused**: JM gets its own small per-slot queue (`pw_jm_sched.c`). |
| IRQ plumbing | **Reused** (`pw_irq.c`); the JM job line handler is `pw_jm_job_irq_handler`, registered by `pw_jm_hw_init`. |

### Module ownership

Recorded at the top of `kmd/core/pw_file.h`.

| Owner | Files | Implements |
|---|---|---|
| **A: jm-hw** | `pw_jm_hw.c/.h`, `pw_regs_jm.h`, `kmd/tests/mock_jm.c`, `test_jm_hw.c`, the G31 parts of `pw_os_mock.c` / `mock_gpu.h` | JM power-up, GET_PARAM, job-slot registers, job IRQ, soft reset/recovery, AS LRU, the mock job-chain executor |
| **B: jm-mem** | `pw_jm_mem.c/.h`, `test_jm_mem.c`, the JM branch of the lock/unlock-region code in `pw_mmu.c` | CREATE_BO, MMAP_BO, GET_BO_OFFSET, WAIT_BO, MADVISE, SET_LABEL_BO, SYNC_BO, QUERY_BO_INFO, per-file AS and VA allocator, HEAP growth, per-BO fences |
| **C: jm-sched** | `pw_jm_sched.c/.h`, `test_jm_sched.c` | SUBMIT, JM_CTX_CREATE/DESTROY, slot queues, dependencies, completion, timeouts, reset policy |
| **D: glue** | `kmd/win/*`, `userspace/libdrm-win/*`, `tools/panwin-info` | G31 install and enumeration (list at the end) |
| jm-arch / lead | `pw_core.h`, `pw_priv.h`, `pw_device.c`, `pw_file.c/.h`, `pw_gpu.c`, `pw_bo.c/.h`, `pw_mmu.h`, `pw_os.h`, `mock_internal.h`, `test_jm.h`, `test_main.c`, `Makefile`, `meson.build` | Contracts and shared code. Engineers **append** here (a new helper, a new mock hook) and note the change in their status doc; they do not change existing meanings. |

Rules for all three engineers: Linux panfrost is GPL-2.0-only, so read it for behaviour and register
meanings only and never copy code. Cite `file:function`. Code is C11 and freestanding (`pw_util.h`
helpers), kernel-style tabs, SPDX MIT. Each engineer writes `docs/status/jm-{hw,mem,sched}.md` in the
usual format.

## Contract changes (all extensions; CSF behaviour unchanged)

1. `pw_core.h`: `enum pw_backend`, `pw_backend_for_gpu_id()`, `pw_device_backend()`,
   `pw_device_drm_version()`. `pw_device_params.boot_firmware` is now documented as CSF-only.
   `pw_core_drm_version()` keeps meaning "the panthor version". The OS layer should switch to
   `pw_device_drm_version()` (task D).
2. `pw_gpu_init()` accepts arch 6/7 GPU_IDs, which used to fail with EOPNOTSUPP. This is the
   intended change; the existing "arch 9" and "arch 14" test cases still fail the same way.
3. `pw_file_ioctl()`: driver ioctl numbers are looked up in the device's backend table. On CSF the
   result is identical to before (same table contents, same gating).
4. `pw_file_mmap_lookup()`: on JM, offsets in the user-MMIO window are EINVAL.
5. `struct pw_bo`: new `ext` / `release_ext`; `mem` may be NULL (JM unbacked BOs only).
   `pw_bo_put()` calls `release_ext` and skips freeing a NULL `mem`. New `pw_bo_alloc()`,
   `pw_bo_get_mmap_offset()`, `pw_bo_set_label_from_user()`. The panthor BO_MMAP_OFFSET and
   BO_SET_LABEL handlers now call them, with byte-identical results (covered by `test_mem.c`).
6. `pw_mmu.h`: `pw_mmu_as_bind_locked()` / `pw_mmu_as_unbind_locked()`. `pw_mmu_irq_handler()`
   calls `pw_jm_mmu_fault()` first on JM only.
7. `pw_gpu_flush_caches()` / `pw_gpu_soft_reset()` emit JM encodings on JM, and
   `pw_gpu_model_name()` knows the Bifrost parts.
8. `struct pw_device`: `backend`, `jm`, `jm_mem`, `jm_sched`. `struct pw_file`: `jm_mem`,
   `jm_sched`. `pw_os.h`: `PW_ENODEV` (-19, panfrost SUBMIT's unknown `out_sync`).
9. Mock: `mock_config.model` (G610 default, so zero-initialised configs are unchanged) and
   `mock_config.as_present` (0 = model default). The G31 model has its own register values,
   JM GPU_CMD codes, a 256 KiB register window, shader/tiler/stack power, and JOB_CONTROL
   routing to `mock_jm.c`. New internal hooks are in `mock_internal.h`.
10. Build: `kmd/core/meson.build` adds the three `pw_jm_*.c` files. The tests Makefile, the
   kmd/win glue tests and `-Dcore=real` pick these up from it. `userspace/libdrm-win/meson.build`
   has its own core list, which now includes the JM files and `mock_jm.c`. The freestanding
   symbol filter ignores `pw_jm_*`. `include/drm-uapi/panfrost_drm.h` is Mesa's copy (MIT),
   since that is what Mesa compiles against; it is older than Linux's 1.7 header, where
   PERFCNT_DUMP gained a `state` field.

## Values to confirm on the board (mock G31, `kmd/tests/mock_gpu.h`)

| Register | Mock value | Confidence |
|---|---|---|
| GPU_ID | `0x70930000` (G31 r0p0) | Product (7.0.x, product 3) from Mesa. `0x7093`/r0p0 is recalled from public panfrost boot logs |
| L2 / CORE / TILER / MEM features | `0x07110206` / `0x2` / `0x209` / `0x1` | recalled from boot logs, guess |
| AS_PRESENT / JS_PRESENT | `0xff` / `0x7` | recalled, guess (8 AS shared by all files: LRU needed beyond 8 files) |
| SHADER / L2 / TILER / STACK present | `0x3` / `0x1` / `0x1` / `0x1` | MP2 = 2 cores; the stack value is a guess |
| MMU_FEATURES | `0x2830` (48 VA / 40 PA bits) | guess. The core needs VA bits ≥ 33, so `0x2823` would also work |
| THREAD_* / TLS_ALLOC | 512 threads, `0x02004000`, 512 | guess (Mesa: "G31 is 512") |
| JS_FEATURES 0-2 | `0x20e` / `0x1fe` / `0x7e` | the usual layout, guess |
| TEXTURE_FEATURES0, COHERENCY, AFBC | G610 dump, `0x1`, `0` | guess |

Z1's `panwin-info` dump (or one Linux panfrost boot on the board) replaces all of these. Each
engineer must keep their tests independent of the guessed values (read them from `mock_gpu.h`).

## Open questions (for a human / the lead)

1. **Mesa on Windows has no JM kmod.** *Resolved 2026-10-30.* The owner made the Mesa tree a hard
   fork (tarball + `userspace/mesa/patches/panwin-mesa.diff`, see `userspace/mesa/AI-POLICY.md`).
   The fork builds `panfrost_kmod.c` on Windows: mmap goes through libdrm-win, perf counters fail
   with ENOTSUP. panvk JM copies event fences with a binary SYNCOBJ_TRANSFER, which the core
   accepts on JM for that purpose. `tests/e2e/run.sh g31` runs panvk v7 on the mock G31.
2. **Page-table format.** Linux panfrost uses the legacy "Mali LPAE" format on the H616: its
   compatible has no `GPU_QUIRK_FORCE_AARCH64_PGTABLE`. Our core only writes AArch64 4K tables.
   The G31 advertises `HW_FEATURE_AARCH64_MMU` (panfrost_features.h), which is what Arm's kbase
   uses on Bifrost, so the plan is AArch64, reusing `pw_mmu.c` as it is. Confirm on hardware at
   Z2. If it fails, the fallback is a second descriptor format in `pw_mmu.c`, with TRANSCFG
   `ADRMODE_LEGACY`.
3. **TLB maintenance on JM.** The shared map/unmap path does LOCK → write PTEs → GPU
   `CLEAN_INV_CACHES` → UNLOCK, which is panthor's sequence. panfrost instead uses
   `AS_COMMAND_FLUSH_PT` / `FLUSH_MEM` on the locked region. Whether UNLOCK invalidates the TLB on
   Bifrost v7 is not confirmed, so task B5 switches the JM branch to panfrost's sequence.
4. **Version 1.5 vs 1.6.** Reporting 1.6 enables WB_MMAP and SYNC_BO in Mesa. Raise it together
   with panthor's 1.7, once user-mode cache maintenance is verified on Windows ARM64.
5. **`boot_firmware` on JM** is ignored: a JM device always does its full init. The KMDF default
   is `BootFirmware = 0` (CSF bring-up mode), which would otherwise leave a G31 without job
   slots. If a Z1 "GPU only" mode is wanted, add a JM-specific parameter instead of reusing it.
6. **MADVISE** never purges: there is no shrinker on Windows yet, so `retained` is always 1.
   Decide whether memory pressure on a 1.5 GB board needs one.
7. **HIGH priority contexts** are refused with EACCES. Linux needs CAP_SYS_NICE or DRM master,
   and the same choice was made for panthor.
8. **JS2 (compute-only slot)** stays unused, as in panfrost.

## Task lists

Tasks within one list run in order. Lists A, B and C can start at once, because they only meet
through the headers. Integration order:
A1-A3 + B1-B4 → C1-C3 (validation, contexts, dependency logic against the stubs) → A6 (executor)
→ C4-C6 (job flow) → A7 + C7 (reset).

### A: jm-hw (`pw_jm_hw.c`, `pw_regs_jm.h`, `mock_jm.c`, `test_jm_hw.c`)

1. **`pw_jm_hw_init` / `fini` / `release`.**
   - `struct pw_jm_hw` (private) and `struct pw_jm_gpu_info`, read from the JM-only registers.
   - The G31 quirk rules (panfrost_gpu.c:panfrost_gpu_init_quirks for 0x7093 r0p0,
     `3BIT_EXT_RW_L2_MMU_CONFIG`).
   - Test: info against `mock_gpu.h`; fini twice; init failure paths (add mock knobs such as
     `no_shader_ready`).
2. **Shader + tiler power-up** (panfrost_gpu.c:panfrost_gpu_power_on).
   - Poll PWRTRANS/READY like `pw_gpu_l2_power_on`; the mock G31 already sets READY on PWRON.
   - Power off in fini.
   - Test: READY masks after probe; a timeout case.
3. **GET_PARAM**, every `DRM_PANFROST_PARAM_*` (contract in `pw_jm_hw.h`).
   - SYSTEM_TIMESTAMP holds the cycle counter (`pw_jm_cycle_counter_get/put`, GPU_CMD 5/6; the
     mock tracks `GPU_STATUS` bit 6).
   - Test: fill in `test_jm_get_param`: all params, pad, unknown param, JS_FEATURES past
     js_present.
4. **Job IRQ.**
   - `pw_irq_init(PW_IRQ_JOB, ...)` with done|err of JS0-2; `pw_jm_job_irq_handler` builds a
     `struct pw_jm_irq_event` and calls `pw_jm_sched_job_irq()`. Read the status before clearing.
   - Test: with a test-local stub scheduler, or after C lands, check the event contents for
     done/failed bits.
5. **Slot helpers**: `pw_jm_slot_*`, `pw_jm_latest_flush_id`, `pw_jm_exception_name`.
   - JS_CONFIG as in the header: START_MMU, flush bits, THREAD_PRI(8), flush reduction,
     JOB_CHAIN_FLAG.
   - Test: register writes seen by the mock (`mock_jm_js_stats`, `mock_reg`).
6. **Mock executor** (`mock_jm.c`).
   - Model per the file header: START latching, a job-chain walk through the AS, WRITE_VALUE and
     NULL jobs, the other types completed without shaders, JOB_INT DONE/ERR, JS_STATUS/HEAD/TAIL/
     JS_STATE, SOFT/HARD_STOP, GPU faults raised as MMU faults.
   - Descriptor layout from Mesa `src/panfrost/genxml/v7.xml` (MIT).
   - Add injection knobs: fail with status X, never complete (for timeouts), stall until kicked.
   - Test: chains of 1 and 3 jobs, a faulting chain, a stop.
7. **`pw_jm_hw_reset`.**
   - Hard-stop, soft reset, re-power, forget AS bindings, re-arm.
   - Test: a reset while a job is "running" in the mock; IRQ masks restored; AS state cleared.
8. **AS LRU**: `pw_jm_as_get/put/release/vm_get` over `pw_mmu_as_bind/unbind_locked`.
   - Test with `mock_config.as_present = 0x3`: reuse, eviction of idle slots only, EBUSY when all
     are busy, re-enable after a fault, and `as_vm_get` racing a final put (`pw_jm_vm_tryget`).
     Use a test-local `pw_jm_vm` double until B2 lands.

### B: jm-mem (`pw_jm_mem.c`, `test_jm_mem.c`, JM branch of `pw_mmu.c` lock/unlock)

1. **Per-file VM.** `struct pw_jm_vm` (refcount, `pw_vm`, VA allocator, lock), `file->jm_mem`.
   - `pw_jm_file_vm`, `pw_jm_vm_get/tryget/put/mmu`; the final put calls `pw_jm_as_release`
     before `pw_vm_destroy`.
   - Test: file open/close, no leaks.
2. **VA allocator** with panfrost's rules (`pw_jm_mem.h`): `[32 MiB, 4 GiB)`; executable BOs
   aligned to their size, never on a 4 GiB edge; NOEXEC ≥ 2 MiB → 2 MiB aligned; ENOSPC when full.
   - Test: unit cases plus exhaustion.
3. **CREATE_BO.**
   - `pw_bo_alloc` (HEAP: unbacked, 2 MiB rounded), `bo->flags` mapping, `ext`/`release_ext`.
   - Map with `pw_vm_map_mem` (NOEXEC → `PW_VM_MAP_NOEXEC`), then `pw_handle_alloc`.
   - Test: errnos, the GPU view (`mock_gpu_translate`, XN on NOEXEC), zeroed memory,
     GEM_CLOSE → VA reusable.
4. **MMAP_BO, GET_BO_OFFSET, SET_LABEL_BO, MADVISE, QUERY_BO_INFO, SYNC_BO** (errnos in the
   header).
   - Test each. For the 1.6 ioctls, call the handlers directly while `PW_JM_DRM_MINOR` is 5.
5. **TLB maintenance on JM** (open question 3).
   - In `pw_mmu.c` `vm_lock_region`/`vm_unlock_region`, add a JM branch: FLUSH_PT after a map,
     FLUSH_MEM after an unmap, on the locked region (panfrost_mmu.c:mmu_hw_do_operation_locked).
   - Teach the mock to accept it (`as_command_locked` currently flags UNLOCK without a GPU flush).
     This edit is shared with A in `pw_os_mock.c`, so coordinate.
   - Test: the mock sees the right AS commands on a bound VM; CSF tests unchanged.
6. **Implicit sync**: `pw_jm_resv_lock/unlock`, `pw_jm_bo_fence_get/set` (per-BO fence under a
   small lock).
   - **WAIT_BO** with absolute deadlines: OK / EBUSY / ETIMEDOUT / EINTR.
   - Test: create fences by hand (`pw_fence_create`), signal them from a thread.
7. **`pw_jm_bos_lookup/put`** for SUBMIT.
   - Test: EFAULT, ENOENT, duplicates allowed (Linux allows them), refs released on error.
8. **HEAP growth** (`pw_jm_mmu_fault`): translation faults only; 2 MiB chunk; idempotent; chunk
   memory freed in `release_ext`.
   - Test: inject faults with `mock_inject_mmu_fault` on a bound VM; check the mapping, the AS
     still enabled, no "Unhandled" log; non-heap / outside / ENOMEM → unhandled.

### C: jm-sched (`pw_jm_sched.c`, `test_jm_sched.c`)

1. **Contexts.** Per-file context table, default ctx 0 MEDIUM, JM_CTX_CREATE/DESTROY with the
   header's errnos.
   - Test: fill in `test_jm_ctx`.
2. **SUBMIT validation and setup** in the header's order: syncobjs, BOs, context, slot,
   flush id, then the job object with its references.
   - Test: every errno, and no side effect on failure (out_sync untouched, BO fences untouched).
3. **Dependencies.**
   - in_syncs fences plus the BO fences, collected and published under `pw_jm_resv_lock`; wait
     with `pw_fence_add_callback`, never block in the ioctl.
   - Test: against the stub hardware a job never runs, so test ordering and cancellation (file
     close with pending dependencies).
4. **Slot queues** (running + NEXT per slot), `pw_jm_as_get` when a job becomes ready
   (EBUSY: retry when an AS is released), submit through the `pw_jm_slot_*` helpers.
5. **Completion** in `pw_jm_sched_job_irq`.
   - Fence error 0 / -EINVAL / -ECANCELED, STOPPED → resume from the tail, `pw_jm_as_put`.
   - Release references from a work item.
   - Test (needs A6, B3): a WRITE_VALUE job writes its value and signals out_sync; FS → JS0;
     two jobs pipelined on one slot.
6. **Context destroy / file close with jobs in flight** (cancel NEXT, hard-stop the running job).
   - Test: no leaks; the fences end with -ECANCELED.
7. **Timeouts and reset.**
   - Per-job timer (`PW_JM_JOB_TIMEOUT_MS`, `pw_jm_sched_set_job_timeout` for tests).
   - On expiry: stop feeding, `pw_jm_hw_reset` from a work item, fail the guilty job, resubmit
     the innocent ones.
   - Test with the mock's "never complete" knob (A6).
8. **Stress**: 1000 submits over 3 files with shared BOs and more files than AS slots
   (`as_present = 0x3`); zero leaks; no model errors.

### D: Windows glue and libdrm-win for the G31 (small)

1. **INF** (`kmd/win/panwin.inf`).
   - Add `%DeviceDesc% = Panwin_Device_Zero3, ACPI\AWGP0031` and `ACPI\VEN_AWGP&DEV_0031`.
   - Give the G31 install section no `mali_csffw.bin` CopyFiles, and `Rk3588PmuCheck = 0`.
   - The PMU check defaults to 1 and maps the RK3588 PMU at `0xFD8D8000`, which is not the PMU
     on the H618 and must never be touched there. Better still, key the check on the ACPI HID.
   - The `NTarm64.10.0...19041` decoration covers the Windows 10 22H2 / 11 ≤ 23H2 builds the
     Zero3 runs.
2. **`pw_win_driver.c`.**
   - The MMIO window check accepts 0x40000 (≥ 0x10000) but warns "RK3588 has 0x200000". Make
     the message per device.
   - IRQ order job/mmu/gpu matches the GPU0 node (GSIV 127/128/129).
   - Pass `timer_hz` from CNTFRQ_EL0 (24 MHz on the H616).
   - `BootFirmware` and `FirmwareName` are ignored on JM; say so in the log.
3. **GET_INFO and logs**: use `pw_device_drm_version()` instead of `pw_core_drm_version()`
   (`pw_win_driver.c:535`, `pw_win_ioctl.c:549`).
   - Consider an additive `PANWIN_INFO_JM` flag, since `PANWIN_INFO_FW_BOOTED` means nothing on JM.
   - Fix the "emulated panthor version" comments in `panwin_ioctl.h`.
4. **The user-MMIO mmap path** (`pw_win_ioctl.c`) already relies on `pw_file_mmap_lookup`. Check
   that the JM EINVAL surfaces as EINVAL.
5. **libdrm-win `xf86drm_win.c`.**
   - `drmOpenWithType()` only accepts "panthor": accept the device's own VERSION name.
   - `drmGetDevices2()` reports RK3588 platform info (`/gpu@fb000000`,
     `rockchip,rk3588-mali` / `arm,mali-valhall-csf`). Derive it from the driver name: for
     panfrost, `/gpu@1800000` with `allwinner,sun50i-h616-mali` / `arm,mali-bifrost`.
6. **libdrm-win `transport_core.c`** (the e2e in-process core).
   - Add a model switch, e.g. `PANWIN_CORE_MODEL=g31` → `mock_config.model`, with no firmware
     needed.
   - The `nr == 0x40 + DRM_PANTHOR_GROUP_DESTROY` special case also matches panfrost
     `JM_CTX_CREATE` (0x4a): gate it on the backend.
   - The LATEST_FLUSH_ID mmap special case is panthor-only.
7. **`tools/panwin-info`**: on "panfrost", print every GET_PARAM value instead of DEV_QUERY.
   This is the Z1 dump that confirms the mock's guessed values.

## Verified (run here, 2026-10-30)

- `make -C kmd/tests`, gcc with ASan/UBSan and `-Werror`: 110 tests, 0 failed, 8 skips.
  - 96 tests existed before; 14 JM tests are new (7 jm_hw, 4 jm_mem, 3 jm_sched).
  - The 8 skips are the stub placeholders.
- `make -C kmd/tests clean && make -C kmd/tests CC=clang`: 110 tests, 0 failed, 8 skips.
- `make -C kmd/tests freestanding` (llvm-mingw aarch64, `-ffreestanding`): every core file compiles.
- `meson setup /tmp/claude-1000/pw-jm-kmd kmd --cross-file toolchain/meson/aarch64-w64-kmd.ini
  -Dcore=real && ninja && meson test`: panwin.sys links; 2/2 tests OK.
- `make -C kmd/win/tests` (glue on the real core): 9 tests OK.
- The new tests that pass:
  - `jm_backend_select`, `jm_probe_g31`: G31 probe, JM soft reset and flush encodings,
    GPU_INFO, describe.
  - `jm_csf_version_unchanged`.
  - `jm_dispatch`: VERSION "panfrost" 1.5, GET_CAP, timeline EOPNOTSUPP, PERFCNT ENOSYS,
    1.6 gating, no MMIO mmap.
  - `jm_unhandled_fault`: the JM hook → unhandled path, AS0 bind/unbind/re-enable.
  - `jm_bo_ext_hooks`: `pw_bo_alloc` with and without backing, `release_ext`, shared mmap
    offsets and GEM_CLOSE on a JM file.
- Not built here: libdrm-win with `-Dcore_transport=true` (its source list was updated).
