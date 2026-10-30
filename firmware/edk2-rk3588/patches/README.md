# edk2-rk3588 patches: Mali-G610 GPU node and power-up

These patches add Mali-G610 support to the [edk2-rk3588](https://github.com/edk2-porting/edk2-rk3588)
UEFI/ACPI firmware. With them, Windows enumerates the GPU as `ACPI\RKCP7410` and the GPU is powered
when the OS starts.

**Status: builds (rock-5b and orangepi-5 images), AML logic exercised under ACPICA `acpiexec`.
Untested on hardware.**

| Patch | What it does |
|---|---|
| `0001-RK3588-add-ACPI-device-for-the-Mali-G610-GPU.patch` | New `Silicon/Rockchip/RK3588/AcpiTables/Gpu.asl`, included from `DsdtCommon.asl`, so every RK3588/RK3588S board (all 22 in-tree DSDTs) gets `\_SB.GPU0`. Orange Pi 5 raises its `_DSM` clock cap to 1 GHz. |
| `0002-RK3588Dxe-power-on-the-Mali-G610-GPU-at-boot.patch` | New `RK3588Dxe/Gpu.c`. It powers on PD_GPU at DXE, behind a new setup page "GPU" (`GpuState`, default **Enabled**). `AcpiPlatformDxe` sets `\_SB.GPU0._STA` to 0 when the setting is Disabled. |
| `0003-ITX-3588J-cap-the-GPU-_DSM-clock-at-700-MHz.patch` | Firefly ITX-3588J (RK3588J SoC) caps the `_DSM` clock at 700 MHz, the top of the Linux `rk3588j.dtsi` GPU OPP table. Added in review. |

Base commit: `2f3f8265590f77901ac6a1af5c963f957a844158` (edk2-porting/edk2-rk3588 master,
2026-10-25, "RK3588: enable PCIe segments 2/3 when the PCIe 3.0 PHY is bifurcated (#298)").
The patches only touch `edk2-rockchip/` in the superproject. No submodule is changed.

License: the new files (`Gpu.asl`, `Gpu.c`, `Gpu.h`) are MIT. The small edits to existing files
stay under those files' BSD-2-Clause-Patent license. The register meaning was cross-checked against
TF-A `plat/rockchip/rk3588` (BSD-3-Clause), the Rockchip CRU HAL IDs in `RK3588.h`, and the Linux
DT. No GPL code (Rockchip pmdomain or the Linux clock driver) was copied. If the patches go
upstream, the authors may relicense the new files to BSD-2-Clause-Patent to match the tree.

## What the firmware now provides

### ACPI device `\_SB.GPU0` (`Gpu.asl`)

| Object | Value |
|---|---|
| `_HID` | `"RKCP7410"`. This must match `PANWIN_ACPI_HID` in `include/panwin/panwin_ioctl.h`. |
| `_UID` | 0 |
| `_CCA` | 0. The GPU is not IO-coherent (there is no `dma-coherent` in the Linux DT). |
| `_STA` | `Name (_STA, 0xF)`. AcpiPlatformDxe patches it to 0 when setup "GPU" is Disabled. |
| `_CRS` | `Memory32Fixed (ReadWrite, 0xFB000000, 0x200000)`, then `Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive)` 124, 125, 126, in the order **job, mmu, gpu**. |
| `_DSD` | Informational only: `compatible`, `interrupt-names`, `dynamic-power-coefficient`. There is no `PRP0001` `_CID`, so Linux in ACPI mode ignores the device. |
| `_PS0` / `_PS3` / `_PSC` | Runtime power management of PD_GPU. See the sequence below. |
| `_DSM` | Clock get/set through SCMI, the maximum safe clock, and power diagnostics. See below. |

The interrupt values were checked against `rk3588-base.dtsi` `gpu@fb000000`:
`interrupts = <GIC_SPI 92 IRQ_TYPE_LEVEL_HIGH 0>, <GIC_SPI 93 ...>, <GIC_SPI 94 ...>`,
`interrupt-names = "job", "mmu", "gpu"`. That gives GSIV = SPI + 32 = 124, 125, 126, all
level-triggered and active-high. This matches the tree's convention (eMMC: DT SPI 205 is ASL 237).

### Power-up sequence (DXE `Gpu.c` and ASL `_PS0`)

| Step | Register | Write / wait |
|---|---|---|
| 0 | vdd_gpu_s0 (RK806 BUCK1) | Already on at 750 mV (850 mV on Orange Pi 5) from the board's `Rk806Configure()`. The DXE code waits until that has run. |
| 1 | clk_gpu = 200 MHz | SCMI clock 5 (`SCMI_CLK_GPU`) RateSet(200 MHz). TF-A then programs CRU `CLKSEL_CON158` = GPLL/6 (198 MHz), PVTPLL deselected. If the SCMI protocol is missing, the DXE code writes `CLKSEL_CON158` directly with the same values. |
| 2 | CRU `CLKGATE_CON66` `0xFD7C0908`, `CON67` `0xFD7C090C` | Ungate clk_gpu_src(_df), clk_gpu, coregroup, stacks, NIU a/pclks, pclk_gpu_root, pclk_gpu_grf, PVTM/PVTPLL. ASL writes `0x7FDA0000` and `0x000E0000`. |
| 2b | CRU `SOFTRST_CON66/67` `0xFD7C0B08/0B0C` | DXE only: de-assert GPU, SYS_GPU, NIU, GRF and PVTPLL resets. This should be a no-op, and the code logs any reset it finds asserted. |
| 3 | PMU2 `PWR_GATE_SFTCON0` `0xFD8D814C` | Write `0x00010000` (bit 0 = 0 powers PD_GPU on). Then poll `BISR_STATUS4` `0xFD8D8290` until bit 1 = 1 (powered and memory repaired), with a 10 ms timeout. |
| 4 | PMU2 `BUS_IDLE_SFTCON0` `0xFD8D810C` | Write `0x00010000` to release the GPU NIU idle request. Then poll `BUS_IDLE_ACK0` `0xFD8D8118` and `BUS_IDLE_ST0` `0xFD8D8120` until bit 0 = 0 in both. |
| 5 | GPU `GPU_ID` `0xFB000000` | DXE reads it and logs it on UART2 (DEBUG build). |

`_PS3` runs the sequence in reverse:

1. Park the clock at 200 MHz. This moves the mux off the PVTPLL, which lives in GPU_GRF inside PD_GPU.
2. Request idle (`0x00010001`) and wait for ACK and status = 1.
3. Write PD off (`0x00010001`) and wait for BISR bit 1 = 0.
4. Gate clk_gpu, coregroup and stacks.

If the bus does not drain, `_PS3` releases the idle request and leaves the domain on. Powering off
a busy interconnect can hang the SoC. Both methods do nothing if the domain is already in the
target state, so `_PS0` keeps the driver's clock if DXE already powered the GPU. Timeouts set
sticky bits in `GERR`, which `_DSM` function 4 reports.

The driver must quiesce the GPU before D3 (halt the MCU, flush and power down L2; panthor:
`panthor_device_suspend()`). ACPI cannot do that.

### Clock choice

The boot rate is **200 MHz**, the same as Linux's `assigned-clock-rates`. At that rate TF-A uses the
GPLL divider path, not the PVTPLL. So it:

- is in spec at any vdd_gpu in the 675-850 mV range (see the OPP table below),
- does not touch GPU_GRF (`0xFD5A0000`), which needs PD_GPU on,
- does not depend on PVTPLL ring-oscillator tuning, whose output frequency depends on voltage.

`rk3588-opp.dtsi` (mainline) OPP table for the GPU:

| Frequency | Voltage |
|---|---|
| 300-600 MHz | 675 mV |
| 700 MHz | 700 mV |
| 800 MHz | 750 mV |
| 900 MHz | 800 mV |
| 1000 MHz | 850 mV |

The firmware programs 750 mV on every board except Orange Pi 5 (850 mV). The `_DSM` cap is
therefore 800 MHz by default, 1 GHz on Orange Pi 5, and 700 MHz on the Firefly ITX-3588J
(RK3588J: `rk3588j.dtsi` stops at 700 MHz). A board can override it with
`#define BOARD_GPU_MAX_FREQ_HZ` in its `Dsdt.asl`.

The vendor (BSP) kernel also tunes the GPU SRAM read margin in GPU_GRF depending on voltage
("volt-mem-read-margin"). Mainline Linux and TF-A do not, and at a fixed 750/850 mV the reset
defaults are what mainline runs with. That is left as a TODO for DVFS below 750 mV.

### `_DSM` (UUID `102bc718-f3be-4e3c-8e12-dc78892ef9a5`, revision 0)

| Function | Arg3 | Returns |
|---|---|---|
| 0 | — | `Buffer { 0x1F }` (functions 0-4 supported) |
| 1 get clock | — | Integer Hz from `\_SB.SCMI.CLRG(5)`, or 0 on SCMI error |
| 2 set clock | `Package { Hz }` | Integer: 0 on success, the SCMI status on SCMI error, or `0xFFFFFFFF` if rejected. Treat any non-zero value as failure: SCMI `NOT_SUPPORTED` (-1) is also `0xFFFFFFFF`, and a transport error is 1. A request is rejected if it is not a multiple of 100 MHz in [200 MHz, max], or if it is above 200 MHz while PD_GPU is off. |
| 3 max clock | — | Integer Hz (`BOARD_GPU_MAX_FREQ_HZ`) |
| 4 diagnostics | — | `Package { PWR_GATE_ST0, BISR_STATUS4, BUS_IDLE_ST0, BUS_IDLE_ACK0, CLKGATE_CON66, CLKGATE_CON67, GERR }` |

TODO (function 5): set vdd_gpu through `\_SB.SCMI.VDLS`. This needs a TF-A SCMI voltage domain for
RK806 BUCK1. Today `arm-trusted-firmware-patches/0006` only exposes `vccio_sd`. The sketch is in
`docs/research/platform-rk3588-windows.md` section 3.3.

## Build

Prerequisites:

- the edk2-rk3588 tree at the base commit, with submodules
- an aarch64 GCC
- iasl
- python3 with pyelftools
- dtc

This was tested with GCC 16.2.0 (`aarch64-linux-gnu-gcc` from Arch), iasl 20260930 built from
ACPICA source, and pyelftools 0.33 in a venv.

```sh
git clone https://github.com/edk2-porting/edk2-rk3588.git && cd edk2-rk3588
git checkout 2f3f8265590f77901ac6a1af5c963f957a844158
git submodule update --init --depth 1 edk2 edk2-non-osi edk2-platforms misc/rkbin \
    devicetree/mainline/upstream arm-trusted-firmware
git -C edk2 submodule update --init --recursive --depth 1
git am --keep-cr /path/to/panfrost-win/firmware/edk2-rk3588/patches/*.patch   # tree uses CRLF
./build.sh -d rock-5b                     # -> RK3588_NOR_FLASH.img
```

Workarounds for GCC >= 15 / 16. These are upstream issues, unrelated to the patches:

- TF-A: patch 0006 (`rk3588_voltd.c`) fails `-Werror=unterminated-string-initialization`. Build
  with `--tfa-flags "E=0"`.
- edk2-rockchip: `uboot-env.h` has `typedef BOOLEAN bool;`, which is a C23 keyword (GCC 15+
  defaults to gnu23). Put a wrapper that adds `-std=gnu17` first, then pass it with `CROSS_COMPILE`:

  ```sh
  mkdir xgcc && for t in /usr/bin/aarch64-linux-gnu-*; do ln -s "$t" xgcc/; done
  rm xgcc/aarch64-linux-gnu-gcc
  printf '#!/bin/sh\nexec /usr/bin/aarch64-linux-gnu-gcc -std=gnu17 "$@"\n' > xgcc/aarch64-linux-gnu-gcc
  chmod +x xgcc/aarch64-linux-gnu-gcc
  CROSS_COMPILE=$PWD/xgcc/aarch64-linux-gnu- ./build.sh -d rock-5b --tfa-flags "E=0"
  ```

- `misc/extractbl31.py` needs pyelftools. Either `pacman -S python-pyelftools`, or put a venv
  that has it first in `PATH`.

## Flash

Use the edk2-rk3588 README. The ROCK 5B has SPI NOR:

- **From Linux on the board:** `dd if=RK3588_NOR_FLASH.img of=/dev/mtdblock0`, or use the vendor
  flashing tool.
- **From another PC, in MaskROM:**
  `rkdeveloptool db rk3588_spl_loader_*.bin && rkdeveloptool wl 0 RK3588_NOR_FLASH.img`.
- **To update from the UEFI shell:** see "Flash SPI NOR from the UEFI Shell" in the upstream
  README.

Keep a known-good image and a way into MaskROM. If the board fails to boot, re-flash the
stock image.

## Verification recipe (UEFI shell, untested)

Boot to the UEFI shell. On a DEBUG build, also watch UART2 at 1.5 Mbaud: it should print
`GPU: powered on, GPU_ID=0x........ (arch 10.8..., product 7, ...) Mali-G610`. In the shell, `mm`
needs `-n` for non-interactive reads.

```
mm FD8D8290 -w 4 -MMIO -n    # PMU2 BISR_STATUS4   : bit1 = 1 -> PD_GPU on      (e.g. 0x00000002 | other PDs)
mm FD8D8180 -w 4 -MMIO -n    # PMU2 PWR_GATE_ST0   : bit0 = 0 -> PD_GPU on
mm FD8D8120 -w 4 -MMIO -n    # PMU2 BUS_IDLE_ST0   : bit0 = 0 -> GPU NIU active
mm FD8D8118 -w 4 -MMIO -n    # PMU2 BUS_IDLE_ACK0  : bit0 = 0
mm FD7C0908 -w 4 -MMIO -n    # CRU CLKGATE_CON66   : bits 1,3,4,6-14 = 0 (ungated)
mm FD7C0578 -w 4 -MMIO -n    # CRU CLKSEL_CON158   : [4:0]=5, [7:5]=0, bit14=0 -> GPLL/6 = 198 MHz
mm FB000000 -w 4 -MMIO -n    # GPU_ID  ONLY IF the four PMU checks above passed
```

The expected `GPU_ID` is `0xA8670005`: arch 10.8.6, product major 7 (Mali-G610), r0p0, status 5.
This value comes from public Linux panthor logs on RK3588 ("mali-g610 id 0xa867 ... status 0x5")
and has **not been read on hardware here**. The required part is
`(GPU_ID & 0xF00F0000) == 0xA0070000`.

Other checks:

- `GPU_ID` reads `0x00000000` or `0xFFFFFFFF` with no fault: the clock or reset is wrong.
  Check `CLKGATE_CON66` and `SOFTRST_CON66` (`mm FD7C0B08 -w 4 -MMIO -n`, bits 4, 5, 8-12, 14
  should be 0).
- In setup, *Device Manager → Rockchip Platform Configuration → GPU* toggles the feature. When it
  is Disabled, the PMU values stay at whatever BL31/BootROM left, and `\_SB.GPU0._STA` is 0.
- From Windows, Device Manager shows "Unknown device" `ACPI\RKCP7410` until panwin.sys is
  installed. Its resources should be one memory range and IRQs 124, 125 and 126.

### Safety note

**Do not read or write `0xFB000000`-`0xFB1FFFFF` unless PD_GPU is on and the GPU NIU is out of
idle.** On Rockchip, accessing a powered-down or idle domain raises an asynchronous SError or hangs
the bus. In the UEFI shell that means a crash or a frozen board that needs a power cycle. The same
applies to GPU_GRF (`0xFD5A0000`). Always check `0xFD8D8290` bit 1 = 1 and `0xFD8D8120` bit 0 = 0
first. The OS driver should do the same check (or call `_PSC` / `_DSM` function 4) before its
first register access.

## Tests

`../tests/run_aml_test.sh` compiles `Gpu.asl` with iasl and runs `_PS0`, `_PS3`, `_PSC` and all
`_DSM` functions under ACPICA `acpiexec`. The PMU/CRU memory is emulated as all zeros and preset
by the test, and `\_SB.SCMI` is a stub. The test has 31 checks covering:

- the write values,
- the timeout paths,
- the idempotence of `_PS0`/`_PS3`,
- the `_DSM` bounds.

It checks the AML control flow only, not the hardware behaviour.

```sh
IASL=/path/to/iasl ACPIEXEC=/path/to/acpiexec firmware/edk2-rk3588/tests/run_aml_test.sh \
    /path/to/edk2-rk3588/edk2-rockchip/Silicon/Rockchip/RK3588/AcpiTables/Gpu.asl
```
