# panfrost-win: an open Mali GPU driver for Windows on Arm

Status: plan v1, 2026-10-30. M0 (hardware-free foundations) landed, see [`docs/status/`](status/). Research behind every decision here is in [`docs/research/`](research/).

## Goal

A working, open-source (MIT) GPU driver for Arm Mali on Windows on ARM64. "Port panfrost" was the
starting idea; the plan below keeps the useful part of that (reusing Mesa's Mali userspace, which
is where most of the value and code is) and drops the parts that don't transfer (Linux kernel
code, GPL code, DRM).

## Decision record

### D1. Hardware target: Rockchip RK3588 / Mali-G610 MP4 (Valhall v10, CSF)

- RK3588 boards (ROCK 5B/5B+, Orange Pi 5/5 Plus, Edge2, NanoPC-T6, ...) are the most mature
  Windows-on-Arm platform with a Mali GPU (edk2-rk3588 UEFI+ACPI, Windows 11 24H2+ boots;
  USB/eMMC/NVMe/Ethernet work). The GPU is listed as "software rendered" and nobody is working on it.
- G610 is the only Mali that Mesa's panvk calls conformant (Vulkan 1.4). Bifrost (G52) panvk
  requires `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER`; RK3576 (G52) is capped at Windows 23H2 (ARMv8.0).
- Cost: G610 is a CSF GPU, so the kernel driver has to load and talk to Arm's MCU firmware
  (`mali_csffw.bin`, redistributable alongside Mali hardware). This is more work than JM, but
  ~40% of the kernel work (MMU, BOs, sync, ioctl layer, SoC power) is shared with JM, so JM
  (G52 / RK3566 / RK3576) remains a cheap second backend later.

### D2. Architecture: thin custom kernel driver + Mesa userspace, panthor uAPI over IOCTL

```
  app (Vulkan) ─► vulkan-1.dll (Khronos loader) ─► vulkan_panfrost.dll  (Mesa panvk, CSF v10)
  app (GL)     ─► Mesa opengl32.dll (WGL + Zink) ─┘        │
  app (D3D9-11)─► DXVK / Gallium D3D UMD (later)  ─┘        │ libdrm API
                                                   libdrm-win.dll  (drmIoctl → DeviceIoControl,
                                                            │       mmap → map ioctl, syncobj)
  ═══════════════════════════ user / kernel ═══════════════╪══════════════════════════════════
                                                   panwin.sys  (KMDF)
                                                     win/   KMDF glue: ACPI resources, MMIO,
                                                            3 IRQs, IOCTL dispatch, file ctx
                                                     core/  OS-neutral Mali CSF core: GPU/MMU,
                                                            FW boot, scheduler, BOs, syncobjs
                                                            (also built on Linux host for tests)
  ═════════════════════════════════ firmware ═════════════════════════════════════════════════
                                                   edk2-rk3588 + our patch: ACPI GPU node
                                                   (RKCP7410), GPU power domain / clocks / vdd
```

Why this shape:

1. **The kernel driver speaks the panthor uAPI byte-for-byte** (same structs, same ioctl numbers
   embedded in the IOCTL code, same semantics, including DRM syncobjs with timelines). Mesa's
   `panthor_kmod.c` and the panvk CSF queue code then run unmodified, so the kernel driver is
   exercised by the same userspace as on Linux, and bugs can be cross-checked against Linux
   panthor on the same board.
2. **Not WDDM first.** A KMDF driver with private IOCTLs is enough for Vulkan: the Khronos loader
   accepts ICDs registered under `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` with no WDDM adapter, and
   Mesa's win32 WSI already has a CPU present path (GPU→host-visible copy→GDI blit). WDDM costs
   months before the first pixel; it's phase 2, and the core/ split is designed so only
   `kmd/win/` changes when it happens.
3. **Kernel code is a clean MIT reimplementation**, informed by panthor (which is "GPL-2.0 or
   MIT", so porting its logic is fine) but not a LinuxKPI port. GPL-only sources
   (io-pgtable-arm.c, panfrost, rockchip pmdomain, gahingwoo/wddm-mali-g52) are reference only.
4. **Toolchain runs on Linux**: llvm-mingw (clang) builds the ARM64 `.sys` (verified at build
   level: ARM64, native subsystem, WDM_DRIVER, `.pdata`), Mesa cross-builds with
   `-mno-ms-bitfields`. No WDK needed for phase 1 (KMDF from Microsoft's MIT headers + our stub).

### D3. API coverage path

| Stage | What users get |
|---|---|
| panvk on Windows | Vulkan 1.4 for native ARM64 apps; CPU/GDI present (slow-ish but works) |
| + Mesa opengl32.dll with Zink | app-local OpenGL (≈GL 3.1 until panvk gets GS/tess) |
| + ARM64X/ARM64EC build | x64-emulated apps can load the ICD |
| WDDM 2.x (phase 2) | proper adapter, D3DKMT path, DXGI flip present, system GL ICD, D3D via a Gallium UMD |
| DXVK / vkd3d-proton | blocked on panvk geometry + tessellation shaders (upstream work in progress) |

## Milestones

Hardware-free work (M0) can be verified here; everything from M1 needs an RK3588 board with UART.

- **M0 — foundations (no hardware).** Toolchain, repo, KMDF skeleton that loads ACPI resources,
  OS-neutral core with host unit tests against a mock register file + the real firmware binary,
  libdrm-win shim, Mesa panvk cross-compiling and linking, edk2 GPU node patch.
- **M1 — GPU alive.** edk2 powers PD_GPU; panwin.sys loads (test-signed), maps MMIO, reads
  GPU_ID (expect arch 10.8, G610), soft reset, L2 on, IRQs fire. `panwin-info` prints DEV_QUERY.
- **M2 — firmware boot.** MCU VM on AS0, FW sections mapped, MCU boots, global iface parsed,
  GLB_PING acked.
- **M3 — first job.** Kernel-written command stream (SYNC_ADD64 only) through one CSG/CS,
  completion IRQ, syncobj signalled.
- **M4 — panvk compute.** BO/VM/VM_BIND, groups/queues, GROUP_SUBMIT, syncobjs, mmap; vulkaninfo
  and a compute sample run.
- **M5 — rendering.** Tiler heap + OOM grow; vkcube via win32 WSI (CPU blit).
- **M6 — usability.** Zink GL, ARM64X ICD, installer/INF, DVFS via SCMI `_DSM`, runtime PM.
- **M7 — WDDM.** Render-only or render+display (own the UEFI framebuffer) WDDM 2.x driver,
  D3DKMT kmod backend, DXGI present, then a Gallium D3D UMD for D3D9-11/DWM.

**Hardware-free status (2026-10-30).** The kernel-side code for M3–M5 is written and passes against
the mock G610: BOs/VMs/VM_BIND (`pw_bo.c`), fences + binary/timeline syncobjs (`pw_sync.c`), the
CSF scheduler with GROUP_SUBMIT and tiler heaps (`pw_sched.c`, `pw_heap.c`), the per-file ioctl
dispatcher (`pw_file.c`, reports panthor DRM 1.6), and the Windows glue (MDL mmap, SYNCOBJ_EVENT,
errno NTSTATUS). `tests/e2e/run.sh` runs unmodified Mesa panvk → libdrm-win → the real core + a mock
command-stream executor under wine (x86_64): device creation, submit/fence, timeline semaphores,
sparse binding, 1000-submit stress, zero leaks. Shader execution is not emulated, so no rendering
results are checked. Per-piece detail and review findings: `docs/status/{core-mem,core-sync,
core-sched,glue,e2e}.md`. Known gaps before multi-app use: no CSG slot rotation (the 17th live group
waits forever), no GPU/FW reset, a timed-out CSG TERMINATE reuses the slot, MMU faults reach the
scheduler late, one GPU fault makes a VM permanently unusable (stricter than panthor), async
VM_BIND runs synchronously, DRM 1.7 (cached mappings) withheld until user-mode cache maintenance
is checked on Windows ARM64.

## Repository layout

```
docs/                 plan, research notes, bring-up guides
include/panwin/       user/kernel ABI (IOCTL encoding, private ioctls), shared by kmd + libdrm-win
include/compat/       tiny compat headers so Mesa's drm-uapi headers compile on Windows (sys/ioccom.h)
kmd/core/             OS-neutral Mali CSF core (C11, no OS headers; talks to the OS via pw_os.h)
kmd/win/              KMDF glue → panwin.sys, INF
kmd/tests/            host (Linux) unit tests for core with a mock GPU
toolchain/            ARM64 kernel headers fixups, import-lib .defs, KMDF stub, cross files, signing
userspace/libdrm-win/ libdrm-compatible DLL over DeviceIoControl
userspace/mesa/       pinned Mesa commit + patch series + cross build script
firmware/edk2-rk3588/ patches for the edk2-rk3588 firmware (ACPI GPU node, power-up)
tools/                panwin-info and other bring-up tools
```

## Risks (top 5)

1. **SoC power at handover** — touching GPU registers with PD_GPU off raises an SError. Mitigated by
   the edk2 patch; first hardware check is reading PMU `0xFD8D8290` from the UEFI shell.
2. **Blind bring-up** — every bug is a hang. Mitigations: host tests against a mock, UART + WPP/
   DbgPrint logs, Linux panthor on the same board for register traces.
3. **ARM64 kernel import list is approximate** (generated from x64+ARM32 defs). Replace with
   `gendef` output from a real ARM64 `ntoskrnl.exe` on day one of M1.
4. **Memory types / non-coherent GPU** — CPU mappings of shared memory must be Normal-NC/WC, not
   Device; atomics on uncached memory may fault. Verify `MmNonCached` vs `MmWriteCombined` on HW.
5. **panvk feature gaps** (GS/tess) cap GL and block DXVK — upstream work, outside our control.

## Contract decisions (from the M0 reviews)

Items 1–5 were settled and implemented in M3–M5 (`include/panwin/panwin_ioctl.h` ABI v2, `pw_os.h`).
SYNCOBJ_EVENT takes a caller-created event handle.

1. **IOCTL errors**: adopt `NTSTATUS = 0xE0000000 | (0xD52 << 16) | errno` for DRM ioctls, so the shim recovers
   the exact Linux errno (today ETIME and ETIMEDOUT collapse into one status). Needs kmd/win + libdrm-win changes.
2. **User MMIO offset**: Mesa on Windows computes `DRM_PANTHOR_USER_MMIO_OFFSET` as `1ull << 43` (`unsigned long`
   is 32-bit on Windows). The kernel accepts that value and the 64-bit Linux one.
3. **Syncobj timeouts** are absolute nanoseconds on the QueryPerformanceCounter clock (Mesa's `os_time` base on Windows).
4. **pw_os.h clarifications**: events are manual-reset; `0xFFFFFFFF` is an infinite wait; only the reg/udelay/time
   functions are ISR-safe; flush cleans to the PoC. Also add `timer_hz` to `pw_device_params` (today 24 MHz is hard-coded).
5. **GPU `_DSM`** (edk2 patch): UUID `102bc718-f3be-4e3c-8e12-dc78892ef9a5`, functions 1 get / 2 set / 3 max / 4 diag;
   any non-zero return from "set" is a failure. The driver checks `_PSC == 0` before touching registers.
6. **Mesa AI policy**: see [`userspace/mesa/AI-POLICY.md`](../userspace/mesa/AI-POLICY.md). Owner decision pending.
