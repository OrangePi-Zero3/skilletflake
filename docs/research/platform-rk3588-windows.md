# Platform and firmware research: Windows on Arm with a Mali GPU (RK3588 / edk2-rk3588)

Date: 2026-10-30. Author: platform research subagent.

Reference clone (owned by this research): `/home/e3xp0/Projects/panfrost-win/references/edk2-rk3588`
(edk2-porting/edk2-rk3588, shallow, HEAD `2f3f8265590f77901ac6a1af5c963f957a844158`, 2026-10-25).
Only the `arm-trusted-firmware` submodule was initialised (shallow, blob-less, pinned commit `51d8794cc2b5`).
The edk2-rockchip platform code is in-tree at `edk2-rockchip/`.

Linux reference files (fetched from git.kernel.org torvalds/linux master into the scratchpad, not a clone):
`rk3588-base.dtsi`, `rk3588-opp.dtsi`, `rk3588-rock-5b-5bp-5t.dtsi`, `rk3588s-orangepi-5.dtsi`,
`drivers/pmdomain/rockchip/pm-domains.c`, `drivers/clk/rockchip/clk-rk3588.c`, `dt-bindings/power/rk3588-power.h`.

Legend: **[V]** = I read the code or page myself. **[I]** = my inference. **[?]** = not checked or uncertain.

---

## TL;DR

* The hypothesis holds up. RK3588 (Mali-G610 MP4, Valhall v10, CSF) running edk2-rk3588 in ACPI mode is the most mature Windows-on-Arm platform with a Mali GPU. No Windows GPU driver exists for it. The GPU is listed as "Software-rendered / Not working" [V].
* **The DSDT has no Mali node today** [V]. Nothing in the ASL matches GPU, Mali, `fb000000` or interrupts 124-126.
* What the firmware already provides [V]:
  * `vdd_gpu_s0` (RK806 master BUCK1) is enabled at 750 mV (850 mV on Orange Pi 5) by every board's `Rk806Configure()`.
  * There is a working **ACPI SCMI client in AML**, `\_SB.SCMI` with CLRG/CLRS/CLCS/VDLG/VDLS. It can set `SCMI_CLK_GPU` (id 5) through TF-A.
  * TF-A has GPU PVTPLL clock code for 200-1000 MHz.
* What is missing [V]:
  * Nobody powers on `PD_GPU` (TF-A powers PHP/PCIE/GMAC/SECURE/VOP/VO0/VO1 only).
  * No GPU clock ungating.
  * No SCMI voltage domain for vdd_gpu. The edk2 TF-A patch only exposes `vccio_sd`.
* Adding a GPU node is small and well-patterned. Write a `Gpu.asl` with `_HID`, `_CRS` (0xFB000000/0x200000 + GSIV 124/125/126), `_CCA 0`, and `_PS0`/`_PS3` that do the PMU PD sequence with PMU+CRU OperationRegions. Add a `_DSM` that wraps `\_SB.SCMI.CLRS(5, rate)`. Gate it with `_STA` through the existing AcpiPlatformDxe fixup table. Concrete sketch in section 3.
* GPU is **not** behind any SMMU. It uses its own Mali MMU. It is **not IO-coherent**: no `dma-coherent` in the Linux DT, and every ACPI device in this firmware sets `_CCA 0`. The firmware explicitly avoids "atomics on uncached memory" for Windows because they crash the system on this SoC. This is a real risk for shared CPU/GPU memory.
* Prior art: **gahingwoo/wddm-mali-g52** (RK3576, Mali-G52 Bifrost/JM, panfrost). It reached "first job under Windows" on 2026-10-01 and display-only WDDM plus a triangle through WDDM by 2026-10-03. It uses an ACPI HID `RKCP7402` added to edk2-rk3576. This is the closest precedent, and it validates the "firmware powers on, driver does the rest" split.

---

## 1. Which Windows-capable devices have Mali GPUs?

| Platform | GPU | Windows today | Maturity (USB / PCIe / display / storage) | Verdict |
|---|---|---|---|---|
| **RK3588/RK3588S** boards (ROCK 5B/5B+/5A/5 ITX, Orange Pi 5/5 Plus, Khadas Edge2, NanoPC-T6, CM3588, etc. 25+ boards in `edk2-rockchip/Platform/*`) | Mali-G610 MP4 (Valhall v10, CSF). Linux: panthor + panvk | **Yes**, Windows 11 via edk2-rk3588 ACPI mode [V README] | From the worproject table [V]: USB3 host works (one Type-C orientation); USB2 partial (no OHCI). PCIe "partially working": NVMe was failing with inbox storport (issue #6, possibly stale), and no cache-coherent/MSI devices. eMMC and SD work. GMAC and RTL8125 Ethernet work. GPIO/I2C/I2S work. HDMI/DP "partial": single display through UEFI GOP + MSBDD, 1080p60. GPU, NPU, VPU and CPU DVFS not working. | **Best target** |
| RK3576 (ArmSoM CM5-IO, ROCK 4D) via gahingwoo/edk2-rk3576 + woa-rk3576 | Mali-G52 MC3 (Bifrost, job manager, no FW). Linux: panfrost | Yes, Windows 11 **23H2 only**: "24H2 and later can't run because RK3576 is ARMv8.0" [V web] | USB works (USB-C at USB2), NVMe works, eMMC/SD work, Ethernet works, display BasicDisplay 1 mode, no audio [V web] | Viable but a weaker long-term target (23H2 ceiling). A one-person project already does exactly this. |
| RK3566/RK3568 (Quartz64, ROC-RK356x, Station M2/P2, ODROID-M1S) via jaredmcneill/quartz64_uefi | Mali-G52 2EE (Bifrost) | Only "Windows PE" listed, version "?", BSOD on USB2 plug (#2) [V web] | HDMI/USB3/PCIe in WinPE only | Not mature |
| MediaTek Kompanio (MT8183 Mali-G72, MT8192/8195 Mali-G57/G57MC5) Chromebooks | Bifrost/Valhall JM | No known edk2/Windows port. Renegade Project forum says the MediaTek bootloader is too opaque [V web, weak source] | n/a | Not viable |
| Allwinner A64 (Pine64, BPI-M64) | Mali-400 (Utgard, wrong family) | Windows 10 **IoT Core** only, via an old Allwinner edk2 [V web] | n/a | Irrelevant (Utgard, Win10 IoT) |
| Allwinner H6/H616, Amlogic S922X etc. | Mali-T720/G31/G52 | No Windows port found [V web: nothing found] | n/a | Not viable |
| Exynos / Google Tensor phones | Mali-G7x | No UEFI Windows port found [V web: nothing found] | n/a | Not viable |
| Commercial Windows devices | — | None ship with a Mali GPU (all WoA PCs are Snapdragon/Adreno). The wddm-mali-g52 README also says no Windows Mali driver exists [V web, their claim] | — | — |

Also relevant: Windows 11 24H2+ needs ARMv8.1 (LSE atomics) [V web, edk2-rk3576 README statement]. RK3588 (A76+A55, ARMv8.2) and RK356x (A55) qualify. RK3576 (A72+A53) does not.

Recommended dev boards [I]: **Radxa ROCK 5B / 5B+** or **Orange Pi 5 Plus**. Both are "Platinum" in the edk2-rk3588 README, have mainline DT, SPI NOR, UART2 debug at 1.5 Mbaud, and RK806 with BUCK1 = vdd_gpu. Orange Pi 5 (RK3588S) is cheaper and its firmware sets vdd_gpu to 850 mV, which allows the 1 GHz OPP without DVFS. A single board can dual-boot Linux (DT mode, panthor) and Windows (ACPI mode) from the same firmware. That gives an on-target A/B reference, which is very valuable.

## 2. What edk2-rk3588 does today

### 2.1 DSDT contents (no GPU)

* Common ASL is in `edk2-rockchip/Silicon/Rockchip/RK3588/AcpiTables/*.asl`. Each board picks pieces in its `Dsdt.asl`, for example `edk2-rockchip/Platform/Radxa/ROCK5B/AcpiTables/Dsdt.asl`, which includes DsdtCommon (SCMI), Cpu, Pcie, Sata, Emmc, Sdhc, Dma, Gpio, I2c, Uart, I2s, Usb2Host, Usb3Host0-2 and Es8388. [V]
* `grep -rniE 'gpu|mali|fb000000'` over all of edk2-rockchip only hits CRU clock/reset IDs in `RK3588/Include/RK3588.h` (e.g. `SRST_GPU 1060`, `CLK_GPU_GATE 1060`, `CLK_GPU_COREGROUP_GATE 1062`, `CLK_GPU_STACKS_GATE 1063`, lines ~3299-3312, 4315-4333) and `SCMI_CLK_GPU 5` (`RK3588/Include/ScmiDefinitions.h:18`). **No ACPI device for the GPU.** [V]
* HIDs in use (count): `RKCP3001` I2C x8, `RKCP3002` GPIO x5 (+`PRP0001` CID), `RKCP3003` I2S x2, `RKCP0D40` eMMC (dwcmshc), `RKCPFE2C` SDMMC, `RKCP0D20` EHCI (CID `PNP0D20`, hidden on Windows), `RKCP0161` AHCI x3, `RKCP6543` GMAC x2, `HISI0031` UART (8250-dw), `ARMH0330` PL330 DMA x3, `PNP0D10` XHCI x3, `PNP0A08/PNP0A03` PCIe RCs, `AMZN0001`, `PNP0C02` (SCMI shmem, PCIe resources), `ACPI0007/ACPI0010` CPUs/clusters, `ESSX8316/ESSX8388` codecs, `PRP0001` for misc DT-style nodes. [V]
  → The project convention is a project-local `RKCPxxxx` vendor prefix. [I] A new `RKCP` ID for the G610 fits, but avoid `RKCP7402` (used by edk2-rk3576 for the Mali-G52).
* Interrupt numbering in `_CRS` = GIC INTID = DT `GIC_SPI n` + 32. Verified with XHC0: DT SPI 220 → ASL 252 (`Usb3Host0.asl`), and eMMC: DT SPI 205 → ASL 237 (`Emmc.asl`). [V]
* Every peripheral sets `_CCA 0` (non-coherent), e.g. `Emmc.asl:14`, `Pcie.asl:15`, `Sata.asl:16`, all of `Usb*Host*.asl`, `Gmac*.asl`. [V]
* `IORT` (`RK3588PcieIort.aslc`) has only 2 ITS groups + 1 root complex. **No SMMUv3 nodes**, so Windows does not use the MMU-600s. [V]

### 2.2 The ACPI SCMI client (important building block)

`edk2-rockchip/Silicon/Rockchip/RK3588/AcpiTables/Scmi.asl` [V]:

* `Device (SCMI)` with `_HID PNP0C02`. Shared memory is `PcdRkMtlMailBoxBase` = `0x0010F000`, size `0x100` (`RockchipPkg.dec:99-100`). The doorbell is mailbox0 ch0 at `0xFEC60030`. Polled SMT transport.
* Methods:
  * `CLRG(id)` (clock rate get, line 52)
  * `CLRS(id, rate)` (clock rate set, line 83)
  * `CLCS(id, enable)` (clock config set, line 114)
  * `VDLG(dom)` / `VDLS(dom, uV)` (voltage level get/set, line 171)
* Nothing in edk2 ASL calls them. [V] [I] They exist so OS drivers can call them (Windows: `IOCTL_ACPI_EVAL_METHOD` / `ACPI_INTERFACE_STANDARD2`).
  → **A Windows GPU driver can set the GPU clock today** with `\_SB.SCMI.CLRS(5, Hz)`. TF-A side: `arm-trusted-firmware/plat/rockchip/rk3588/drivers/scmi/rk3588_clk.c`:
  * `clk_scmi_gpu_set_rate()` (~line 1003) and `clk_gpu_set_rate()` (~955).
  * PVTPLL table at lines 236-246: 300-1000 MHz use the ring-oscillator PVTPLL in GPU_GRF `0xFD5A0000`. 200 MHz uses GPLL/div via `CRU_CLKSEL_CON(158)`.
  * Rate list `rk3588_gpu_rates[]` (line 275): 200, 300, …, 900, 1000 MHz. [V]

### 2.3 Regulator (vdd_gpu) handling

* Linux DT: `vdd_gpu_s0` = RK806 **dcdc-reg1** (master BUCK1), 550-950 mV, ramp 12.5 mV/us, enable delay 400 us. It is both `mali-supply` and `pd_gpu` `domain-supply` (`rk3588-rock-5b-5bp-5t.dtsi:703-714`, `:180-183`, `:505-507`; same in `rk3588s-orangepi-5.dtsi:373`, `:102`, `:218`). [V]
* edk2: `RK3588Dxe.c:303` calls `Rk806Configure()`. On every board `RockchipPlatformLib.c` has `RK8XX_VOLTAGE_INIT(MASTER_BUCK1, 750000)` (OrangePi5: **850000**). `RK806RegulatorInit()` (`Silicon/Rockchip/Library/SpiLib/RK806.c:705-727`) sets the voltage **and enables** the buck. → **vdd_gpu is ON at 750 mV when Windows boots.** [V]
* Linux OPP (`rk3588-opp.dtsi:118-153`): 300-600 MHz @ 675 mV, 700 @ 700, **800 @ 750**, 900 @ 800, 1000 @ 850 mV. [V] → At the firmware default of 750 mV, anything ≤ 800 MHz is in spec without touching the PMIC. [I]
* TF-A SCMI voltage domains (edk2 patch `arm-trusted-firmware-patches/0006-feat-rk3588-initial-support-for-SCMI-voltage-domain.patch`) expose only `SCMI_VCCIO_SD_S0 → MASTER_PLDO5`. [V] Adding `SCMI_VDD_GPU_S0 → MASTER_BUCK1` (550-950 mV, step 6250 or 12500 uV) is a ~10-line patch. Then `\_SB.SCMI.VDLS(1, uV)` gives Windows GPU DVFS. [I]
* Windows has no regulator framework and never turns the buck off. Linux's "disable unused regulators" problem does not exist here. [I]

### 2.4 Power domain

* PMU2 block at `0xFD8D8000` (TF-A `PMU0_BASE 0xFD8D0000` + `0x8xxx` offsets; Linux `pmu@fd8d8000`). [V]
* Linux `pm-domains.c` [V]:
  * `rk3588_pmu`: `pwr_offset 0x14c`, `status 0x180`, `req 0x10c`, `idle 0x120`, `ack 0x118`, `repair_status 0x290`.
  * `RK3588_PD_GPU` = id 12: `DOMAIN_RK3588("gpu", 0x0, BIT(0), 0, 0x0, 0, BIT(1), 0x0, BIT(0), BIT(0), false, true)`. That means pwr bit0 @ +0x14c (write-mask bit16), status via repair bit1 @ +0x290 (1 = on), idle-req bit0 @ +0x10c, ack/idle bit0 @ +0x118/+0x120, `need_regulator = true`.
* Linux on-sequence (`rockchip_pd_power()`, pm-domains.c ~640-690) [V]:
  1. Enable PD clocks (`CLK_GPU`, `CLK_GPU_COREGROUP`, `CLK_GPU_STACKS`).
  2. Write `0x00010000` to `+0x14c`.
  3. Poll `+0x290` bit1 == 1.
  4. Write `0x00010000` to `+0x10c` (leave idle).
  5. Poll `+0x118` bit0 == 0.
  6. Restore QoS (`qos_gpu_m0..m3` @ `0xFDF35000/5200/5400/5600`, 0x20 each).
  7. Gate the clocks again. The GPU driver then holds its own clocks.

  Off is the reverse: save QoS, idle-req = 1, wait ack = 1, then pwr bit = 1.
* TF-A `rockchip_pmu_pd_init()` (`plat/rockchip/rk3588/drivers/pmu/pmu.c:1375-1388`) powers on **PHP, PCIE, GMAC, SECURE, VOP, VO0, VO1, not GPU**. TF-A touches PD_GPU only in suspend/resume (pmu.c:620, :709). [V]
* edk2 touches PMU PDs only for VOP (`Vop2Dxe.c:778`, `MmioWrite32(0xfd8d8150, 0xffff0000)`) and PCIe/PHP (`Pcie30PhyLib.c:57`). [V]
* **The PD_GPU state at OS handoff is the reset/BootROM default, which I could not verify** (no TRM here). [?] The wddm-mali-g52 project says touching GPU registers before the PD ACK clears raises an **SError**. So **firmware or the driver must power it on explicitly**.

### 2.5 Clocks (CRU, base `0xFD7C0000`)

From `clk-rk3588.c` (~1362-1376) and `RK3588.h` (`CRU_CLKGATE_CON[]` @ +0x800, `CRU_CLKSEL_CON[]` @ +0x300) [V]:

| Clock | Register | Bit / field |
|---|---|---|
| `clk_gpu_src` (mux gpll/cpll/aupll/npll/spll, owned by TF-A via SCMI) | `CLKSEL_CON(158)` = `0xFD7C0578`; gate `CLKGATE_CON(66)` = `0xFD7C0908` | sel [7:5], div [4:0], bit14 = PVTPLL path; gate bit1 |
| `clk_gpu` ("core") | `CLKGATE_CON(66)` | bit4 |
| `clk_gpu_coregroup` | `CLKGATE_CON(66)` | bit6 |
| `clk_gpu_stacks` | `CLKGATE_CON(66)` | bit7; div in `CLKSEL_CON(159)` [4:0] |
| `aclk_s/m0..m3_gpu_niu`, `pclk_gpu_root` | `CLKGATE_CON(66)` | bits 8-13 (from gate IDs 1064-1069) [I: mapping id/16, id%16] |
| `clk_gpu_pvtm`, `clk_core_gpu_pvtm` | `CLKGATE_CON(67)` = `0xFD7C090C` | bits 0, 1 |

Rockchip registers use a write-enable mask in the upper 16 bits. Ungating bits 1,4,6,7 is `MmioWrite32(0xFD7C0908, 0x00D20000)`. [I: from the convention]

Linux DT for the GPU node (`rk3588-base.dtsi:449-465`) [V]:
* `compatible "rockchip,rk3588-mali", "arm,mali-valhall-csf"`, `reg 0xfb000000 0x200000`
* `assigned-clocks = <&scmi_clk SCMI_CLK_GPU>` @ 200 MHz
* `clocks core/coregroup/stacks`
* `interrupts SPI 92/93/94` named `job`, `mmu`, `gpu` → **GSIV 124 / 125 / 126**
* `power-domains = <&power RK3588_PD_GPU>`, no `resets`, no `iommus`, **no `dma-coherent`**, `dynamic-power-coefficient 2982`

### 2.6 Display on Windows today

* `Silicon/Rockchip/Drivers/Vop2Dxe` + `LcdGraphicsOutputDxe` bring up VOP2 + HDMI/DP in UEFI and publish a GOP. The framebuffer is allocated `AllocateMaxAddress` below 4 GiB, as `EfiRuntimeServicesData`, with the comment "VOP2 only supports 32-bit addresses" (`LcdGraphicsOutputDxe.c:862`). [V]
* Windows binds the inbox **Microsoft Basic Display Driver (MSBDD/BasicDisplay)** to the GOP framebuffer: single display, 1080p60, no mode changes. [V worproject table] There is no VOP2 OS driver. HDMI audio is blocked on it [V].
* Implications for us [I]:
  1. A render-only GPU driver (no scanout) can coexist with BasicDisplay. Presenting means a CPU copy (or GPU blit) into a BasicDisplay/DWM surface, which is how a non-WDDM compute/Vulkan path would work.
  2. A real WDDM "full" driver would also need a VOP2 flip path. Scanout buffers must be < 4 GiB physical. wddm-mali-g52 plans a "small display driver that flips VOP2 windows and never touches the HDMI controller/PHY", reusing the UEFI-programmed mode.

### 2.7 Other firmware hooks useful for us

* `AcpiPlatformDxe.c:106-132` `AcpiDsdtFixupStatus()`: a table of `{ "\\_SB.XXXX._STA", condition }` driven by HII/PCD options. This is the pattern for a "GPU node enabled" setup option. [V]
* `AcpiPlatformDxe.c:402-432`: an ExitBootServices handler that knows the OS type (`ExitBootServicesOsWindows`) and patches DSDT names. It hides the EHCI CID "for Windows to avoid binding to the inbox driver, which by default uses atomics on uncached memory and would crash the system". [V] We could expose the GPU only to Windows the same way (Linux ACPI mode would ignore an unknown HID anyway).
* `FdtPlatformDxe` lets the same firmware boot Linux in DT mode. Useful for side-by-side panthor reference traces. [V]

## 3. What to add to edk2-rk3588 (concrete sketch)

### 3.1 Two options for power-up

**A. "Firmware powers on, driver owns the rest"** (simplest; what wddm-mali-g52 did on RK3576). In `RK3588Dxe` (or a tiny `GpuDxe`), at boot:

```c
// Silicon/Rockchip/RK3588/Drivers/RK3588Dxe/Gpu.c  (sketch, untested)
#define PMU2            0xFD8D8000
#define PMU_BUS_IDLE_REQ0   (PMU2 + 0x10C)
#define PMU_BUS_IDLE_ACK0   (PMU2 + 0x118)
#define PMU_BUS_IDLE_ST0    (PMU2 + 0x120)
#define PMU_PWR_GATE_CON0   (PMU2 + 0x14C)
#define PMU_BISR_STATUS4    (PMU2 + 0x290)
#define CRU_GATE_CON66      (0xFD7C0000 + 0x800 + 66*4)   // 0xFD7C0908

VOID GpuPowerOn (VOID) {
  UINTN T;
  // vdd_gpu_s0 already enabled by Rk806Configure() (BUCK1 750 mV)
  MmioWrite32 (CRU_GATE_CON66, 0x00D20000);           // ungate gpu_src, gpu, coregroup, stacks
  MmioWrite32 (PMU_PWR_GATE_CON0, 0x00010000);        // PD_GPU power on (bit0=0, wmask bit16)
  for (T = 0; T < 10000 && !(MmioRead32 (PMU_BISR_STATUS4) & BIT1); T++) MicroSecondDelay (1);
  MmioWrite32 (PMU_BUS_IDLE_REQ0, 0x00010000);        // release GPU NIU idle request
  for (T = 0; T < 10000 && (MmioRead32 (PMU_BUS_IDLE_ACK0) & BIT0); T++) MicroSecondDelay (1);
  // optional: SCMI clock set 200 MHz via the RkMtl mailbox lib (or leave to OS via \_SB.SCMI.CLRS)
}
```

To sanity-check: read `0xFB000000` (GPU_ID). Expect `0xa8670005`-ish for G610 (arch 10.8). [? exact value: compare with panthor log on Linux.] Print it to the UART2 debug log.

**B. ACPI-managed power (better long term, enables D3 / runtime PM).** Put the same sequence in `_PS0`/`_PS3` so Windows (PoFx / KMDF `WdfDeviceStopIdle`) can power-gate the GPU. You can also do both: DXE powers on at boot, `_PS3`/`_PS0` handle later transitions.

### 3.2 ASL sketch: `Silicon/Rockchip/RK3588/AcpiTables/Gpu.asl`

```asl
#include "AcpiTables.h"

// Mali-G610 MP4 (Valhall v10, CSF). Linux: rockchip,rk3588-mali / arm,mali-valhall-csf
Device (GPU0) {
  Name (_HID, "RKCP7410")        // placeholder ID; coordinate with edk2-rk3588 maintainers (RKCP7402 = RK3576 G52)
  Name (_UID, 0)
  Name (_CCA, 0)                 // NOT coherent (no dma-coherent in DT)
  Name (_S0W, 3)                 // can go to D3 while S0 (if _PS3 is implemented)

  Method (_STA) { Return (0xF) } // patched by AcpiDsdtFixupStatus() from an HII option

  Method (_CRS, 0x0, Serialized) {
    Name (RBUF, ResourceTemplate () {
      Memory32Fixed (ReadWrite, 0xFB000000, 0x200000)
      Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 124 } // JOB  (SPI 92)
      Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 125 } // MMU  (SPI 93)
      Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 126 } // GPU  (SPI 94)
    })
    Return (RBUF)
  }

  // Documentation-only DT properties (do NOT add _CID PRP0001: Linux panthor has no ACPI clock/PD glue)
  Name (_DSD, Package () {
    ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
    Package () {
      Package () { "compatible", Package () { "rockchip,rk3588-mali", "arm,mali-valhall-csf" } },
      Package () { "interrupt-names", Package () { "job", "mmu", "gpu" } },
      Package () { "dynamic-power-coefficient", 2982 },
    }
  })

  OperationRegion (PMUR, SystemMemory, 0xFD8D8000, 0x300)
  Field (PMUR, DWordAcc, NoLock, Preserve) {
    Offset (0x10C), IREQ, 32,   // BUS_IDLE_SFTCON0  (bit0 = GPU NIU, wmask <<16)
    Offset (0x118), IACK, 32,   // BUS_IDLE_ACK0
    Offset (0x120), IDST, 32,   // BUS_IDLE_ST0
    Offset (0x14C), PGCN, 32,   // PWR_GATE_SFTCON0  (bit0 = PD_GPU, 1 = off)
    Offset (0x180), PGST, 32,   // PWR_GATE_ST0
    Offset (0x290), BISR, 32,   // BISR_STATUS4      (bit1 = GPU repaired/on)
  }
  OperationRegion (CRUG, SystemMemory, 0xFD7C0908, 0x4)
  Field (CRUG, DWordAcc, NoLock, Preserve) { GT66, 32 }  // CLKGATE_CON66

  Name (PSTA, 0)

  Method (_PS0, 0, Serialized) {
    GT66 = 0x00D20000                         // ungate clk_gpu_src/clk_gpu/coregroup/stacks
    PGCN = 0x00010000                         // power on PD_GPU
    Local0 = 1000
    While (((BISR & 0x2) == 0) && (Local0 > 0)) { Stall (10); Local0-- }
    IREQ = 0x00010000                         // leave bus idle
    Local0 = 1000
    While (((IACK & 0x1) != 0) && (Local0 > 0)) { Stall (10); Local0-- }
    \_SB.SCMI.CLRS (5, 200000000)             // SCMI_CLK_GPU = 200 MHz (safe at any vdd)
    PSTA = 0
  }

  Method (_PS3, 0, Serialized) {
    // NOTE: driver must have idled the GPU / halted the MCU (panthor: panthor_fw_suspend) first.
    IREQ = 0x00010001                         // request bus idle
    Local0 = 1000
    While (((IACK & 0x1) == 0) && (Local0 > 0)) { Stall (10); Local0-- }
    PGCN = 0x00010001                         // power off PD_GPU
    Local0 = 1000
    While (((BISR & 0x2) != 0) && (Local0 > 0)) { Stall (10); Local0-- }
    GT66 = 0x00D000D0                         // gate gpu/coregroup/stacks (leave gpu_src to TF-A)
    PSTA = 3
  }

  Method (_PSC) { Return (PSTA) }

  // Driver-private helpers (UUID to be generated). Func 1: set rate (Hz), 2: get rate, 3: set vdd_gpu (uV)
  Method (_DSM, 4, Serialized) {
    If (Arg0 == ToUUID ("00000000-0000-0000-0000-000000000000")) {   // TODO real UUID
      Switch (ToInteger (Arg2)) {
        Case (0) { Return (Buffer () { 0x0F }) }
        Case (1) { Return (\_SB.SCMI.CLRS (5, DerefOf (Arg3[0]))) }
        Case (2) { Return (\_SB.SCMI.CLRG (5)) }
        Case (3) { Return (\_SB.SCMI.VDLS (1, DerefOf (Arg3[0]))) }    // needs TF-A voltd patch (3.3)
      }
    }
    Return (Buffer () { 0 })
  }
}
```

Then:
* Add `include ("Gpu.asl")` to each board `Dsdt.asl`. At least ROCK5B, OrangePi5, OrangePi5Plus to start.
* Add `{ "\\_SB.GPU0._STA", PcdGet32 (PcdGpuAcpiEnable) }` to `AcpiDsdtFixupStatus()`, plus an HII toggle in `RK3588DxeHii.vfr`, default off until the driver exists.

Caveats [I/?]:
* `Stall()` is limited to ≤100 us per call, so the loops above are fine.
* Whether AML on Windows is allowed to touch PMU/CRU MMIO concurrently with other drivers: the write-mask registers make the writes atomic per bit, so this is OK.
* QoS registers are not restored after PD on. The defaults are probably fine for bring-up. A later step could save/restore `0xFDF35000+{0,0x200,0x400,0x600}` x 0x20.
* **Order of SCMI clock set vs. PD on**: in Linux, `assigned-clock-rates` are applied in `platform_probe()` before `dev_pm_domain_attach()`. At 200 MHz TF-A uses the GPLL path and does not touch GPU_GRF. For ≥300 MHz TF-A programs PVTPLL in **GPU_GRF 0xFD5A0000**. That probably requires PD_GPU on and `pclk_gpu_root`/`pclk_gpu_grf` ungated [?]. So call CLRS(≥300 MHz) only after `_PS0`. Also: PVTPLL is a ring oscillator, so its achieved frequency depends on voltage. Raise the voltage first.
* Mali also needs its CSF firmware (`mali_csffw.bin`, arch10.8, from linux-firmware) loaded by the OS driver. That is not a firmware concern, but redistribution licensing should be checked (other researcher).

### 3.3 TF-A patch for GPU DVFS (optional, later)

Extend the edk2 patch `0006` in `plat/rockchip/rk3588/boards/rk3588_reference_pmic/scmi/rk3588_voltd.c`:

```c
#define SCMI_VDD_GPU_S0  1
rk806_scmi_reg_t rk806_ref_table[] = {
  RK806_SCMI_REG(SCMI_VCCIO_SD_S0, MASTER_PLDO5),
  RK806_SCMI_REG(SCMI_VDD_GPU_S0,  MASTER_BUCK1),
};
static rk_scmi_voltd_t rk3588_voltd_table[] = {
  RK3588_SCMI_VOLTD(SCMI_VCCIO_SD_S0, "scmi_vccio_sd_s0", 1800000, 3300000, 12500, &rk806_voltd_ops),
  RK3588_SCMI_VOLTD(SCMI_VDD_GPU_S0,  "scmi_vdd_gpu_s0",   550000,  950000,  6250, &rk806_voltd_ops),
};
```

Risk: all supported boards must really wire BUCK1 to vdd_gpu. That holds for every board in-tree, since all of them init BUCK1 to the GPU voltage [V: grep of `MASTER_BUCK1,` across 22 `RockchipPlatformLib.c`]. But some boards may use a different PMIC topology. Make it per-board.

Without this patch: run fixed at 750 mV, which gives ≤ 800 MHz (Linux OPP table). That is good enough for the whole bring-up.

## 4. IOMMU / SMMU and DMA coherency

* **No SMMU in front of the GPU.** [V] The RK3588 MMU-600s are `mmu600_pcie @ 0xFC900000` and `mmu600_php @ 0xFCB00000` (rk3588-base.dtsi:554, 565), and neither is in the Windows IORT. The other `rockchip,rk3568-iommu` instances serve NPU/VPU/RGA/VOP/ISP. Mali uses its **own internal MMU** (AS0..ASn, LPAE-like page tables). So the Windows driver programs physical addresses into Mali page tables itself. There is no DMA remapping and no `DmaIommu` involvement. [I]
* **Not IO-coherent.** [V] No `dma-coherent` anywhere in rk3588-base.dtsi. Every ACPI device in edk2-rk3588 uses `_CCA 0`. A linux-rockchip post says RK356x and possibly RK3588 lack an outer coherent cache [V web, unconfirmed by TRM]. Panthor reads `GPU_COHERENCY_FEATURES`, but with no `dma-coherent` it uses `COHERENCY_NONE` and maps BOs write-combined. [I: from memory of panthor_device.c, to be confirmed by the kernel-side researcher]. Consequences for Windows [I]:
  * GPU-visible CPU mappings should be write-combined / uncached (`MmWriteCombined` / `MmNonCached`, or MDL mapped with `MmMapLockedPagesSpecifyCache(…, MmWriteCombined)`). Otherwise there must be explicit cache maintenance (`KeFlushIoBuffers` is a no-op on some arm64 configs, so check this; Windows arm64 drivers normally rely on `_CCA 0` + DMA adapter APIs for maintenance).
  * **Atomics on uncached memory crash RK3588 under Windows**, per the firmware comment in `AcpiPlatformDxe.c:412-416` (EHCI). CPU-side atomics on GPU-shared NC/WC memory (fences, ring-buffer pointers, CSF interface shared regions, Vulkan app atomics on HOST_COHERENT mappings) are a real hazard. Possible mitigations: use cached memory + explicit clean/invalidate for CPU-written control structures, or avoid LDXR/STXR/LSE on WC mappings. Needs investigation by the KMD/UMD researchers.
  * Physical address width: RK3588 RAM goes up to 32 GiB (`Rk3588Mem.c:111-136`: RAM < 0xF0000000, MMIO 0xF0000000-0xFFFFFFFF, RAM ≥ 4 GiB at 0x1_0000_0000, "BAD" ranges at 0x3_FC00_0000 and 0x3_FFF0_0000). The Mali MMU supports ≥40-bit PA [? check MMU_FEATURES on hardware]. wddm-mali-g52 ran page tables above 4 GiB fine on RK3576 [V web]. Only **VOP2 scanout** needs < 4 GiB [V].
* PCIe dGPU alternative: the worproject README says PCIe on Windows does not support "cache-coherent/MSI devices" [V]. No Windows dGPU success on RK3588 was found [V web: none found]. A dGPU path is not an easy way out. [I]

## 5. Community status and prior attempts

* worproject/Rockchip-Windows-Drivers: GPU "🔴 Not working, Software-rendered" [V]. Issue #54 "gpu support" (Jan 2025): maintainer idigdoug said "Nobody is currently working on GPU driver", "You're welcome to start. This is a volunteer project", and closed it [V].
* edk2-rk3588 #183/#184 "Hardware acceleration/gpu support" (Jan 2025): closed "not planned" by mariobalanica. A commenter noted that the mainline Mali driver doesn't support ACPI mode, so users should use DT mode on Linux [V].
* edk2-rk3588 README: "ACPI support is only being developed and tested against Windows" [V]. That makes a Windows-only GPU ACPI node in scope for upstream.
* **gahingwoo/wddm-mali-g52** [V web, README + docs/PLAN.md]. GPL-2.0. RK3576, Mali-G52 (Bifrost JM), ArmSoM CM5-IO, firmware gahingwoo/edk2-rk3576.
  * HID `ACPI\RKCP7402`, with reg + 3 IRQs (GSIV 379-381) in DSDT. Firmware does supply/clock/PD; the driver does reset/MMU/jobs.
  * Milestones: M1 KMDF job ran 2026-10-01. Mesa 26.2 built for Win ARM64 in CI. Panfrost via the Mesa **d3d10umd** frontend: D3D11 triangle 2026-10-02, display-only WDDM 2026-10-02, Mesa loaded as hardware UMD 2026-10-03. DWM-on-Mali not done.
  * Their notes: an SError if GPU registers are touched before the PD ACK clears. VOP2 32-bit scanout. Mesa **removed d3d10umd on 2026-09-30** (commit `1f9af627b1`, MR 44717), so 26.2 is the last release with it. pan_kmod has ~20 ops and the Linux backend is ~1000 LOC.
  * This is the most important precedent. Their KMDF→WDDM path and edk2 ACPI node design transfer to RK3588 almost directly. The differences: CSF firmware/MCU instead of JM, and panthor uAPI instead of panfrost.
* gahingwoo/woa-rk3576 + edk2-rk3576: Windows 11 23H2 boots on CM5-IO. Windows 24H2+ impossible (ARMv8.0) [V web].
* quartz64_uefi (RK356x): WinPE only. FreeBSD users note panfrost is unusable because "the quartz64 does not expose it in an acpi table" [V web].
* No Mali support exists in any shipping Windows build, and no vendor driver is known [V web, weak sources: forum speculation about a Qualcomm exclusivity deal is unverified].

## 6. Recommendation (platform)

1. **Primary target: RK3588 on edk2-rk3588 ACPI mode, Windows 11 24H2+.** Dev boards: ROCK 5B(+) or Orange Pi 5 Plus, plus a 1.5 Mbaud UART. Keep a Linux DT-mode boot on the same board for panthor/panvk reference traces.
2. Firmware work:
   * Phase 0: in a fork of edk2-rk3588, add DXE power-on of PD_GPU + clock ungate, and add `Gpu.asl` with `_HID/_CRS/_CCA/_STA`. Verify by reading GPU_ID from a trivial KMDF driver.
   * Phase 1: `_PS0/_PS3` + `_DSM` wrapping SCMI clock.
   * Phase 2: TF-A SCMI vdd_gpu voltd patch for DVFS.
   * Then upstream to edk2-porting with a Windows-only `_STA`, default off.
3. Coordinate the HID with edk2-rk3588 maintainers (Mario Bălănică) and gahingwoo early, so both Rockchip GPU nodes follow one convention.
4. Consider RK3576 (G52, JM, no FW) as a **secondary/fallback** target. It is simpler hardware, and if our KMD abstracts JM vs CSF it can share code with wddm-mali-g52. But it is capped at Windows 23H2.

## 7. Risks

* PD_GPU default state and exact on-sequence are unverified without the TRM or hardware. Reading GPU registers too early gives an SError [V web].
* PVTPLL above 200 MHz may need GPU_GRF access, which may need PD on and voltage-dependent rates [?].
* Non-coherent DMA + "no atomics on uncached memory" on RK3588/Windows [V firmware comment]. This is a design-level constraint for shared memory, fences and CSF rings.
* Windows sees only BasicDisplay. Real presentation needs a VOP2 driver (WDDM full) or CPU copy. Scanout is < 4 GiB only.
* There is no Windows regulator/clock framework, so DVFS must go through AML→SCMI (slow, polled mailbox, ~ms latency). This is OK for coarse DVFS, not for fast boosting.
* The HID is not registered (RKCP is project-local). That collides with nothing today except RKCP7402 (RK3576 G52).
* Licensing: CSF firmware blob redistribution (to check). wddm-mali-g52 is GPL-2.0, which limits code reuse if our driver is MIT/BSD.

## 8. Open questions

* Reset value of `PMU PWR_GATE_SFTCON0` bit0 / is PD_GPU on at BL31 exit? Test: read `0xFD8D8290` bit1 from the UEFI shell (`mm 0xFD8D8290 -w 4`) on a real board.
* GPU_ID / MMU_FEATURES (PA bits) / COHERENCY_FEATURES as read on RK3588. Get them from a Linux panthor boot log on the same board.
* Does Windows arm64 map `MmNonCached` as Device-nGnRE or Normal-NC? Which of these does the "atomics crash" apply to?
* Whether PoFx D3 on an ACPI-enumerated KMDF/WDDM device on this platform actually calls `_PS3`/`_PS0` (ACPI.sys should).
* Is 750 mV enough for 800 MHz on all RK3588 bins? Linux uses OPP-binning/leakage tables (rockchip opp-supported-hw) in vendor kernels; mainline uses the single table above.

## Sources

* edk2-rk3588 clone (paths above); TF-A `51d8794c` + `arm-trusted-firmware-patches/` in the same clone.
* Linux: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/arch/arm64/boot/dts/rockchip/rk3588-base.dtsi (and rk3588-opp.dtsi, rk3588-rock-5b-5bp-5t.dtsi, rk3588s-orangepi-5.dtsi, drivers/pmdomain/rockchip/pm-domains.c, drivers/clk/rockchip/clk-rk3588.c)
* https://github.com/worproject/Rockchip-Windows-Drivers (status table), https://github.com/worproject/Rockchip-Windows-Drivers/issues/54
* https://github.com/edk2-porting/edk2-rk3588/issues/183
* https://github.com/gahingwoo/wddm-mali-g52 , https://github.com/gahingwoo/wddm-mali-g52/blob/main/docs/PLAN.md
* https://github.com/gahingwoo/woa-rk3576 , https://github.com/gahingwoo/edk2-rk3576
* https://github.com/jaredmcneill/quartz64_uefi
* https://www.cnx-software.com/2023/03/09/windows-11-rock-5b-arm-sbc-rockchip-rk3588/
* https://lists.infradead.org/pipermail/linux-rockchip/2022-March/030324.html (non-coherent PCIe / no outer coherent cache claim)
