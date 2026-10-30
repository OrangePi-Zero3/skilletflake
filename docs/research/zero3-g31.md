# Orange Pi Zero3 / Mali-G31 MP2: platform and GPU facts

Researched 2026-10-30 for the new first hardware target (D4 in the plan). Sources: halal-beef's
firmware (`github.com/OrangePi-Zero3/Mu-Silicium`, `UEFI-Builder`), its ACPI tables
(`Project-Silicium/Silicium-ACPI` at 6f911ab, `SoCs/Allwinner/H616/`), Linux
`sun50i-h616.dtsi` / `sun50i-h618-orangepi-zero3.dts` / `ccu-sun50i-h616.c` /
`sun50i-h6-prcm-ppu.c` (read for hardware facts only), Mesa at the pinned commit.

## Board and Windows

- Allwinner H618: 4x Cortex-A53 (ARMv8.0), GIC-400, AXP313A PMIC. The test board has 1.5 GB.
- Firmware: halal-beef's Mu-Silicium fork (Project Mu based), flashed to SPI NOR. It brings up
  HDMI (GOP), EHCI, MMC, EMAC, the PMIC and SPI. Built with `UEFI-Builder` (SPL + TF-A + UEFI).
- Windows 11 ARM64, build 22000 or later, installed on the microSD card with halal-beef's
  `awmmc` storage driver. Test-signing is already on (the guide requires it), which our
  test-signed `panwin.sys` also needs. A53 is ARMv8.0, so 23H2 is the ceiling: 24H2 and later
  reportedly need ARMv8.1.
- Windows has EHCI only (no OHCI), so keyboards need a USB 2.0 hub. PSCI shutdown may hang.

## ACPI

The firmware ships prebuilt AML from Silicium-ACPI (`Zero3Pkg/Include/ACPI.inc`: APIC, DBG2,
`DSDT_minimal`, SSDT, FACP, GTDT). `DSDT_minimal` describes the CPUs, three EHCI controllers and
`SDC0` (`AWMC0001`). There is no GPU node, and nothing in the firmware touches the GPU's clocks,
reset or power domain. Silicium-ACPI has no licence file, so this repo keeps only our patch
(`firmware/zero3/patches/`), not a copy of their DSDT.

## GPU hardware

| What | Value |
|---|---|
| Block | `gpu@1800000`, 0x40000 bytes, `allwinner,sun50i-h616-mali`, `arm,mali-bifrost` |
| IRQs | GIC SPI 95 / 96 / 97 = job / mmu / gpu = GSIV 127 / 128 / 129, level high |
| Coherency | none (`_CCA` 0): user mappings write-combined, same as on RK3588 |
| Supply | vdd-gpu-sys = AXP313 DCDC1, 810–990 mV, shared and always on; `PmicDxe` enables it (for HDMI) |
| Power domain | PRCM PPU GPU gate, 0x07010254 bit 0, inverted (1 = off) |
| Core clock | CCU 0x03001670 `gpu0`: mux bit 24 (0 = pll-gpu, 1 = gpu1), M bits 0–1, gate bit 31 |
| Fallback clock | CCU 0x03001674 `gpu1` = pll-periph0-2x / (M+1), gate bit 31; Linux sets M = 2 (400 MHz) at boot and never gates it |
| Bus gate / reset | CCU 0x0300167C bit 0 / bit 16 (reset de-asserted when set) |
| Model | Mali-G31, Bifrost v7, Mesa `PAN_PROD_ID(7, 0, 3)`, MP2; confirm GPU_ID on hardware |

`firmware/zero3/patches/0001` adds `\_SB.GPU0` (`_HID "AWGP0031"`) with those resources and
`_PS0` / `_PS3` / `_PSC`. `_PS0`: PPU on, gpu1 at 400 MHz, gpu0 from gpu1, bus gate, reset
release. Running from gpu1 avoids programming the GPU PLL; DVFS comes later. The register
sequence is checked under acpiexec (`firmware/zero3/tests/run_aml_test.sh`).

## GPU programming model (differs from G610)

- **Job manager, no firmware.** The kernel writes job-chain addresses into job-slot registers
  (JS0 fragment, JS1 vertex/tiler/compute) and gets per-slot done/fail interrupts. No MCU, no
  CSF interfaces, no `mali_csffw.bin`.
- **uAPI: Linux panfrost** (`include/uapi/drm/panfrost_drm.h`, MIT). Mesa's
  `src/panfrost/lib/kmod/panfrost_kmod.c` issues GET_PARAM, CREATE_BO, MMAP_BO, GET_BO_OFFSET,
  SUBMIT, WAIT_BO, MADVISE, SET_LABEL_BO, SYNC_BO, QUERY_BO_INFO and PERFCNT_*, plus the DRM core
  syncobj, GEM_CLOSE and VERSION ioctls the core already has.
- **BO model:** the kernel picks each BO's GPU VA at creation (one address space per file),
  there is no VM_BIND. Implicit sync through the BO list passed to SUBMIT. HEAP BOs are grown
  page-by-page from the MMU fault handler.
- **Licensing:** Linux panfrost is GPL-2.0-only. It is a behaviour reference only; no code goes
  into the MIT driver. Register meanings are facts. Mesa's MIT `genxml/v7.xml` describes the job
  descriptors the mock executor has to parse.

## Mesa on Bifrost

- panvk builds v7 (`src/panfrost/vulkan/meson.build` arch list), but refuses v6/v7 unless
  `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1` is set and warns it is non-conformant. No source change
  is needed to use it.
- Gallium panfrost (OpenGL ES 3.1 / desktop GL) is the mature Bifrost path, but needs Mesa's WGL
  front end hooked up on Windows: later work.

## Constraints that shape the design

- 1.5 GB shared between Windows and the GPU: keep kernel allocations small, grow heaps on fault.
- Four in-order A53 cores: CPU-side present (GDI blit) will be slow; fine for bring-up.
- The core's OS layer, fences and syncobjs, handle tables, most of the MMU, the Windows glue and
  libdrm-win are reused as they are. The CSF backend stays for a later G610 board.
