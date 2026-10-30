# kmd-core status

Owner key: `kmd-core`. Scope: the OS-neutral Mali CSF core for milestones M1 and M2, and a host test suite.
Date: 2026-10-30.

## Summary

`kmd/core/` now implements everything declared in `pw_core.h`. The bring-up sequence is:

1. Read GPU_ID and the feature registers, and fill `drm_panthor_gpu_info` the same way panthor does.
2. Soft reset.
3. Set COHERENCY_PROTOCOL to NONE, then power on the L2.
4. MMU: AArch64 page tables written from the Arm ARM, plus AS0–AS15 slot programming.
5. Firmware:
   - parse `mali_csffw.bin` with bounds checks;
   - build the MCU VM and map every IFACE section with the flags it asks for;
   - boot the MCU;
   - discover the global, CSG and CS interfaces, with all of panthor's checks;
   - run global init;
   - send one GLB_PING.

Interrupts use a top-half/bottom-half split for the job, MMU and GPU lines, and MMU and GPU faults are decoded. Logic is ported from Linux panthor (dual GPL-2.0/MIT, used under MIT) and every non-obvious offset or sequence cites `file:line` in v7.3. The page-table code was written from the architecture spec. Nothing came from io-pgtable-arm.c.

The core compiles freestanding for ARM64 Windows with zero warnings. The only symbols it imports are `pw_os_*`. It also links into the `kmd/win` KMDF project as `panwin.sys` with `-Dcore=real`.

There are 31 host tests, all passing under ASan and UBSan, with gcc and with clang. They exercise the real firmware binary and a behavioural G610 model. **Everything is untested on hardware.**

## What was built and how to build/test it

| Path | Content |
|---|---|
| `kmd/core/pw_regs_{gpu,mmu,fw}.h` | Register maps from panthor `*_regs.h`. MMU and MCU offsets are absolute here, whereas panthor's are relative. Also the exception codes. |
| `kmd/core/pw_fw_iface.h` | Firmware binary format, the global/CSG/CS control, input and output structs, and the req/ack bits. The layout is pinned with `_Static_assert`s. |
| `pw_gpu.c` | gpu_info, model names, soft reset, L2 on and off, cache flush, GPU IRQ (GPU_FAULT_STATUS and GPU_FAULT_ADDR). |
| `pw_mmu.c/.h` | 4K and 2M map and unmap, block split, rollback on failure, MAIR→MEMATTR, AS UPDATE, LOCK (pack_region_range), FLUSH and UNLOCK, AS_STATUS wait, fault decode. |
| `pw_fw.c/.h` | Pure parser (`pw_fw_parse`), section loader, MCU boot, iface init, global init, req/ack waits, doorbells, ping, job IRQ (acks GLB_IDLE). |
| `pw_irq.c` | ISR and bottom-half state machine, register polling. |
| `pw_device.c` | Lifetime and DEV_QUERY payloads. The firmware name is derived as `arm/mali/arch%u.%u/mali_csffw.bin`. |
| `pw_util.c/.h` | `pw_mem*`, `pw_str*`, a small `pw_snprintf`, bit helpers, barriers. |
| `kmd/core/meson.build` | Defines only `pw_core_sources` and `pw_core_inc`. |
| `kmd/compat-km/sys/types.h` | Minimal header that keeps CRT headers out of the kernel build. |
| `kmd/tests/` | `pw_os_mock.c` (mock OS and G610 model), `test_*.c`, `Makefile`. |

Added to `pw_core.h` (additions only):
- `pw_device_alloc()` and `pw_device_probe()`: two-phase creation.
- `pw_device_fw_booted()`
- `pw_device_fw_ping()`
- `pw_device_get_mmu_info()`

Commands:
```
cd kmd/tests && make            # build + run all tests (gcc, ASan+UBSan, -Werror)
make clean && make CC=clang     # same with clang
make freestanding               # aarch64-w64-mingw32-clang -ffreestanding ... -Werror on every core .c, lists imports
./pw_tests mmu                  # filter; PW_TEST_VERBOSE=1 prints core logs; PW_FW_PATH=... overrides firmware
# integration (scratch build dir, glue owned by kmd-win):
meson setup /tmp/b kmd --cross-file toolchain/meson/aarch64-w64-kmd.ini -Dcore=real && ninja -C /tmp/b && meson test -C /tmp/b
```

### Mock / model (`kmd/tests/pw_os_mock.c`)

- **Register values.** GPU_ID is 0xa8670000. The feature values come from Mesa's "Dumped from a G610" (drm-shim, MIT): MMU 0x2830 (48 VA bits, 40 PA bits), shader_present 0x50005, AS_PRESENT 0xff.
- **Completions.**
  - Soft reset raises RESET_COMPLETED, and flush raises CLEAN_CACHES_COMPLETED. Both appear asynchronously, 100 µs after the command.
  - L2 shows PWRTRANS, then READY, then raises POWER_CHANGED (which the core must keep masked).
  - AS commands report ACTIVE for a few reads. The model flags any protocol violation: UNLOCK without a cache flush, a command while busy, a double LOCK.
- **Memory.**
  - Fake physical addresses start at 4 GiB. Non-contiguous allocations get reversed, gapped PAs.
  - A cached KMAP is a separate buffer, so the emulated GPU only sees what was `pw_os_mem_flush()`ed.
- **Page tables.** An independent AArch64 walker reads "DRAM" through the latched AS configuration. It checks AF, AP[2] and XN.
- **Emulated MCU.**
  - On MCU_CONTROL=AUTO it checks that the code at 0x800000 is executable and read-only, and that the shared section is writable through AS0.
  - It then writes a v1.1.0 interface (8 CSGs × 8 CS, 96 registers, 8 scoreboards) and raises GLOBAL_IF.
  - Doorbell 0 echoes req→ack and raises GLOBAL_IF according to ack_irq_mask.
- **IRQ delivery.** One thread per line calls `pw_irq_top`/`pw_irq_bottom`, like panthor's threaded IRQs. Lines that assert but aren't claimed are counted.

## Verified (actually run here)

- **Freestanding build.**
  - The exact command from the task, plus `-Werror`, compiles every core `.c` with zero warnings.
  - `llvm-nm -u` shows only `pw_os_*` imports, with no memcpy or memset.
  - `panwin.sys` (81920 bytes, ARM64, NATIVE subsystem) links with the real core, and the glue's `meson test` passes 2/2.
- **Layout and register cross-check.**
  - `panthor_fw.h` compiled with Linux types: every interface struct size and member offset matches ours.
  - All ~170 scalar macros from panthor's `fw.h` and `*_regs.h` have equal values. The only differences are four where my test shim's `BIT()` was 32-bit; ours are correctly 64-bit.
- **Full suite.** `make test` gives 31/31 OK, with gcc and with clang.
- **Repeat runs.**
  - The suite was run 12 times in a row and 8 times in parallel, all green.
  - Two earlier flaky failures were races in the tests themselves, not in the core: one sampled a mask while its bottom half was running, the other sampled AS state before the disable finished. Both are fixed by waiting.
- **Real firmware.**
  - 8 IFACE entries, at 0x0, 0x400000, 0x410000, 0x800000, 0x1000000, 0x2000000, 0x3000000 and 0x4000000.
  - The 0x3000000 section has the PROT flag and is skipped, as panthor does. That leaves 7 mapped.
  - Header v0.3, hash 0x1050000, size 792, git sha `95a25d71…`.
- **Parser robustness.**
  - Every truncation from 0 to 2047 bytes, and strided lengths up to the full size, is rejected.
  - 32 crafted cases (malformed images plus a few deliberately valid edge cases) each give the expected result.
  - 20,000 random mutations ran with no ASan or UBSan error, and the callback invariants held (6,188 were accepted).
  - 150 mutated images went through the full loader (57 booted) with no leaks on the error paths.
- **MMU.**
  - 4K attributes for each flag combination.
  - 2M blocks, and the split on partial unmap.
  - Fault levels are reported correctly.
  - Overlapping maps return EBUSY with full rollback, and range and PA-width checks work.
  - A locked update shows exactly one LOCK, one flush (cmd 0x23304) and one UNLOCK, with the LOCKADDR encoding checked.
  - TRANSCFG is 0x420001c6 and MEMATTR is 0x4c4c4c4c8c4c8f4c.
  - The 7 AS slots run out as expected.
  - A control test confirms that an unflushed PTE is invisible to the emulated GPU.
- **Device bring-up**, in four modes:
  - IRQ-driven;
  - IRQ-only completions (RAWSTAT hidden from waiters);
  - no IRQs at all (polling fallbacks, about 30 ms);
  - M1 mode.
- **Bring-up results.**
  - gpu_info and csif_info are as expected.
  - Global input: core_en_mask 0x50005; poweroff and idle timers 235 and 19 (at 24 MHz); progress timer 2560000; ack_irq_mask 0x0400050e.
  - Five pings succeed.
  - Seen through AS0, the section contents match the file, and permissions and cache attributes follow the section flags.
- **13 failure paths**, each with the right error code and log line and no leaks: GPU_ID 0, arch 9, arch 14, reset timeout, L2 timeout, missing firmware, MCU never boots, interface version 0, group_num 2, bad input VA, stream_num 40, mismatched CSG, doorbell ignored.
- **IRQ paths.**
  - Direct top-half and bottom-half calls.
  - GPU fault log: `GPU Fault 0x000000c4 (TRANSLATION_FAULT_4) at …`.
  - MMU fault decode: exception, access type (READ, WRITE, EXECUTE, ATOMIC), decoder versus slave, source id. The AS is disabled, the mask is cleared, and re-activation clears the fault.
  - REQ_COMPLETE bits are never delivered.
  - GLB_IDLE is acknowledged.
- **ThreadSanitizer.** One run reported only expected races: core and emulated MCU on shared interface memory (inherent to the protocol), and a test counter, since fixed.

## Not verified / needs hardware

- Everything depends on the real G610: register behaviour, timing, whether IRQ completions arrive, and whether the real arch10.8 firmware boots with this sequence.
- The mock's firmware interface (v1.1.0, the layout, the strides) is plausible but invented. On hardware, the version, strides and CS features will be whatever the firmware reports.
- GPU_ID revision and status bits are unknown; the mock uses 0.
- The shareability and cacheability attributes on the GPU (SH_IS plus MIDGARD_INNER for write-back, non-cacheable plus outer-shareable for uncached). These are copied from panthor's effective configuration, which I am confident of but have not verified on Mali.
- The 24 MHz arch-timer assumption used for FW timeouts.
- The barriers: `dsb st` before doorbells, and WC behaviour of the shared section.

## Known issues & TODO (prioritised)

1. **Pass the arch-timer rate from the OS instead of the hard-coded 24 MHz.** See requested contract changes.
2. **No periodic FW watchdog ping.** panthor pings every 12 s. The core exposes `pw_device_fw_ping()`; the OS layer or a future core timer should call it.
3. **No AS LRU eviction.** VMs beyond 7 active ones get EBUSY. Needed at M4.
4. **Empty page-table pages are never freed on unmap.** They are freed only at VM destroy, because freeing safely has to wait until after the TLB flush.
5. **No reset or recovery path yet.** This would be panthor's pre_reset/post_reset with a section reload; the section data is already kept for it. Today a flush or AS timeout is logged and the caller gets the error.
6. **Deliberate deviations from panthor**, all commented in the code:
   - A soft reset at probe.
   - The L2_READY wait checks `(val & mask) == mask`. panthor's `(mask & val) == val` (gpu.c:265) is already true while L2_READY reads 0.
   - RAWSTAT and ack polling fallbacks on every wait, so the driver works even if IRQ routing is broken.
   - The ISR claims and re-masks a line that asserts while its bottom half is already running.
   - The section tail is always zeroed.
   - Extra parser caps: entry size of at least 4, at most 256 entries, at most 32 MiB per section and 64 MiB total, exactly one shared section of at least 8 KiB.
   - One GLB_PING at the end of init.
7. **Scheduler events are ignored.** CSG IRQs are only logged until M3.

## Requested contract changes

- **`pw_device_params`: add `uint32_t timer_hz`** (CNTFRQ_EL0) for FW timer conversion (fw.c:420). I can't change the struct, so the core assumes 24 MHz.
- **`pw_os.h`: document three semantics**:
  - Whether `pw_os_reg_write32` orders earlier Normal-memory writes. The core issues `dsb st` itself before doorbells, so this is safe either way.
  - That events are manual-reset. kmd-win uses NotificationEvent; the core tolerates either kind.
  - That `pw_os_mem_flush` cleans to the Point of Coherency.
- **`pw_os.h`, later (M4): add a `PW_MEM_ALIGN_2M` flag**, or a 2 MiB-aligned contiguous allocation, so BOs can use 2M blocks.
- **Obligations on the OS layer** (kmd-win already follows the first two, from reading its sources):
  - Call `pw_device_alloc`, publish the pointer to the ISR, then call `pw_device_probe`.
  - Give each line its own bottom-half work item. The MMU bottom half can wait for a GPU cache flush.
  - Disconnect the ISR and flush queued bottom halves before `pw_device_destroy`.
  - After a failed probe the device is quiesced but must still be destroyed.
- **No system packages needed.**

## Review

Independent review, 2026-10-30. Nothing here has run on Mali hardware or on ARM64 Windows.

### What was re-run (clean build dirs) and results

| Command | Result |
|---|---|
| `cd kmd/tests && make clean && make` (gcc, ASan+UBSan, `-Werror`) | 31/31 OK before fixes; 32/32 OK after (one new test); 5 extra repeat runs green |
| `make clean && make CC=clang` | 32/32 OK, 3 repeat runs green |
| `make freestanding` | every core `.c` compiles with zero warnings; undefined symbols are only `pw_os_*` |
| `meson setup <scratch> kmd --cross-file toolchain/meson/aarch64-w64-kmd.ini -Dcore=real && ninja && meson test` | `panwin.sys` (81920 bytes) links; 2/2 OK; imports only ntoskrnl/hal/WDFLDR (memcpy/memset/strlen from ntoskrnl) |
| Own cross-check: every object-like macro in panthor `*_regs.h` and `panthor_fw.h` (209 names) evaluated against ours | all equal, except MCU_CONTROL/MCU_STATUS, which are absolute here by design (+0x700) |
| Own cross-check: `sizeof`/`offsetof` of panthor's interface structs vs ours | identical |

The real-firmware parser test ran (no skips), so the 8-IFACE / 7-mapped claim was reproduced.

### Code review against references

These match panthor (v7.3 tree in `references/linux`):
- GPU_ID decode, gpu_info fill, coherency NONE before L2_PWRON, flush command encoding (0x23304), soft-reset sequence.
- AS enable/disable order, TRANSCFG (0x420001c6 for 48-bit VA), `mair_to_memattr`, `pack_region_range`, lock → flush → unlock, fault decode.
- Firmware header/entry/section checks, `iface_fw_to_cpu_addr`, CSG/CS discovery checks, global init (core_en_mask, timers, ack_irq_mask, req toggles, doorbell 0), job IRQ boot detection.

The PTE attribute choice (nG, AP[1], AP[2] for read-only, XN, WB+SH_IS / NC+SH_OS, Device for MMIO) is the same as the Linux stage-1 table semantics.

License hygiene is fine:
- Every file has an MIT SPDX header.
- The panthor files used are all "GPL-2.0 or MIT".
- There is no io-pgtable-arm.c or panfrost code, and the page-table code is structurally independent.

### Bugs found and fixed

1. **IRQ line could stay masked forever** (`pw_irq.c`, `pw_irq_bottom`).
   - Unlike panthor, the ISR takes no lock. So `pw_irq_enable/disable_events` can re-write INT_MASK after the ISR masked the line.
   - A second ISR then samples PROCESSING, and can write INT_MASK = 0 just after the running bottom half re-armed the line and set ACTIVE. It still claims and requeues the work item.
   - The requeued bottom half saw ACTIVE and did not restore the mask. The line was then dead, with only the polling fallbacks left (fatal for MMU faults and, later, scheduler events).
   - Fix: the bottom half re-arms whenever the state is not SUSPENDED.
   - Regression check added to `irq.top_bottom_gpu_fault`; it failed before the fix.
2. **2 MiB map wrongly returned PW_EBUSY over an empty level-3 table** (`pw_mmu.c`, `vm_map_pa_locked`).
   - Unmap keeps level-3 tables (known TODO), so after mapping and unmapping any 4K page in a 2 MiB slot, a later 2M-eligible map of that slot failed although nothing was mapped there.
   - Fix: when a table is already present, fall back to 4K pages in that table, which still detect real overlaps.
   - New test: `mmu.block_over_empty_table`, which covers both the fallback and a real overlap with rollback. It failed before the fix.
3. **Firmware ack waits busy-spun** (`pw_fw.c`, `fw_wait_acks`).
   - `fw.req_event` is manual-reset on Windows (NotificationEvent) and was never reset after boot. Every `pw_os_event_wait(…, 1)` returned at once, so each ping or ack wait spun a CPU for up to its whole timeout.
   - Fix: reset the event before re-reading the ack. No lost wakeup for the waiter; a concurrent waiter loses at most one slice.
4. **Hardening** (`pw_gpu.c`): JOB_INT_MASK and MMU_INT_MASK are written to 0 right after GPU_ID is validated. Those lines are only initialised later (never in M1 mode), so a mask left by a previous owner could otherwise storm an unclaimed level-triggered line. The glue has a fallback for this, but the core no longer relies on it.

### Remaining concerns, most severe first

1. **(High) Real hardware is unverified.** Real G610 or arch10.8 firmware behaviour has not been seen. The mock's firmware interface (v1.1.0 layout and strides) is invented, so "fw booted" against the mock only shows that the core follows its own model of the protocol.
2. **(Medium) The L2_READY wait is stricter than panthor's.** panthor's check `(mask & val) == val` (gpu.c:265) is a no-op at val 0, so panthor never really waits. If real L2 power-up takes longer than 20 ms, our probe fails where panthor would carry on. kbase does wait for READY, so the stricter check is defensible. If bring-up shows this timeout, make it a warning.
3. **(Medium) No reset or recovery.** A flush, AS or ping timeout leaves the GPU wedged until reboot. There is also no 12 s watchdog ping.
4. **(Medium) The FW timer rate is hard-coded to 24 MHz.** That is correct for RK3588's system counter, but should come from CNTFRQ via the OS (the `timer_hz` contract request stands).
5. **(Low) Windows timing granularity.** On Windows, `KeWaitForSingleObject` with a 1 ms timeout rounds to the clock tick (about 15.6 ms). The polling fallbacks, and bring-up without interrupts, will therefore be much slower than the 30 ms measured on the host.
6. **(Low) Shared GPU event.** Soft reset and cache flush share one `reqs_event`. This is harmless today, because the waits are serialised at probe and flushes take `flush_lock`.
7. **(Low, kmd-win) All-ones register reads.**
   - With `regs_enabled` false, `pw_os_reg_read32` returns 0xFFFFFFFF, which `pw_irq_top` would treat as a pending interrupt. Today this can't happen, because the ISRs are disconnected before `regs_enabled` is cleared.
   - The kmd-win comment says KMDF flushes interrupt work items before D0Exit; the core's destroy precondition depends on that.
8. **(Low) Empty page-table pages are never freed before VM destroy.** This is unchanged; it now only costs memory, not correctness.

### Verdict

The piece's claims held up:
- The build, test, freestanding and integration results reproduce.
- The register, bit and struct cross-checks are correct.
- The deviations from panthor are documented and reasonable.

The review did find two real logic bugs, one IRQ race and one MMU remap, plus a CPU-burn issue, all outside what the original tests exercised. All are now fixed and covered by tests.
