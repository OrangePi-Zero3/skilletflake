# jm-mem: job-manager memory (Mali-G31, panfrost uAPI)

Owner key: `jm-mem`. Task list B in [`jm-arch.md`](jm-arch.md). Date: 2026-10-30.
Everything here is **untested on hardware**. It has only run against the mock G31.

## Summary

- **`kmd/core/pw_jm_mem.c`** (tasks B1-B4 and B6-B8):
  - **Per-file address space.** `pw_jm_vm` is refcounted and wraps a `pw_vm`. Its VA allocator is
    the file's BOs in a list sorted by VA, over `[32 MiB, 4 GiB)`. The allocator follows
    panfrost's rules:
    - executable BOs are aligned to their own size (which need not be a power of two);
    - the `panfrost_drm_mm_color_adjust` 4 GiB / 16 MiB hole rule applies to executable BOs;
    - NOEXEC BOs of 2 MiB or more are 2 MiB aligned;
    - a full range returns ENOSPC.
  - **Ioctls.** All panfrost BO ioctls with Linux's errnos: CREATE_BO (flags NOEXEC and HEAP, plus
    WB_MMAP once the version is ≥ 1.6), MMAP_BO, GET_BO_OFFSET, WAIT_BO, MADVISE, SET_LABEL_BO,
    SYNC_BO and QUERY_BO_INFO. The last two are complete, but the dispatcher returns ENOTTY for
    them at our 1.5.
  - **CPU mappings** are write-combined unless WB_MMAP is set. They go through the shared fake
    mmap offsets and `pw_file_mmap_lookup`.
  - **GPU VA lifetime.** A BO's GPU VA lives as long as its last reference (`release_ext`). A
    GEM_CLOSE while a job holds the BO leaves the mapping in place.
  - **WAIT_BO** treats `timeout_ns` as an absolute time on `pw_os_time_ns`, as panfrost does
    through `drm_timeout_abs_to_jiffies`. Mesa passes 0 or INT64_MAX. Results:
    - 0, negative or past deadline: poll, returning EBUSY if the BO is busy;
    - expired wait: ETIMEDOUT;
    - interrupted wait: EINTR;
    - idle BO: OK, even if its job failed.
  - **Implicit sync.** One fence per BO, under `pw_jm_resv_lock` and a leaf fence lock, through
    `pw_jm_bo_fence_get/set`.
  - **`pw_jm_bos_lookup/put`** for SUBMIT.
  - **HEAP growth** happens from the MMU bottom half (`pw_jm_mmu_fault`). It only handles
    translation faults (0xc0-0xc7) in a HEAP BO, and maps the zeroed 2 MiB chunk containing the
    faulting address as RW NOEXEC cacheable. It is idempotent. Anything else, including ENOMEM, is
    reported as an unhandled fault.
- **`kmd/core/pw_mmu.c`, JM branch:**
  - **B5, TLB maintenance on JM:** LOCK, then update, then FLUSH_PT after a map, or FLUSH_MEM after
    an unmap or a rolled-back map. There is no GPU_CMD flush and no UNLOCK. CSF is unchanged.
  - **Legacy Mali LPAE page tables**, selectable with `pw_device_set_pt_format()` (JM only, default
    AArch64). The differences from AArch64:
    - leaf type is 0b01 at every level;
    - bits 6 and 7 are read and write permission;
    - there is no access flag, and shareability is always outer;
    - the walk always has 4 levels;
    - TRANSCFG is `ADRMODE_LEGACY`;
    - TRANSTAB is the root PA ORed with `TABLE | READ_INNER`;
    - MEMATTR is `0x00888D88`, byte for byte what Linux programs.

  Table walk, split and unmap code is shared between the two formats.
- **Mock** (`pw_os_mock.c`, `mock_gpu.h`, additions only):
  - an independent walker for the legacy format (G31 model only), so tests run in both formats;
  - FLUSH_PT and FLUSH_MEM end a LOCK on the G31 model, and are counted in `flush_pts` /
    `flush_mems`;
  - `mock_fail_mem_allocs()`, which makes the next allocations fail.
- **`kmd/tests/test_jm_mem.c`** has 14 tests (13 new; `jm_bo_ext_hooks` is jm-arch's). They bind
  through jm-hw's `pw_jm_as_get` and fall back to `pw_mmu_as_bind_locked` while it is a stub.
  Since jm-hw landed, they run on the LRU.

## How to build/test

```sh
# shared tree (never `make clean` there while others test)
PW_FW_PATH=/home/e3xp0/Projects/panfrost-win/references/firmware/mali_csffw.bin \
  make -C /home/e3xp0/Projects/panfrost-win/repo/kmd/tests
make -C /home/e3xp0/Projects/panfrost-win/repo/kmd/tests freestanding
# clang / isolated runs: private copy
mkdir -p /tmp/claude-1000/jm-mem && cd /tmp/claude-1000/jm-mem && rm -rf kmd include toolchain
cp -r /home/e3xp0/Projects/panfrost-win/repo/{kmd,include,toolchain} .
rm -rf kmd/tests/obj kmd/tests/pw_tests && make -C kmd/tests CC=clang
./kmd/tests/pw_tests jm_          # JM suites only (run from kmd/tests)
make -C kmd/win/tests             # glue on the real core
meson setup build-kmd kmd --cross-file toolchain/meson/aarch64-w64-kmd.ini -Dcore=real \
  && ninja -C build-kmd && meson test -C build-kmd
```

## Verified

All runs were on 2026-10-30, on the final code.

- **gcc (ASan+UBSan, `-Werror`), shared tree:** `make -C repo/kmd/tests` gave 130 tests, 0 failed,
  3 skips. The skips are jm-sched placeholders. All pre-existing CSF tests pass.
- **clang:** `make CC=clang` on a fresh private copy of the shared tree gave 130 tests, 0 failed,
  3 skips.
- **freestanding:** `make freestanding` in the shared tree compiles every core file. It leaves no
  undefined symbol outside `pw_*`, so no libc or compiler helper is pulled in.
- **Glue tests:** `make -C kmd/win/tests` on the private copy: 9 tests OK.
- **meson `-Dcore=real`:** `panwin.sys` links; `meson test` Ok: 4, Fail: 0.
- **jm-mem tests:** all 14 pass individually, and 5 repeated `pw_tests jm_` runs gave no failures
  (thread-timing check). What they cover:
  - `jm_file_vm`: one VM per file, refcounting and tryget, no leaks.
  - `jm_create_bo`: every CREATE_BO errno, size rounding, size not written back, heap and WB
    flags, VA reuse after GEM_CLOSE.
  - `jm_va_rules`:
    - alignment for exec sizes of 12K, 64K, 1M, 16M and 20K, and for NOEXEC ≥ 2M;
    - no 16 MiB crossing;
    - the 4 GiB / 16 MiB hole rule, in three layouts;
    - exhaustion with ENOSPC and recovery;
    - ENOMEM from the OS allocator.
  - `jm_gpu_view` (AArch64 and LPAE):
    - TRANSTAB, TRANSCFG and MEMATTR values;
    - physical address of every page;
    - PTE bits through both walkers (the CPU's and the mock's);
    - XN on NOEXEC BOs and zeroed memory;
    - write-combined user mapping seen by the GPU;
    - B5 command counts (LOCK+FLUSH_PT on map, LOCK+FLUSH_MEM on unmap, no UNLOCK, no GPU_CMD
      flush, nothing while unbound).
  - `jm_lpae_tables`: LPAE 2 MiB blocks, read-only permission fault, block split into level-3
    pages, uncached attributes.
  - `jm_pt_format_select`:
    - the format is fixed per VM at creation;
    - both formats work on two AS at the same time;
    - on CSF, the format is ignored before probe and LPAE returns EOPNOTSUPP after it.
  - `jm_bo_ioctls`: MMAP_BO, GET_BO_OFFSET, SET_LABEL_BO (E2BIG and truncation), MADVISE, and an
    mmap reference keeping BO and VA alive after GEM_CLOSE.
  - `jm_sync_query`: the 1.6 ioctls give ENOTTY through the dispatcher. Called directly:
    - every errno;
    - flush and invalidate data flow on a cached BO;
    - processing stops at the first failing op.
  - `jm_wait_bo`: poll, negative, past and future deadlines; signalled from another thread with
    INT64_MAX and with a finite deadline; fence replacement.
  - `jm_close_busy_bo`: GEM_CLOSE with a job reference and a pending fence. The VA stays mapped
    until the job puts its reference, then FLUSH_MEM runs and the VA is reused.
  - `jm_bos_lookup`: EFAULT, ENOENT, EINVAL and ENOMEM, duplicates allowed, no references left
    after an error.
  - `jm_heap_fault` (both formats): an injected MMU fault maps exactly the 2 MiB chunk. After it:
    - the AS stays enabled and nothing is logged as unhandled;
    - the chunk is zeroed and NOEXEC;
    - a second fault in the same chunk is a no-op;
    - these give an unhandled fault and a disabled AS: ENOMEM (then a retry succeeds), a
      permission fault, a fault in a non-heap BO, a fault outside any BO;
    - GEM_CLOSE frees the chunks.
  - `jm_fault_hook`: wrong exception types, no bound VM.

## Not verified / needs hardware

- **Which page-table format the H616 G31 accepts.** Linux uses LPAE there; the G31 advertises
  AARCH64_MMU. Both are implemented, and only the mock checks the descriptor bits.
- **That FLUSH_PT / FLUSH_MEM end a lock region and invalidate the TLB on Bifrost v7.** We LOCK
  before the update; Linux locks after.
- **MMU fault retry semantics.** We clear the IRQ after mapping, and the GPU must then resume the
  faulting access.
- **Zeroing and cache behaviour of heap chunks and BOs on real memory**, including whether
  `PW_MEM_UNCACHED` with GPU-cacheable PTEs matches Linux's map_wc behaviour on the H616.
- **EINTR from WAIT_BO.** The mock cannot interrupt a wait.

## Known issues & TODO (prioritised)

1. **Format switch in the glue.** kmd/win needs a registry value, e.g. `JmPageTableFormat = 1`,
   that calls `pw_device_set_pt_format()` between `pw_device_alloc` and `pw_device_probe`, so the
   board can try both formats without a rebuild (task D).
2. **JM `as_enable` / `as_disable` still use the shared sequence.** That sequence is GPU_CMD flush
   + UNLOCK + UPDATE. Linux sends FLUSH_MEM over the whole range before reprogramming a slot. This
   is shared code (lead / jm-hw). Add a JM branch if stale TLB entries show up after LRU eviction.
3. **Page tables are only freed at VM destroy** (an existing pw_mmu.c TODO), so heaps and BO
   churn grow the tables of long-lived files.
4. **No shrinker:** MADVISE always reports retained = 1 (jm-arch open question 6). Linux returns
   retained = 0 for `madv` values ≥ 2³¹; we do not replicate that edge.
5. **VA placement is first-fit** where Linux uses drm_mm best-fit. The constraints are the same;
   the chosen addresses may differ.
6. **SYNC_BO and SUBMIT array caps.** More than 2¹⁶ SYNC_BO ops or 2¹⁸ SUBMIT BO handles fail
   with ENOMEM; Linux is bounded by kvmalloc only.
7. **WB_MMAP / SYNC_BO / QUERY_BO_INFO are done but gated** until `PW_JM_DRM_MINOR` becomes 6
   (jm-arch open question 4).

## Contract changes

All are extensions; CSF behaviour is unchanged.

1. **`pw_core.h`:** new `enum pw_pt_format` and `pw_device_set_pt_format()`, implemented in
   pw_mmu.c. Reason: a selectable legacy page-table format for the board.
2. **`pw_priv.h`:** `struct pw_mmu` gains a `pt_format` field (zero means AArch64).
3. **`pw_mmu.h`:**
   - `struct pw_vm` gains `pt_format` (appended);
   - new `PW_LPAE_*` defines, `pw_mmu_lpae_pte_attrs()`, `pw_vm_transtab()` and
     `pw_vm_memattr()`;
   - the JM TLB sequence is now documented in the header.

   Behaviour on JM only: map/unmap on a bound VM end the lock with FLUSH_PT or FLUSH_MEM (task B5).
   `pw_vm_activate()` and `pw_mmu_as_bind_locked()` now program `pw_vm_transtab()` /
   `pw_vm_memattr()`, which equal the old values for AArch64 VMs.
4. **`pw_jm_mem.h`:**
   - new `pw_jm_bo_create_user()`, so WB_MMAP can be tested at 1.5;
   - new `PW_JM_MAX_SYNC_OPS` and `PW_JM_MAX_JOB_BOS`;
   - implementation notes.

   `pw_jm_mmu_fault` does **not** call `pw_jm_as_vm_get`. It resolves `mmu.slots[as]` through a
   registry of live VMs and holds the registry lock across the growth, so no VM reference is taken
   or dropped in the MMU bottom half. This works for LRU and for direct bindings.
5. **Mock** (`mock_gpu.h`, `pw_os_mock.c`, additions only):
   - `mock_as_state.flush_pts` / `flush_mems` counters;
   - `mock_fail_mem_allocs()`;
   - the legacy LPAE walker (an error on the G610 model);
   - FLUSH_PT/MEM end a LOCK on the G31 model; the G610 model only counts them.

## Review

Independent review, 2026-10-30. Everything is still **untested on hardware**.

### What was re-run

- **Private copy, gcc:** `/tmp/claude-1000/rev-jm-mem` was a fresh copy of `kmd`, `include` and
  `toolchain`, built from clean.
  - Before the fixes: 130 tests, 0 failed, 3 skips.
  - After the fixes: 131 tests, 0 failed, 3 skips.
  - `pw_tests jm_` was run 8 times after the fixes: 35 tests, 0 failed each time.
- **Private copy, clang:** `make CC=clang` from clean gave 131 tests, 0 failed.
- **freestanding:** every undefined symbol is a `pw_*` one.
- **Glue tests:** `make -C kmd/win/tests` gave 9 tests OK.
- **meson `-Dcore=real`:** `panwin.sys` links, and `meson test` gave Ok 4, Fail 0.
- **Shared tree:** an incremental `make -C kmd/tests` (no clean) gave 133 tests, 0 failed,
  3 skips. Other engineers had added tests in the meantime.

### Do the claims hold up?

Yes. Every claim in the report was reproduced or checked against the sources.

- **uAPI against Linux** (`panfrost_drv.c`, `panfrost_gem.c`): the errnos and their check
  order match for CREATE_BO, MMAP_BO, GET_BO_OFFSET, WAIT_BO, MADVISE, SET_LABEL_BO, SYNC_BO
  (`panfrost_gem_sync` order) and QUERY_BO_INFO. So does SUBMIT's BO lookup.
- **WAIT_BO deadline:** `timeout_ns` is absolute, as in `drm_timeout_abs_to_jiffies`.
- **VA rules:** they match `panfrost_gem_open` and `panfrost_drm_mm_color_adjust`.
- **Heap faults:** the 0xF8 / 0xC0 exception-type test, the 2 MiB chunk, the idempotent
  re-fault and the RW, cached, NOEXEC mapping all match Linux.
- **Mali LPAE format:** checked against `io-pgtable-arm.c`, facts only:
  - leaf type 0b01 at every level;
  - HAP read and write in bits 6 and 7;
  - always SH_OS, no access flag, start level 0;
  - TRANSTAB = root | TABLE | READ_INNER;
  - MEMATTR `0x00888D88`.

  One difference: Linux sets XN as bits 53 and 54, and we set only bit 54, the Mali NX bit
  (as in kbase). This is harmless.
- **Unmap flush:** Linux panfrost sends **FLUSH_PT** after an unmap as well. Our FLUSH_MEM
  there is a deliberate, stronger deviation, not Linux behaviour.

### Bugs found and fixed

1. **Freed pages could stay mapped (`jm_bo_release`).** After a failed `pw_vm_unmap`, the pages
   and heap chunks were still freed.
   - When it happens: an AS LOCK timeout on a hung GPU, or ENOMEM while splitting a block.
   - What went wrong: the descriptors and TLB can still point at freed memory, and a reset
     re-enables the AS.
   - Fix: the release now logs the failure and **leaks** the pages instead. A later map of that
     VA returns EBUSY rather than aliasing memory.
   - No test covers it: the mock has no knob that makes an AS command time out.
2. **License hygiene in `pw_mmu.h`.** The LPAE comment quoted a GPL-only Linux comment word for
   word ("Mali seems to need a full 4-level table…"). It is reworded now. An n-gram scan
   (7 words) of the jm-mem files against Linux panfrost and io-pgtable-arm finds no other
   copied text. The `pw_mmu.c` hits are the existing, dual-licensed panthor-derived memattr code.

**New test, `jm_heap_job` (both formats):** WRITE_VALUE jobs run on jm-hw's mock executor and
write into two unpopulated heap chunks.
- The real MMU IRQ path grows the heap: 2 faults, 2 retries and 2 FLUSH_PT.
- The jobs complete and the data lands in memory.
- A job that writes outside any BO fails with an unhandled fault.
- GEM_CLOSE frees both chunks.

No contract changes. The `pw_mmu.h` change is wording only.

### Remaining concerns (ranked)

1. **The fault IRQ is cleared after the heap is grown** (shared `pw_mmu_irq_handler`, owned by
   the lead / jm-arch).
   - Linux `panfrost_mmu_irq_handler_thread` writes MMU_INT_CLEAR *before*
     `panfrost_mmu_map_fault_addr`. kbase also clears before the FLUSH_PT that resumes the
     access.
   - We clear after the map and FLUSH_PT. If the resumed GPU faults again before our clear, that
     new fault's IRQ is wiped and the AS stalls until the job times out.
   - The mock retries on clear, so it cannot show this.
   - Suggested fix: clear first, and make `mock_jm` retry on FLUSH_PT.
2. **Single fence per BO vs. cancellation** (jm-sched contract).
   - The model only holds if a job's fence never signals before the job's implicit
     dependencies.
   - JM_CTX_DESTROY or file close dropping queued jobs with -ECANCELED must first wait for those
     dependencies, as `drm_sched_entity_kill` does.
   - Otherwise WAIT_BO reports idle, and the next SUBMIT skips a writer that is still running.
3. **Bus faults are not serviced on JM** (shared code). `MMU_INT_FAULT_MASK` covers bits 0-15
   only. On JM, bits 16-31 are per-AS bus faults, which panfrost handles (`BIT(as + 16)`). They
   currently go unserviced until the job times out.
4. **JM AS enable/disable** still use the shared GPU_CMD flush + UNLOCK sequence (TODO 2 above).
   Linux sends FLUSH_MEM over the whole range there.
5. **`vms_lock` is held across the 2 MiB allocation and the map** in the MMU bottom half. This
   is correct, but it serialises every file open and close with heap growth.
6. **Cosmetic:** a HEAP CREATE_BO whose size wraps when rounded up to 2 MiB returns EINVAL. Other
   oversized BOs return ENOMEM.
