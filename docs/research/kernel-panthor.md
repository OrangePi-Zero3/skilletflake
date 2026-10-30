# Linux kernel side: panthor (CSF) / panfrost (JM) analysis for a Windows-on-ARM Mali driver

Author: kernel-side researcher. Date: 2026-10-30.

Reference clone: `/home/e3xp0/Projects/panfrost-win/references/linux`. It is a sparse, depth-1 clone from
`https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git`. GitHub returned HTTP 429 during this
session, so kernel.org was used instead. kernel.org ignored `--filter=blob:none`, so the clone is a full
depth-1 copy of about 300 MB. Its HEAD is `d89e1d186ec9` (2026-10-29), kernel **v7.3.0**, "Baby Opossum Posse".

Sparse paths checked out: `drivers/gpu/drm/{panthor,panfrost}`, `drivers/iommu/io-pgtable-arm.c`,
`include/linux/io-pgtable.h`, `include/uapi/drm/{panthor,panfrost,}drm.h`,
`arch/arm64/boot/dts/rockchip/rk3588*.dtsi`, `rk356*.dtsi`, rock-5b dts, `drivers/pmdomain/rockchip`,
`include/dt-bindings/{power/rk3588-power.h,clock/rockchip,rk3588-cru.h}`.

Mesa cross-references come from `/home/e3xp0/Projects/panfrost-win/references/mesa`, which another researcher owns.
I only read it. It is at commit `b39d00df594f` (2026-10-29).

Legend: **[V]** means I verified it by reading code or data in this session. **[I]** means inference or
estimate. **[?]** means it is open or uncertain.

---

## 1. panthor architecture, file by file

All paths are under `drivers/gpu/drm/panthor/`. Every file carries `SPDX-License-Identifier: GPL-2.0 or MIT` **[V]**.
That includes the 2025/2026 ARM Ltd additions (`*_regs.h`, `panthor_hw.c`, `panthor_pwr.c`).
The uAPI header `include/uapi/drm/panthor_drm.h` is **MIT** **[V]** and has 1329 lines.
Copyright holders are mostly Collabora (2023), ARM Ltd (2025/2026), Linaro/Rob Herring (2019, inherited from
panfrost code), Marty Plummer (2018) and Amazon (2025) **[V]**.

| File | Lines | Role |
|---|---:|---|
| panthor_drv.c | 1953 | DRM driver glue. Holds the ioctl table (lines 1648-1667), the mmap of BOs and of the user MMIO page, fdinfo, platform probe, and the OF match (`rockchip,rk3588-mali`, `arm,mali-valhall-csf`, `mediatek,mt8196-mali`, l.1875-1883). Driver version 1.9 (l.1800). Features are `DRIVER_RENDER|GEM|SYNCOBJ|SYNCOBJ_TIMELINE` (l.1790). |
| panthor_device.c/.h | 613 / 699 | Device init order, PM runtime suspend/resume, reset work, the dummy flush-id page, and the user-MMIO fault handler. The `.h` file holds `struct panthor_device`, the `panthor_irq` helper (raw/threaded IRQ pattern, l.560-640) and the register accessors. |
| panthor_hw.c/.h | 318 / 64 | Reads GPU_ID and features, names the product (Mali-G610 = PROD_ID(10,7), l.~137), and dispatches per arch: v10-13 use the gpu.c power paths, v14 uses pwr.c (l.31-58). |
| panthor_gpu.c/.h | 510 / 63 | GPU_CONTROL block: L2/shader power on/off, soft reset, cache flush command, GPU IRQ (faults, reset done, power changed, clean-caches done), timestamp/cycle counters, coherency selection. |
| panthor_gpu_regs.h | 119 | GPU_CONTROL register map. |
| panthor_pwr.c/.h + pwr_regs.h | 584 / 23 / 79 | New PWR_CONTROL block for **arch >= 14 only**. It is **not needed for G610 (arch 10)**. |
| panthor_mmu.c/.h | 3516 / 113 | VM objects built on `drm_gpuvm`, the VM_BIND state machine (map/remap/unmap with huge-page split handling), the async VM_BIND queue (a drm_sched entity per VM), AS slot LRU, AS programming, lock/flush regions, MMU fault IRQ, the BO eviction/reclaim hooks, and the io-pgtable glue. |
| panthor_mmu_regs.h | 69 | MMU_INT (0x2000) and the AS registers (0x2400 + 64*as). |
| panthor_fw.c/.h | 1565 / 532 | CSF firmware loader (`mali_csffw.bin` parser), MCU VM setup, MCU boot, global/CSG/CS interface discovery, req/ack helpers, doorbells, watchdog ping, halt/reset handling. The `.h` file has all the FW-shared interface structs. |
| panthor_fw_regs.h | 29 | MCU_CONTROL (0x700), JOB_INT (0x1000), LATEST_FLUSH_ID (0x10000), doorbells (0x80000 + i*0x10000). |
| panthor_sched.c/.h | 4259 / 63 | CSF scheduler: groups and queues, CSG slot (re)assignment through a tick, priority rotation, suspend/resume of groups, idle/sync-wait handling, job ring-buffer emission, completion fences, timeouts, tiler OOM, fatal/fault handling, fdinfo. |
| panthor_heap.c/.h | 632 / 41 | Tiler heap contexts and chunk pools, grown on FW TILER_OOM events. |
| panthor_gem.c/.h | 1768 / 334 | BO implementation. It no longer uses `drm_gem_shmem` (it has its own pages, sgt, vmap, a shrinker and LRU eviction), plus kernel BOs (`panthor_kernel_bo`), dma-buf import/export and labels. |
| panthor_devfreq.c/.h | 334 / 23 | devfreq simple_ondemand, OPP table, the `mali` regulator via OPP and the optional `sram` supply. |
| panthor_trace.h | 124 | Tracepoints. |
| Kconfig/Makefile | 22 / 18 | |
| **Total .c/.h** | **18,438** | |

Licensing consequences:
- **[V]** panthor is dual GPL-2.0/MIT. A Windows port can be released under MIT, as long as the copyright headers are kept.
- **[V]** `io-pgtable-arm.c` (1301 lines) is **GPL-2.0-only**. We cannot copy it into an MIT driver.
  That is harmless: the format is the standard AArch64 VMSA stage-1 4 KB-granule format, fully documented in the Arm ARM. A clean reimplementation is about 400-600 lines **[I]**.
- **[V]** panfrost (7,927 lines) and the Rockchip pmdomain driver are GPL-2.0-only.
- Licences of the DRM core helpers panthor relies on (only the first lines read via kernel.org): `drm_gpuvm.c` is GPL-2.0-only OR MIT, `sched_main.c` is MIT-style (AMD), `drm_syncobj.c` is MIT-style (Red Hat), `dma-resv.c` is MIT, `drm_exec.c` is GPL-2.0 OR MIT, `drm_mm.c`/`drm_gem.c` are MIT-style, and `dma-fence.c` is GPL-2.0-only **[V, header lines only]**. We should not port these anyway. Windows-native equivalents are simpler.

Device init order (`panthor_device_init`, panthor_device.c:166-300) **[V]**:
1. clocks: `core` (required), `stacks` and `coregroup` (optional)
2. power domains (`devm_pm_domain_attach_list`)
3. devfreq, including the OPP and regulators
4. ioremap of reg[0]
5. pm_runtime resume, which runs `panthor_device_resume` and enables the 3 clocks
6. `panthor_hw_init` (GPU_ID and features)
7. `panthor_pwr_init` (a no-op on v10)
8. `panthor_gpu_init` (IRQ "gpu", soft reset)
9. `coherency_init`
10. `gem_shrinker_init`
11. `panthor_mmu_init` (IRQ "mmu")
12. `panthor_fw_init` (IRQ "job", L2 power-on, MCU VM, load FW, activate AS0, boot MCU, parse interfaces, global init)
13. `panthor_sched_init`
14. `drm_dev_register`
15. autosuspend set to 50 ms

---

## 2. Linux kernel APIs panthor depends on, and their Windows mapping

The usage tally comes from grepping `*.c` **[V]**. The counts are call sites. The mapping column is **[I]**.

| Linux API | Use in panthor | Windows kernel (KMDF on ARM64) equivalent |
|---|---|---|
| platform_device, `devm_platform_get_and_ioremap_resource`, `platform_get_irq_byname` | MMIO window and 3 named IRQs | ACPI `_CRS` gives `CmResourceTypeMemory` and `CmResourceTypeInterrupt` in `EvtDevicePrepareHardware`. Map with `MmMapIoSpaceEx(PAGE_READWRITE\|PAGE_NOCACHE)`. Name the IRQs by order (job, mmu, gpu). |
| `request_threaded_irq` + the `panthor_irq` raw/threaded split | Raw handler masks INT_MASK and wakes a thread, which loops on RAWSTAT | `WdfInterruptCreate` with an ISR that reads INT_STAT, writes INT_MASK=0 and queues a passive-level work item (`WdfInterruptQueueWorkItemForIsr`) or a DPC. This maps one-to-one. |
| readl/writel, `read_poll_timeout(_atomic)` | Register access and polling | `READ_REGISTER_ULONG` / `WRITE_REGISTER_ULONG`, `KeStallExecutionProcessor`, `KeQueryInterruptTime` loops. |
| clk (`devm_clk_get`, `clk_prepare_enable`, `clk_get_rate`) | 3 GPU clocks | **No Windows equivalent on Rockchip.** Either firmware leaves them on, or the driver pokes CRU registers (0xFD7C0000) directly, or uses SCMI through SMC (see §5). Windows PEP/PoFx is the "proper" route but there is no Rockchip PEP. |
| regulator / OPP (`devm_pm_opp_set_regulators`, `dev_pm_opp_set_rate`) | DVFS of vdd_gpu (RK806 DCDC1 on SPI) | Skip for the MVP: run at a fixed boot frequency and voltage. Later options are an ACPI method (`_DSM`), a PEP, or a small SPI driver for the PMIC. |
| devfreq, devfreq_cooling, thermal | Utilisation-based DVFS | Skip for the MVP. Later: PoFx performance states. |
| pm_domain (genpd), pm_runtime, autosuspend | Power domain PD_GPU and runtime suspend | MVP: keep powered, with no runtime PM. Later: `WdfDeviceAssignS0IdleSettings` plus direct PMU register access, or an ACPI `_PS0`/`_PS3` in firmware (the right place). |
| `request_firmware` | Load `arm/mali/arch10.8/mali_csffw.bin` | Read the file from the driver store (`ZwCreateFile`/`ZwReadFile` on `\SystemRoot\System32\drivers\...` or the DriverStore path from `IoGetDriverDirectory`). Alternatively embed it as a resource, which the redistributable licence allows (see §3.1). |
| alloc_page / shmem / sg_table / `dma_map_sgtable` / `dma_sync_*` | BO backing and cache maintenance (the GPU on RK3588 is **non-coherent**) | `MmAllocatePagesForMdlEx` (or `MmAllocateContiguousNodeMemory` for page tables and FW). Physical addresses come from the MDL PFN array, because the GPU has its own MMU and there is no SMMU in front of it on RK3588 **[I]**. Cache maintenance uses ARM64 `DC CVAC`/`DC CIVAC` loops in kernel mode, or WC/uncached mappings (`MmWriteCombined`). **[?]** Whether `MmNonCached` on ARM64 is Device memory or Normal-NC must be checked: it must not be Device memory for user-visible BOs. |
| vmap / iosys-map | Kernel CPU mappings of BOs (FW sections, ring buffers, syncobjs) | `MmMapLockedPagesSpecifyCache(KernelMode, MmWriteCombined or MmCached)`. |
| mmap of BOs and user MMIO (`vm_fault`, `unmap_mapping_range`) | User mapping of BOs, and of the LATEST_FLUSH_ID page (read-only), swapped for a dummy page while suspended | Map into the calling process with `MmMapLockedPagesSpecifyCache(UserMode, …)` from the ioctl, which returns the VA instead of an offset. For the MMIO page, build an MDL for the IO page (`MmBuildMdlForNonPagedPool` over an `MmMapIoSpace` mapping) and map it read-only for the user. Hot-swapping the dummy page on suspend is harder on Windows, so the MVP stays powered. |
| io-pgtable (`alloc_io_pgtable_ops(ARM_64_LPAE_S1)`) | GPU page tables (4K + 2M, `SZ_4K\|SZ_2M`, mmu.c:3379) | Write our own AArch64 stage-1 table walker (MIT). The MAIR-to-memattr conversion is in mmu.c:2178-2217. |
| drm_gpuvm / drm_gpuva / drm_mm / drm_exec | VA-space bookkeeping, map/remap/unmap splitting, multi-BO locking | MVP: a simple interval tree or RTL AVL table of mappings per VM. Only UMD-chosen VAs (panvk picks VAs itself) plus a kernel auto-VA allocator for kernel BOs. No remap splitting is needed for MVP if partial unmaps are rejected, but panvk sparse binding needs it later. |
| drm_sched (job queues, deps, timeouts) | One entity per queue and one per VM_BIND queue | A Windows work queue per queue: wait on in-fences (`KeWaitForMultipleObjects` / callbacks), then emit to the ring buffer. The ring buffer and FW do the real scheduling, so the CPU-side scheduler is thin. |
| dma_fence, dma_resv, drm_syncobj (binary + timeline) | Out-fences per job, implicit sync, user syncobjs | A custom kernel fence object (seqno + KEVENT/notification list) and a handle table per open file. Timeline syncobj = an ordered list of (point, fence). This is a must for panvk (see §4). |
| dma-buf / prime | Export/import BOs and sync files | Windows has no dma-buf. Use NT handles (section objects or a duplicated handle to our own object), or skip for the MVP. WSI needs a present path anyway (see risks). |
| xarray, idr, kref, list, rwsem, mutex, spinlock, atomic | Handle tables and refcounts | RTL generic tables / simple arrays, `InterlockedIncrement`, `KGUARDED_MUTEX`/`FAST_MUTEX`, `ERESOURCE`, `KSPIN_LOCK`. |
| workqueues (`alloc_workqueue`, `queue_work`, `queue_delayed_work`: 18+19+5 sites) | Tick work, sync update, reset, FW ping, OOM, group release | `IoQueueWorkItem` / `WdfWorkItem`, and `KTIMER`+DPC for the delayed ones (`ExAllocateTimer`). |
| wait_event_timeout / completion | FW boot wait, req/ack waits, soft reset | `KEVENT` + `KeWaitForSingleObject` with a timeout. |
| `arch_timer` (`asm/arch_timer.h`), ktime | CPU/GPU timestamp correlation | `CNTVCT_EL0` via `_ReadStatusReg(ARM64_CNTVCT)`, `KeQueryPerformanceCounter`. |
| nvmem | Leakage cell for OPP (rk3588 `gpu_leakage@1b`) | Skip. |
| debugfs, tracepoints, fdinfo, shrinker | Diagnostics and memory reclaim | ETW/WPP for logs. No shrinker for the MVP (BOs stay pinned). |

---

## 3. Minimal subset: boot CSF FW plus one compute and one render job

### 3.1 Firmware binary **[V]**
- Path: `arm/mali/arch<maj>.<min>/mali_csffw.bin`, built at panthor_fw.c:811. G610 on RK3588 is arch **10.8**.
  `MODULE_FIRMWARE` lists arch10.8, 10.10, 10.12, 11.8, 12.8, 13.8 and 14.8 (fw.c:1559-1565).
- I downloaded `linux-firmware/arm/mali/arch10.8/mali_csffw.bin` to the scratchpad. It is **282,624 bytes**.
  - Header: magic `0xc3f13a6e`, version 0.3, hash `0x1050000`, header `size` 792. The entries are within those first 792 bytes and the section data follows.
  - It has **8 IFACE sections**: MCU VA 0x0-0x10000, 0x400000-0x420000, 0x800000-0x820000 (code, flag EX), 0x1000000/0x2000000 (+0x40000 each), 0x3000000 (0x40000, zero data), and the **shared region 0x4000000-0x4010000** (flags 0xc000001b = SHARED|ZERO|cached|RW).
  - Other entries: CONFIG ("Log verbosity", "WA_CFG0"), TRACE_BUFFER (fwutf, firmware trace, fwlog, benchmark, timeline, debug monitor in/out, gpu_metrics), BUILD_INFO, plus types 4/5/7/8/9.
  - My parser: a 30-line Python script following `panthor_fw_binary_hdr` and `panthor_fw_binary_section_entry_hdr` (fw.c:40-200).
- Licence: `LICENSES/LICENCE.mali_csffw`, an Arm EULA.
  - It allows use, copying and **redistribution of the whole Software** "standalone and/or as incorporated into Applications", provided the copyright notices and the licence are kept and Arm marks are not used for marketing.
  - "Applications" means use "solely in conjunction with Mali-based products".
  - I saw **no OS restriction**, so shipping it in a Windows driver package looks permitted **[V text / I legal reading, not legal advice]**.

### 3.2 Hardware bring-up sequence (G610, arch 10, no PWR block)
Register offsets are from `panthor_gpu_regs.h`, `panthor_mmu_regs.h` and `panthor_fw_regs.h` **[V]**.

0. Pre-requisites outside the GPU block (see §5): VD_GPU supply on, PD_GPU powered and its bus not idle, clocks running.
1. Read `GPU_ID` (0x0). Expect arch_major=10 and prod_major=7. Read the feature registers:
   - L2/CORE/TILER/MEM/MMU features at 0x4-0x14
   - `AS_PRESENT` (0x18), `CSF_ID` (0x1C)
   - THREAD_* (0xA0-0xAC), TEXTURE_FEATURES (0xB0+)
   - SHADER_PRESENT (0x100), TILER_PRESENT (0x110), L2_PRESENT (0x120)
   - COHERENCY_FEATURES (0x300)
2. Soft reset (gpu.c:402-445):
   - Clear `GPU_IRQ_RESET_COMPLETED` (bit 8) in GPU_INT (base 0x20; RAWSTAT +0, CLEAR +4, MASK +8, STAT +0xC).
   - Write `GPU_CMD`(0x30) = `GPU_SOFT_RESET` (0x101).
   - Wait up to 100 ms for the IRQ or RAWSTAT bit.
3. Write `GPU_COHERENCY_PROTOCOL` (0x304) = 31 (NONE). RK3588 is not dma-coherent. ASN hash is only for MT8196, so skip it.
4. L2 power-on (gpu.c:259-323):
   - Wait for `L2_PWRTRANS` (0x220) to clear.
   - Write `L2_PWRON` (0x1A0) = 1 (64-bit).
   - Poll `L2_READY` (0x160), with a 20 ms timeout.
   - The shader cores are **not** powered by the driver. The FW powers them using `core_en_mask`.
5. MMU interrupts: enable fault bits 0-15 in MMU_INT (0x2000). Bits 16-31 are REQ_COMPLETE.
6. MCU VM (fw.c:1525):
   - Create a page table for VA 0..4 GB. The kernel area for auto-allocation is 0x4000000 + 0x4000000.
   - Allocate a backing BO for each IFACE section at its exact MCU VA.
   - Copy the data and zero the remainder. Honour RD/WR/EX: no-WR means read-only, no-EX means NOEXEC, and a non-CACHED mode means uncached.
   - Map the SHARED section into the kernel CPU address space.
   - Clean the dcache to the PoC (`dma_sync_sgtable_for_device`).
7. Activate AS0 for the MCU VM (`panthor_vm_active`, mmu.c:709-830; `as_enable`, mmu.c:604-617):
   - `AS_TRANSTAB(0)` (0x2400) = root table PA (TTBR)
   - `AS_MEMATTR(0)` (0x2408) = `mair_to_memattr(MAIR)`
   - `AS_TRANSCFG(0)` (0x2430) = `PTW_MEMATTR_WB (2<<24) | PTW_RA (bit30) | ADRMODE_AARCH64_4K (6) | INA_BITS(55-va_bits)<<6`. Add `PTW_SH_OS` only when coherent.
   - `AS_COMMAND(0)` (0x2418) = UPDATE (1). Poll `AS_STATUS` (0x2428) bit0 (AS_ACTIVE) until it clears, with a 100 ms timeout.
8. MCU boot (fw.c:1132-1165):
   - Enable all JOB_INT (0x1000) events.
   - Write `MCU_CONTROL` (0x700) = AUTO (2).
   - Wait up to 1 s for the `JOB_INT_GLOBAL_IF` (bit 31) IRQ.
   - On timeout, read `MCU_STATUS` (0x704): 0=disabled, 1=enabled, 2=halt, 3=fatal.
9. Interface discovery (fw.c:1010-1059):
   - The shared section at MCU VA 0x4000000 starts with `panthor_fw_global_control_iface {version, features, input_va, output_va, group_num, group_stride, perfcnt_size, instr_features}`. A version of 0 means the boot failed.
   - `group_num` must be in [3, 31] (`MIN_CSGS`, `MAX_CSGS`).
   - Each CSG control block is at shared + 0x1000 + i*group_stride (`CSF_GROUP_CONTROL_OFFSET`), as `{features, input_va, output_va, suspend_size, protm_suspend_size, stream_num, stream_stride}`.
   - Each CS control block is at CSG + 0x40 + j*stream_stride.
   - `input_va`/`output_va` are MCU VAs inside the shared section, translated by `iface_fw_to_cpu_addr`.
10. Global init (fw.c:1061-1096):
    - Set `core_en_mask` = SHADER_PRESENT, plus `poweroff_timer`, `progress_timer` and `idle_timer`.
    - Set `ack_irq_mask` = CFG_ALLOC_EN|PING|CFG_PROGRESS_TIMER|CFG_POWEROFF_TIMER|IDLE_EN|IDLE.
    - Set IDLE_EN|COUNTER_EN in `req`, and **toggle** CFG_ALLOC_EN|CFG_POWEROFF_TIMER|CFG_PROGRESS_TIMER (a request is "pending" while `req ^ ack` has the bit set).
    - Ring the global doorbell: write 1 to `CSF_DOORBELL(0)` = 0x80000.
    - A watchdog then pings the FW periodically (GLB_PING). In the MVP the ping can be omitted or kept, since it is trivial.

### 3.3 Running a job: one group, one CSG slot, no rotation
Source: sched.c **[V]**.
- Per queue, allocate kernel BOs:
  - a ring buffer (size from the uAPI, a power of 2)
  - a ringbuf input/output iface page (`insert`/`extract`)
  - a slot in the group's `syncobjs` BO (`panthor_syncobj_64b {u64 seqno; u32 status; pad}`)
  - a CSG suspend buffer of `csg_iface->control->suspend_size`. It must be mapped in the **group's VM**, because the FW writes it on suspend.
- Bind the group to CSG slot k (`group_bind_locked`, sched.c:1911). Doorbell id = k+1 (l.1936).
- `cs_slot_prog_locked` (sched.c:1108):
  - `ringbuf_base`, `ringbuf_size`, `ringbuf_input`, `ringbuf_output` (as MCU VAs)
  - `config` = PRIORITY | DOORBELL(id), `ack_irq_mask` = ~0
  - req: set STATE_START plus IDLE_SYNC_WAIT|IDLE_EMPTY
- `csg_slot_prog_locked` (sched.c:1339):
  - `allow_compute/fragment/other` core masks
  - `endpoint_req` (max cores, priority)
  - `config` = AS id of the group's VM (that VM must be active in an AS slot, AS1..n)
  - `suspend_buf`, `ack_irq_mask` = ~0
  - toggle `doorbell_req` for the queues
  - Then set the CSG req `STATE_START` and ring the CSG doorbells. This is done by toggling the bit in the global iface `doorbell_req` and ringing doorbell 0. Wait for the CSG ack (`csgs_upd_ctx_apply_locked`, sched.c:2028).
- Submit (`queue_run_job`, sched.c:3290-3390). Write a kernel-generated instruction sequence into the ring buffer (`prepare_job_instrs`, sched.c:3196):
  1. MOV32 + FLUSH_CACHE2 (skipped when `latest_flush` shows a flush already happened)
  2. MOV48 / MOV32 with the stream address and size, WAIT, then **CALL** into the user's command stream (`stream_addr`/`stream_size` from `drm_panthor_queue_submit`)
  3. MOV48 sync_addr, MOV48 #1, WAIT(all), **SYNC_ADD64** (increments the queue's syncobj seqno, system scope, propagating errors), ERROR_BARRIER
  
  This is padded to a 64-byte cache line. Then `wmb`, set `input->insert`, and ring the queue doorbell (0x80000 + id*0x10000).
- Completion: the FW raises JOB_INT bit k (CSG k) and sets CSG `SYNC_UPDATE`. The driver reads `syncobjs[queue].seqno` and signals all jobs with seqno <= value (`queue_check_job_completion`, sched.c:1682).
- **Compute job**: panvk's command stream does everything itself (RUN_COMPUTE), so the kernel only needs the above.
- **Render job** additionally needs a **tiler heap** (`TILER_HEAP_CREATE`, heap.c:274):
  - The kernel allocates a heap context, placed in a per-VM heap-context pool BO, plus `initial_chunk_count` chunks of `chunk_size`, linked as a list. Each chunk header holds the next-chunk pointer and size.
  - It returns `tiler_heap_ctx_gpu_va` and `first_heap_chunk_gpu_va`, which panvk writes into its tiler descriptors.
  - If the tiler runs out of memory, the FW raises CS `TILER_OOM` (bit 26). The driver reads `heap_address`, `heap_vt_start/end` and `heap_frag_end` from the CS output, calls `panthor_heap_grow`, writes `heap_start`/`heap_end` = new chunk VA, acks, and rings the CSG doorbell (sched.c:1495-1582).
  - For an MVP, a large enough initial heap (for example 16 chunks of 2 MB) can avoid OOM for a triangle. It is still better to implement grow, because it is only about 150 lines.
- Fault paths that are still worth having in the MVP for debuggability:
  - MMU fault IRQ: read `AS_FAULTSTATUS`/`AS_FAULTADDRESS`
  - CS FAULT/FATAL (exception type/data)
  - GPU_FAULT_STATUS
  
  These logs are the only window into the hardware.
- Not needed for the MVP:
  - group rotation/suspend/resume across CSG slots
  - priorities and the tick
  - runtime PM, devfreq
  - reset/recovery, beyond "reload driver"
  - protected mode, perf counters, profiling instructions
  - shrinker/eviction, dma-buf, sparse binding, async VM_BIND (if panvk can be forced into immediate binds, see §4)

### 3.4 MMU details for a reimplementation **[V]**
- Format: AArch64 VMSA stage-1, 4 KB granule, page sizes 4K and 2M (mmu.c:3379). IAS comes from `MMU_FEATURES[7:0]` (VA bits) and OAS from `[15:8]`.
  - On G610 the VA bits are probably 48 **[?]**, to be confirmed on hardware.
  - Note `INA_BITS(55 - va_bits)`.
  - panfrost on older GPUs uses `ARM_MALI_LPAE` (a different, Mali-specific format). Bifrost G52 has `HW_FEATURE_AARCH64_MMU` (panfrost_features.h:73-83), so **both targets can use standard AArch64 tables**.
- Page-table updates on a live AS:
  1. `AS_LOCKADDR` (0x2410) = `pack_region_range()`, which is a naturally aligned power-of-2 region encoded as log2(size)-1 with a minimum of 32 KB (mmu.c:570-597)
  2. `AS_COMMAND` = LOCK (2)
  3. modify the PTEs
  4. GPU cache flush: `GPU_CMD` = `FLUSH_CACHES(l2=CLEAN|INV, lsc=CLEAN|INV, other=INV)` and wait for the `CLEAN_CACHES_COMPLETED` IRQ (bit 17)
  5. `AS_COMMAND` = UNLOCK (3)
  
  The source is mmu.c:1776-1856.
- Non-coherent page-table walk: tables must be cleaned to the PoC after each write. Linux does this with `coherent_walk=false` in io-pgtable, which calls `dma_sync_single_for_device` on the PTE pages.

### 3.5 LOC estimate for a Windows-native minimal port **[I]**
These are rough C line counts, excluding comments, based on the sizes of the corresponding panthor code paths.

| Component | Est. LOC |
|---|---:|
| KMDF skeleton, ACPI resources, MMIO, 3 interrupts plus work items | 500-800 |
| SoC power/clock poke (PMU PD_GPU, CRU gates), if firmware does not do it | 200-400 |
| GPU block: id/features, soft reset, L2 power, cache flush, GPU IRQ | 350-500 |
| AArch64 page-table code (map/unmap 4K/2M, cache clean), AS management, lock/flush, fault IRQ | 900-1300 |
| FW loader, MCU VM, boot, iface parsing, global init, req/ack helpers, doorbells | 700-1000 |
| Minimal scheduler: group/queue create, bind to a fixed CSG slot, ring-buffer emission, completion, CS fault/OOM events | 1200-1800 |
| Tiler heap create/destroy/grow | 400-600 |
| BO manager: MDL allocation, kernel/user mapping, GPU VA map, BO_SYNC with DC CIVAC | 600-900 |
| Fences plus binary/timeline syncobjs, handle tables, wait ioctl | 700-1100 |
| Ioctl dispatch, panthor-compatible structs and validation (DEV_QUERY etc.) | 600-900 |
| **Total** | **~6,000-9,300** (vs 18.4k lines in panthor) |

The full, production-equivalent port would be about 15-20k lines, which matches panthor. The extra is group rotation, PM, reset, eviction, async binds and dma-buf.

---

## 4. Full panthor uAPI, and what panvk uses

Ioctls (panthor_drm.h:85-160 and 1284-1322; all `DRM_IOWR`, base `DRM_COMMAND_BASE` 0x40) **[V]**:

| # | Ioctl | Struct (fields) | Used by Mesa panthor_kmod / panvk? |
|---|---|---|---|
| 0 | DEV_QUERY | `{u32 type, u32 size, u64 pointer}`. Types: GPU_INFO, CSIF_INFO, TIMESTAMP_INFO, GROUP_PRIORITIES_INFO, MMU_INFO | **Yes**, all 5 types (panthor_kmod.c:244-319, 1445) |
| 1 | VM_CREATE | `{u32 flags, u32 id(out), u64 user_va_range}` | **Yes** |
| 2 | VM_DESTROY | `{u32 id, pad}` | **Yes** |
| 3 | VM_BIND | `{u32 vm_id, u32 flags(ASYNC), obj_array ops}`. Each op is `{u32 flags, u32 bo_handle, u64 bo_offset, u64 va, u64 size, obj_array syncs}`. Op flags: MAP_READONLY, NOEXEC, UNCACHED, **SPARSE**. Types: MAP, UNMAP, SYNC_ONLY | **Yes**, with ASYNC when the mode is not IMMEDIATE (panthor_kmod.c:1190) and all op types/flags (l.1266-1304) |
| 4 | VM_GET_STATE | `{vm_id, state}` | Yes |
| 5 | BO_CREATE | `{u64 size, u32 flags(NO_MMAP, WB_MMAP), u32 exclusive_vm_id, u32 handle(out), pad}` | **Yes** |
| 6 | BO_MMAP_OFFSET | `{handle, pad, u64 offset(out)}` | **Yes**. It is followed by `mmap(fd, offset)` |
| 7 | GROUP_CREATE | `{obj_array queues{u8 priority, u32 ringbuf_size}, u8 max_compute/fragment/tiler_cores, u8 priority, u64 compute/fragment/tiler_core_mask, u32 vm_id, u32 group_handle(out)}` | **Yes** (csf/panvk_vX_gpu_queue.c:686) |
| 8 | GROUP_DESTROY | `{group_handle}` | Yes (:704) |
| 9 | GROUP_SUBMIT | `{group_handle, obj_array queue_submits}`. Each is `{queue_index, stream_size, u64 stream_addr, latest_flush, obj_array syncs}`. A sync_op is `{u32 flags(SYNCOBJ/TIMELINE_SYNCOBJ, WAIT/SIGNAL), u32 handle, u64 timeline_value}` | **Yes** (:557, :1230) |
| 10 | GROUP_GET_STATE | `{handle, state(TIMEDOUT, FATAL_FAULT, INNOCENT), fatal_queues}` | Yes (:1514) |
| 11 | TILER_HEAP_CREATE | `{vm_id, initial_chunk_count, chunk_size, max_chunks, target_in_flight, handle(out), u64 tiler_heap_ctx_gpu_va(out), u64 first_heap_chunk_gpu_va(out)}` | **Yes** (:756) |
| 12 | TILER_HEAP_DESTROY | `{handle}` | Yes (:791) |
| 13 | BO_SET_LABEL | `{handle, u64 label(char*)}` | Yes (debug only, can be a no-op) |
| 14 | SET_USER_MMIO_OFFSET | `{u64 offset}` | Yes (can be a no-op on Windows) |
| 15 | BO_SYNC | `obj_array ops{handle, type(FLUSH, FLUSH_AND_INVALIDATE), u64 offset, u64 size}` | **Yes**, needed for WB_MMAP BOs on non-coherent RK3588 (panthor_kmod.c:746) |
| 16 | BO_QUERY_INFO | `{handle, extra_flags(IS_IMPORTED), create_flags}` | Yes (imports) |

Plus a user MMIO mapping at `DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET` (1<<56 on 64-bit; 1<<43 for 32-bit), which is one read-only page holding `CSF_GPU_LATEST_FLUSH_ID`. Mesa mmaps it (panthor_kmod.c:310).

Generic DRM and libdrm calls used by the panvk CSF path plus kmod **[V]**:
- `drmSyncobjCreate`, `Destroy`, `Reset`, `TimelineWait`, `Transfer`, `ExportSyncFile`, `ImportSyncFile`
- the `vk_drm_syncobj` runtime: CREATE, DESTROY, WAIT, RESET, SIGNAL, TRANSFER, QUERY, FD_TO_HANDLE, HANDLE_TO_FD (`src/vulkan/runtime/vk_drm_syncobj.c`)
- `drmPrimeHandleToFD` and `drmPrimeFDToHandle`
- `drmCloseBufferHandle` (GEM_CLOSE)
- `drmGetDevice2`/`drmFreeDevice` (WSI)
- `drmGetVersion`-style backend detection by name: `panfrost`, `panthor`, `panfrost_virtio_gpu`, `panfrost_vpipe` (pan_kmod.c:22-44)

Note the newer Mesa abstraction `struct util_sync_provider *sync_ops` (pan_kmod.h:596, 1045-1056): panvk's syncobj calls go through a pluggable provider. That is a natural seam for a Windows backend. There is also `HAVE_PANFROST_VDRM` with **vpipe** and **virtio_gpu** backends, a remoted-DRM transport the mesa researcher should evaluate **[V existence / I usefulness]**.

**Conclusion [I]**: panvk uses essentially **all 17** panthor ioctls plus the syncobj family. A Windows kernel interface should mirror panthor's structs 1:1 (they are MIT and versioned via `obj_array.stride`). The handle-based syncobj API should be kept, but Linux fds (sync_file/prime) replaced by NT handles. The minimum to get vkcube-like rendering: DEV_QUERY, VM_CREATE/BIND (sync mode), BO_CREATE/MMAP/SYNC, GROUP_CREATE/SUBMIT/DESTROY/GET_STATE, TILER_HEAP_CREATE/DESTROY, syncobj create/destroy/wait/reset/signal/timeline-wait/transfer, and GEM_CLOSE. Sparse/async binds and import/export can come later.

---

## 5. RK3588 GPU device-tree facts, and what must happen before registers are readable

From `arch/arm64/boot/dts/rockchip/rk3588-base.dtsi:449-466` **[V]**:

```
gpu@fb000000 { compatible = "rockchip,rk3588-mali", "arm,mali-valhall-csf";
  reg = <0x0 0xfb000000 0x0 0x200000>;                 // 2 MB MMIO
  assigned-clocks = <&scmi_clk SCMI_CLK_GPU>; assigned-clock-rates = <200000000>;
  clocks = <&cru CLK_GPU>, <&cru CLK_GPU_COREGROUP>, <&cru CLK_GPU_STACKS>;   // ids 262, 263, 264
  clock-names = "core", "coregroup", "stacks";
  interrupts = <GIC_SPI 92 ...LEVEL_HIGH>, <GIC_SPI 93 ...>, <GIC_SPI 94 ...>;  // GIC INTID 124/125/126
  interrupt-names = "job", "mmu", "gpu";
  power-domains = <&power RK3588_PD_GPU>;            // PD id 12
  dynamic-power-coefficient = <2982>; }
```

- MMIO layout inside the 2 MB window **[V]**:
  - GPU_CONTROL at 0x0
  - MCU_CONTROL at 0x700
  - JOB_INT at 0x1000
  - MMU_INT at 0x2000
  - AS regs at 0x2400 + as*0x40
  - LATEST_FLUSH_ID at 0x10000
  - doorbells at 0x80000 + n*0x10000, so 24 doorbell pages fit in 2 MB (0 is global, n+1 per CSG)
- OPPs (`rk3588-opp.dtsi:118-153`) **[V]**:

  | Frequency (MHz) | Voltage (µV) |
  |---|---|
  | 300, 400, 500, 600 | 675000 |
  | 700 | 700000 |
  | 800 | 750000 |
  | 900 | 800000 |
  | 1000 | 850000 (max 850000) |

  The DT boots the clock at 200 MHz.
- Regulator (Rock 5B, `rk3588-rock-5b-5bp-5t.dtsi:180-183, 506, 703-714`) **[V]**:
  - `mali-supply = <&vdd_gpu_s0>`, and `pd_gpu` has `domain-supply = <&vdd_gpu_s0>`.
  - `vdd_gpu_s0` is **RK806 DCDC1 on SPI2** (`regulator-boot-on`, 550-950 mV, ramp 12500 µV/µs, enable delay 400 µs). It is not `always-on`.
  - The same `domain-supply` pattern is in about 16 other RK3588 board dtsi files.
- Power domain (`pm-domains.c:1339`, `rk3588_pmu` at l.1555-1568; PMU at 0xFD8D8000) **[V]**:
  - `DOMAIN_RK3588("gpu", p_offset 0x0, pwr BIT(0), status 0, m_offset 0x0, m_status 0, r_status BIT(1), r_offset 0x0, req BIT(0), idle BIT(0), wakeup false, need_regulator true)`
  - Registers (base + offset): `pwr` at 0x14c, `status` at 0x180, `req` at 0x10c, `idle` at 0x120, `ack` at 0x118, `mem_pwr` at 0x1a0, `chain_status` at 0x1f0, `mem_status` at 0x1f8, `repair_status` at 0x290.
  - All writes use the Rockchip **hi-word write mask** (bits 31:16 enable bits 15:0).
- Power-on sequence (`rockchip_pd_power`, pm-domains.c:647-692; `rockchip_do_pmu_set_power_domain` l.597-645; `set_idle_request` l.408-455) **[V]**:
  1. Enable the regulator.
  2. Enable the PD clocks (CLK_GPU, COREGROUP, STACKS).
  3. Clear the PWR bit0 at PMU+0x14c, writing `(1<<16)|0`.
  4. Poll the power status and repair status (0x290 bit1) for up to 10 ms.
  5. Issue SMC `ROCKCHIP_SIP_SUSPEND_MODE`/`ROCKCHIP_SLEEP_PD_CONFIG` to tell TF-A about the PD state.
  6. Release the bus idle request (PMU+0x10c, writing `(1<<16)|0`).
  7. Poll ack 0x118 and idle 0x120 until clear.
  8. Restore the QoS registers (qos_gpu_m0..m3 at 0xFDF35000/5200/5400/5600).
- Clocks: CLK_GPU is a CRU (0xFD7C0000) clock, but its rate is set through **SCMI clock 5 (SCMI_CLK_GPU)** served by TF-A BL31. TF-A drives the GPU PLL/PVTPLL.
  - edk2-rk3588 defines `SCMI_CLK_GPU 5` (`edk2-rockchip/Silicon/Rockchip/RK3588/Include/ScmiDefinitions.h:18`) **[V]**.
- **edk2-rk3588 ACPI has no GPU device.** Grepping the 42 `.asl` files and the Rockchip silicon code for `FB000000`, `mali` or `gpu` found nothing except the SCMI define **[V]**. A DSDT device has to be added in firmware: `_HID`, `_CRS` with Memory32Fixed 0xFB000000/0x200000 and three level/high interrupts 124, 125 and 126, `_CCA 0`, and ideally `_PS0`/`_PS3` that do the PMU/regulator work.
- TF-A: `arm-trusted-firmware/plat/rockchip/rk3588/drivers/pmu/pmu.c` only toggles PD_GPU on system suspend/resume (l.514-515, 620, 709) **[V]**.
  - **[?]** The PD_GPU state at OS hand-off on edk2 is unknown. The reset default is probably powered on **[I]**. Linux genpd turns unused domains off late, but Windows has nothing that would.
  - **[?]** Whether vdd_gpu_s0 is actually enabled at hand-off is also unknown. It is probably on, since it is marked `regulator-boot-on` and the RK806 defaults are set by OTP **[I]**. It must be measured on hardware: a read of GPU_ID returning `0xa8?7????` (arch 10.8, prod_major 7; the other nibbles are unverified) means alive, and 0/bus hang means not.

**Before GPU_ID is readable** **[I, derived from the above]**:
1. VD_GPU rail on.
2. PD_GPU on, with memory repair done and the NIU idle request released.
3. CLK_GPU (and probably COREGROUP and STACKS) ungated at some non-zero rate.
4. Any GPU resets in the CRU deasserted. panthor uses no `reset_control` at all (grep found none), so the CRU reset defaults are presumably de-asserted.

Reading an unpowered domain on Rockchip usually **hangs the bus or SErrors**, so the driver should check the PMU status bit (PMU+0x180 bit0 == 0 means on) **before** touching 0xFB000000.

---

## 6. Would a JM (panfrost) target be simpler as a first milestone, for example RK3566/RK3568 with Mali-G52?

Facts **[V]**:
- The RK356x GPU is at `gpu@fde60000` with reg size 0x4000 and IRQs SPI 40 (job), 41 (mmu) and 39 (gpu). Its clocks are `scmi_clk SCMI_CLK_GPU` ("gpu") and `cru CLK_GPU` ("bus"), and its power domain is RK3568_PD_GPU (rk356x-base.dtsi:575-587).
- panfrost is 7,927 lines, GPL-2.0-only. G52 supports AARCH64 MMU tables.
- Job submission is direct register writes (JS_HEAD_NEXT/JS_CONFIG_NEXT/JS_COMMAND_NEXT): no firmware, no shared-memory interfaces, no ring buffer, no tiler-heap ioctl (the heap is a plain BO), and 2-3 job slots.
- **However, panvk on arch 6/7 (Bifrost, including G52) refuses to load without `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1`** (panvk_physical_device.c:454-466). Bifrost reports Vulkan 1.3 and is not conformant.
- **v10 (G610) is in the supported list and reports Vulkan 1.4, conformance version 1.4.5.2** (panvk_vX_physical_device.c:808-826).

Assessment **[I]**:
- **Kernel-only bring-up is simpler on JM**: probably 3-4k lines to first job, with no FW blob and no MCU boot that can fail invisibly.
- **The end-to-end goal, Vulkan, then DXVK/vkd3d/Zink, is much better served by CSF/G610.** It is the only Mali generation where panvk is conformant and actively the primary target.
- The 3D stack on JM would mean either a "broken" panvk or porting Gallium panfrost (GL) with a WGL winsys, which is a different and less valuable path.
- RK356x on Windows also has a less mature edk2 port than edk2-rk3588 **[?]**.
- About 40% of the kernel work is shared between the two: MMU/AS code, BO manager, sync objects, the ioctl shell and the SoC power plumbing. The CSF-specific parts are the FW loader, global/CSG/CS interfaces, the scheduler ring buffer and the tiler heap, about 3-4k lines.

**Recommendation**: go straight to RK3588/CSF. Use a staged plan so the early milestones are as observable as JM would be:
- **M0**: map MMIO, read GPU_ID/features, soft reset, L2 power-on, all from a KMDF driver against a new ACPI node.
- **M1**: GPU MMU on AS0 with a test mapping. Optionally use a GPU_CMD cache flush plus MMU fault injection to prove the page tables are right.
- **M2**: load FW and boot the MCU (GLB_IF IRQ, iface version printed, GLB_PING ack).
- **M3**: one group, one queue. Submit a hand-assembled CS that does only `SYNC_ADD64` / `STORE`, then check the syncobj in memory. This needs no shaders.
- **M4**: run panvk through the Windows kmod backend for a compute dispatch.
- **M5**: tiler heap and a render pass.

A JM/G52 port is not worth doing as a separate first milestone unless RK3588 hardware is unavailable.

---

## 7. Risks and open questions
1. **SoC power, clock and regulator ownership on Windows.** There is no Rockchip PEP, no clock framework and no SCMI client in Windows. The PMU/CRU must be driven from our driver (GPL pmdomain code cannot be copied, but the register sequence is simple) or from ACPI methods in edk2. That needs coordination with the edk2 researcher.
   - DVFS through RK806 over SPI is out of scope for the MVP, so the MVP runs at boot clock and voltage.
   - **[?]** Can a Windows ARM64 kernel driver issue SMC (SCMI/SiP)? When VBS/Hyper-V is active, EL2 is Hyper-V and arbitrary SMCs may be filtered.
2. **Cache attributes and coherency.** RK3588 GPU coherency is NONE.
   - The ARM64 Windows mapping types (`MmNonCached` vs `MmWriteCombined`) must match what Mesa assumes. panthor maps non-WB BOs write-combined for the CPU, and Mesa does unaligned accesses, so Device memory is not acceptable.
   - Page-table and FW-section cache cleaning must be explicit (DC CVAC to the PoC).
   - **[?]** Whether EL0 can do DC CIVAC on Windows (SCTLR_EL1.UCI). If not, BO_SYNC must stay a kernel ioctl, which is fine.
3. **Physical address width.** RK3588 boards with 16/32 GB have DRAM above 4 GB. The Mali MMU PA bits (MMU_FEATURES[15:8], probably 40 **[?]**) are enough. The FW sections need no low-memory constraint, because they are mapped through the MCU MMU **[I]**.
4. **No SMMU in front of the GPU** **[I]**, so the GPU MMU is the only isolation. That is fine for us, but the Windows "DMA remapping" policy may complain. It is irrelevant if the device does not declare DMA via IORT.
5. **Debuggability**: without hardware here, every register/handshake mistake shows up as a silent hang. The plan needs a hardware owner and serial/kernel debugging (KDNET over USB/Ethernet on RK3588 under Windows) **[?]**.
6. **Windows driver signing**: a test-signed KMDF driver is fine for development. There is no `libntoskrnl.a` for arm64 in llvm-mingw (per the task context), so the build has to generate an import lib from a .def file or use the WDK/MSVC. The toolchain researcher is covering this.
7. **Present/WSI path**: panvk on Windows still needs a way to put pixels on screen, such as a VOP2 display driver or a copy to the UEFI GOP framebuffer via BasicDisplay. That is outside the kernel-panthor scope, but it gates any visible demo.
8. **FW interface version drift**: panthor handles `glb_state` / 64-bit endpoint_req feature bits (fw.c:326-378). The arch10.8 blob is iface v?.?.? (printed at boot) **[?]**. Mirror these checks.

## 8. Files and paths referenced
- Kernel: `/home/e3xp0/Projects/panfrost-win/references/linux/drivers/gpu/drm/panthor/*`, `.../include/uapi/drm/panthor_drm.h`, `.../arch/arm64/boot/dts/rockchip/rk3588-base.dtsi`, `rk3588-opp.dtsi`, `rk3588-rock-5b-5bp-5t.dtsi`, `rk356x-base.dtsi`, `.../drivers/pmdomain/rockchip/pm-domains.c`, `.../drivers/gpu/drm/panfrost/*`.
- Mesa (read-only): `/home/e3xp0/Projects/panfrost-win/references/mesa/src/panfrost/lib/kmod/{pan_kmod.c,pan_kmod.h,panthor_kmod.c}`, `.../vulkan/csf/panvk_vX_gpu_queue.c`, `.../vulkan/panvk_physical_device.c`, `.../vulkan/panvk_vX_physical_device.c`.
- edk2 (read-only): `/home/e3xp0/Projects/panfrost-win/references/edk2-rk3588/edk2-rockchip/Silicon/Rockchip/RK3588/Include/{ScmiDefinitions.h,RK3588.h}`, `.../arm-trusted-firmware/plat/rockchip/rk3588/drivers/pmu/pmu.c`.
- Scratch, not in the repo: `/tmp/claude-1000/-home-e3xp0-Projects-panfrost-win/b0762636-758d-4a63-b545-586301941984/scratchpad/raw/` contains `panthor_drm.h`, `fw/mali_csffw.bin` (arch10.8), `LICENCE.mali_csffw`, `WHENCE`, and the first lines of the DRM core helper files.
