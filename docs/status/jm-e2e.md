# jm-e2e: the Mali-G31 job path end to end below Mesa

Owner key: `jm-e2e`. Date: 2026-10-30. **Hardware-free. Untested on hardware.** x86_64 under wine
only; no ARM64 binary was run and nothing here says anything about a real G31.

## Summary

Mesa cannot drive the G31 on Windows yet. Patch 0010 leaves `panfrost_kmod.c` out of the Windows
build, and changing that needs a human-written change under `src/panfrost`
([`jm-arch.md`](jm-arch.md) open question 1). So this work proves the stack **below** Mesa:

```
test_libdrm_jm.exe   the libdrm calls + uAPI structs Mesa's panfrost_kmod.c / panvk JM / Gallium pan_jm.c use
  -> libdrm.dll      libdrm-win, transport "core" (PANWIN_TRANSPORT=core PANWIN_CORE_MODEL=g31)
  -> kmd/core        the REAL JM backend: pw_jm_hw, pw_jm_mem, pw_jm_sched (+ shared file/sync/mmu code)
  -> mock G31        kmd/tests/pw_os_mock.c + mock_jm.c (job-chain executor)
```

**New test, `userspace/libdrm-win/tests/test_libdrm_jm.c`** (meson test `libdrm_jm`, 527 checks).
It runs these steps in order:

1. **Enumeration and open.**
   - `drmGetDevices2(0, NULL, 0)` and then the list: one platform device, `arm,mali-bifrost`.
   - `panwin_open` of the render node.
   - `drmGetVersion`: `panfrost` 1.5.
   - `GET_CAP`: SYNCOBJ is 1, SYNCOBJ_TIMELINE is 0.
2. **GET_PARAM.** Covers all 20 params that `panfrost_dev_query_props`, `_thread_props` and
   `query_timestamp` read. Every one must succeed, as Mesa's `required = true` expects.
   - Mesa-level sanity: arch 7, cores present, MMU VA bits ≥ 33, timestamp frequency ≠ 0,
     MEDIUM allowed and HIGH not, coherency NONE.
   - SYSTEM_TIMESTAMP is monotonic.
   - pad and unknown params give EINVAL.
3. **BOs.**
   - CREATE_BO in three kinds: exec (VA aligned to its size), NOEXEC 4 MiB (2 MiB aligned), and
     HEAP|NOEXEC (rounded to 2 MiB, so MMAP_BO gives EINVAL).
   - Every VA is in Mesa's `[32 MiB, 4 GiB)`; GET_BO_OFFSET returns the same VA as CREATE_BO.
   - MMAP_BO + `panwin_mmap`. A fresh BO reads zero. Two mappings alias the same pages. A
     read-only mapping works; an oversized mapping gives EINVAL.
   - CREATE/MMAP/GET_BO_OFFSET errnos.
   - At 1.5, WB_MMAP gives EINVAL and SYNC_BO / QUERY_BO_INFO give ENOTTY.
   - `drmCloseBufferHandle`: a second close gives EINVAL, a closed handle gives ENOENT, and a live
     mapping keeps its pages.
4. **Submit.** A 9-job Bifrost v7 chain on JS1, built by the CPU in BO memory from Mesa
   `genxml/v7.xml`.
   - The chain: NULL, WRITE_VALUE IMM32 / IMM64 / IMM16 / ZERO / SYSTEM_TIMESTAMP /
     CYCLE_COUNTER / IMM8, and a COMPUTE job depending on two earlier jobs.
   - It is submitted with `bo_handles`, a pre-signalled `in_sync`, `out_sync` and
     REQ_CYCLE_COUNT.
   - Then a FRAGMENT + WRITE_VALUE chain on JS0 (REQ_FS), chained with panvk's
     `in_sync == out_sync`.
   - Wait with `drmSyncobjWait(INT64_MAX)`, then read back on the CPU:
     - the exact values, partial-width writes included;
     - the GPU timestamp lies between two GET_PARAM timestamps;
     - every descriptor's exception status is DONE.
   - The GPU walking CPU-written descriptors through the WC mapping is the "CPU writes visible to
     the GPU" check.
   - Also: the SUBMIT errnos.
5. **Implicit sync.** P is 8 chains × 4000 jobs on JS1. A waits for P through `in_sync`. B runs on
   JS0 and shares only one BO with A.
   - B is ready long before A, but must still run after A. The final value must be B's, and the
     timestamps must be ordered P ≤ A ≤ B.
   - WAIT_BO: a poll gives EBUSY (or idle), a 100 µs deadline gives ETIMEDOUT (or idle), and
     INT64_MAX gives idle.
   - pad and unknown handle give EINVAL and ENOENT.
   - P's BOs are closed right after their submits; the jobs keep them alive.
6. **Failing jobs.** For each fault below, `drmSyncobjWait` still returns 0 (Linux semantics) and
   the fence carries **-EINVAL**:
   - a JOB_CONFIG_FAULT, from a type-0 descriptor in the middle of a chain: the first job ran,
     the faulting descriptor reads 0x40, and the job after it did not run;
   - an unhandled GPU page fault (a write at 0xF0000000).

   Afterwards the same file runs jobs again, because the AS is re-enabled.
7. **Heap growth.**
   - WRITE_VALUE into two untouched 2 MiB chunks of a 4 MiB HEAP BO: both are grown on the GPU
     fault and the 4 jobs end DONE.
   - A write past the heap's end gives -EINVAL.
8. **JM_CTX_CREATE/DESTROY, MADVISE, SET_LABEL_BO.**
   - Contexts:
     - LOW and MEDIUM can be created; HIGH gives EACCES and priority 7 gives EINVAL;
     - submits on those contexts work, including FS;
     - destroy works once, then EINVAL; destroying ctx 0 or using pad != 0 gives EINVAL;
     - submitting to a destroyed context gives EINVAL;
     - one context is left for file close to clean up.
   - MADVISE: DONTNEED then WILLNEED leaves `retained = 1` and the contents intact. An unknown
     handle gives ENOENT.
   - SET_LABEL_BO:
     - a label, a 4095-character label (Mesa's truncation), and NULL all succeed;
     - 4096 characters without a NUL give E2BIG;
     - pad != 0 gives EINVAL and an unknown handle gives ENOENT.
9. **Stress: 1000 submits** (`PANWIN_JM_STRESS`) over 2 files, so 2 address spaces.
   - The mix: alternating JS1/JS0, the default and a LOW context, an `in_sync` on the previous
     submit every 5th time, `out_sync` round-robin over 4 syncobjs per file, and a WAIT_ALL every
     100 submits.
   - Each submit writes its number and a GPU timestamp into its own slot, plus its number into a
     per-file word shared by all of that file's jobs.
   - Checks: all values are right, all descriptors are DONE, timestamps never go backwards in
     submit order (implicit sync across both slots), the shared word ends at the last submit, and
     every fence status is 1.
10. **A third file is closed with work in flight.** It has a 2000-job chain running and leaves
    BOs, a syncobj and a context behind.
11. **Teardown** after the last close:
    - device down, devices up = down, open files 0;
    - leaked allocs / mems / sync / maps / events all 0;
    - mock model errors 0, unclaimed IRQs 0, no CS instructions.

**`tests/e2e/run.sh`** gains a `jm` step next to the CSF/Mesa one:
- `run.sh` runs everything;
- `run.sh jm` runs only the libdrm-win build and the jm step (no Mesa needed);
- `run.sh csf` runs the old flow only.

The jm step copies the core `libdrm.dll` and `test_libdrm_jm.exe` to `out/e2e-jm-x86_64/` and runs
them with `PANWIN_CORE_MODEL=g31 PANWIN_CORE_REPORT=1`. The expected ioctl-failure lines the report
prints are filtered out. The CSF steps are unchanged.

**No bug was found in kmd/core or the mock on this path.**

**Mutation check.** I broke implicit sync on purpose in a scratch copy: in `pw_jm_sched.c`, BO
fences were no longer added as dependencies. The test failed in 6 of 6 runs:
- The stress step fails every time: 19-120 backwards timestamps, and the wrong final shared word.
- The implicit-sync step fails in about 4 runs out of 6: final value A's, B older than A.

The shipped code was not changed.

## How to build/test

```sh
# everything (libdrm-win core build + its meson tests, Mesa e2e build if missing, wine runs):
/home/e3xp0/Projects/panfrost-win/repo/tests/e2e/run.sh            # [--rebuild] [csf] [jm]
/home/e3xp0/Projects/panfrost-win/repo/tests/e2e/run.sh jm         # G31 only, no Mesa needed
# just the JM test, from the run directory the jm step fills:
cd /home/e3xp0/Projects/panfrost-win/repo/out/e2e-jm-x86_64
PANWIN_TRANSPORT=core PANWIN_CORE_MODEL=g31 WINEDEBUG=-all wine ./test_libdrm_jm.exe
PANWIN_JM_STRESS=20000 PANWIN_TRANSPORT=core PANWIN_CORE_MODEL=g31 WINEDEBUG=-all wine ./test_libdrm_jm.exe
# (PANWIN_CORE_REPORT=1: transport verdict + every failing ioctl; PW_TEST_VERBOSE=1: core log)
# kernel core suites
make -C /home/e3xp0/Projects/panfrost-win/repo/kmd/tests            # gcc, ASan+UBSan, -Werror
make -C /home/e3xp0/Projects/panfrost-win/repo/kmd/tests freestanding
# clang in a private copy (others test in the shared tree):
C=/tmp/claude-1000/jme2e/clang; mkdir -p $C && cp -a repo/kmd repo/include repo/toolchain $C/ && \
  rm -rf $C/kmd/tests/obj $C/kmd/tests/pw_tests && \
  PW_FW_PATH=/home/e3xp0/Projects/panfrost-win/references/firmware/mali_csffw.bin make -C $C/kmd/tests CC=clang
```

I ran the e2e in a private copy: `cp -a repo /tmp/claude-1000/jme2e/repo` without `out/build`,
plus a `references` symlink next to it, because `run.sh` resolves `../references`.

## Verified

All of this was run here on 2026-10-30. The private copy was identical to the shared tree for
`kmd/` and `userspace/libdrm-win/` (checked with `diff -rq`).

- **Baseline, before any change:** `tests/e2e/run.sh` PASS. meson 4/4; vk_e2e 14/14; icd_smoke OK.
- **`tests/e2e/run.sh` (csf + jm), final code: PASS.**
  - meson test 5/5 OK: `libdrm_fake`, `libdrm_core`, `libdrm_jm`, `libdrm_core_g31`, `ioctl_map`.
  - jm step: "OK: 527 checks, 0 failures". The transport reports "device down: leaked allocs=0
    mems=0 sync=0, mock model errors=0, unclaimed irqs=0 -> clean".
  - vk_e2e: "RESULT: PASS, last consecutive passing step 14 of 14". icd_smoke: OK.
- **`run.sh --rebuild jm`: PASS.** This was a clean libdrm-win rebuild. An unknown argument gives
  a usage message and rc 2. `--rebuild csf` (a Mesa rebuild) was not run.
- **`run.sh jm` alone: PASS.** The 1000-submit stress takes about 50 ms (about 16-21k submits/s
  on the mock), and the implicit-sync step about 8-13 ms.
- **Repeat runs:**
  - `test_libdrm_jm.exe` 12 times (4 in parallel × 3 rounds): 12/12 OK.
  - `PANWIN_JM_STRESS=20000`: OK, 1.23 s.
- **The mutation run above** (scratch copy `/tmp/claude-1000/jme2e/mut`): detected in 6/6 runs.
- **`userspace/libdrm-win/build.sh all`** (arm64 + x86_64, no core transport): rc 0; meson 2/2 OK;
  no warnings in any of my builds.
- **`make -C repo/kmd/tests`** (shared tree, gcc, ASan+UBSan, -Werror): **149 tests, 0 failed, 0
  skips**.
- **`make -C repo/kmd/tests freestanding`** (shared tree): rc 0. The undefined symbols are `pw_*`
  only.
- **`make CC=clang`** (fresh private copy of kmd/include/toolchain): **149 tests, 0 failed, 0
  skips**.

## Not verified / needs hardware

- Everything on a real G31: the whole JM backend is mock-only. In particular:
  - whether the G31 writes the exception status back on a *page* fault (the mock leaves the
    descriptor at 0 there; the test only requires "not DONE");
  - the real WRITE_VALUE widths of ZERO and IMM16;
  - the timestamp and cycle-counter values;
  - heap growth timing.
- Mesa itself on the G31 (panvk / Gallium): blocked on jm-arch open question 1. This test makes
  the same calls, but Mesa's own logic is not exercised: pandecode, the tiler heap descriptor,
  real shaders.
- ARM64 libdrm-win against panwin.sys on a G31. Only the x86_64 in-process core transport ran.
- WAIT_BO really returning ETIMEDOUT: the mock finishes too fast for the 100 µs case to be
  deterministic, so both results are accepted. The core's own `jm_wait_bo` test covers it.

## Known issues & TODO (prioritised)

1. **Lead / human: Mesa on the G31** (jm-arch open question 1). Once `panfrost_kmod.c` builds on
   Windows, add a Mesa panvk-v7 step (`PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1`) to `run.sh jm`, like
   vk_e2e for the G610.
2. **The mock serializes the slots.** `mock_jm.c` runs a whole chain while holding the mock lock,
   so a submit's register writes wait behind a running chain, and JS0 and JS1 never really run at
   the same time. Implicit-sync ordering is still checked (see the mutation check), but true
   cross-slot concurrency is not. A per-job lock release in the executor would make the e2e
   sharper (owner jm-hw).
3. **Fence errors are invisible to Windows userspace** except through the test hook:
   SYNCOBJ_WAIT ignores them, as on Linux, and there is no sync_file. Mesa only sees GPU faults
   through descriptor status in its debug paths (`PANVK_DEBUG=sync`, `PAN_DBG_SYNC`), which is the
   same as on Linux, so this is not a bug. If Windows ever needs a device-lost signal, it needs a
   new uAPI.
4. **Failing-job coverage** covers config and page faults only. Timeouts / reset, context destroy
   in flight and a failed reset (-ECANCELED / -EIO) need mock injection knobs that the libdrm
   level cannot reach; they are covered in `kmd/tests/test_jm_sched.c`.
5. `run.sh` runs `test_libdrm_jm` twice: once in meson test and once in the jm step (about 1 s).
6. The 1.6 checks (WB_MMAP EINVAL, SYNC_BO / QUERY_BO_INFO ENOTTY) are conditional on the reported
   minor, so raising `PW_JM_DRM_MINOR` does not break the test. The test does not exercise them at
   1.6; add that when the minor is raised.

## Contract changes

All are additive; no existing behaviour changes. CSF e2e, `libdrm_core`, `libdrm_core_g31` and
the kmd suites are unchanged and pass.

1. **`userspace/libdrm-win/include/panwin_core.h`** (owner jm-glue / e2e): new
   `panwin_core_syncobj_status_fn` and `PANWIN_CORE_SYNCOBJ_STATUS`.
   - `int panwin_core_syncobj_status(int fd, uint32_t handle, int32_t *status)` returns the fence
     state of a binary syncobj in sync_file terms: 1 = signalled OK, 0 = pending, < 0 = signalled
     with that errno.
   - Reason: the task needs "a failing job surfacing as a fence error", and nothing in the uAPI
     shows a fence error on Windows (see Known issues 3).
2. **`userspace/libdrm-win/src/transport_core.c`** (owner jm-glue): the implementation of (1),
   appended at the end of the file and `__declspec(dllexport)` like `panwin_core_get_report`.
   It exists only in `-Dcore_transport=true` builds; `libdrm.def` is unchanged.
   - It resolves the fd with `pw_file_get()`, refuses non-core files with EINVAL, and calls the
     core's `pw_syncobj_find_fence()` / `pw_fence_is_signalled()` / `pw_fence_error()`.
   - Those four prototypes are declared locally: `pw_sync.h` would clash with libdrm-win's own
     `struct pw_file` in `pw_internal.h`.
3. **`userspace/libdrm-win/meson.build`** (owner jm-glue): new test executable and meson test
   `libdrm_jm` (core transport builds only, `PANWIN_CORE_MODEL=g31`).
4. **`tests/e2e/run.sh`** (mine):
   - optional step selection `csf` / `jm`, with the default unchanged (everything);
   - the new jm step;
   - `--rebuild` now really cleans the libdrm-win build (`ninja -t clean`; the header always said
     it forced it);
   - `PANWIN_CORE_REPORT=1` is set per step instead of globally;
   - a per-step result line at the end.
