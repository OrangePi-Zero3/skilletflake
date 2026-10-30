# Status: edk2 (edk2-rk3588 GPU node + power-up)

Date: 2026-10-30. Owner key: `edk2`.

## Summary

Mali-G610 support is added to edk2-rk3588 as 2 patches on branch `panwin-gpu` in
`references/edk2-rk3588`. The base is `2f3f8265590f` (upstream master). The patches are exported to
`firmware/edk2-rk3588/patches/`.

- **ACPI device:** `\_SB.GPU0` is added to `DsdtCommon.asl`, so all 22 RK3588/RK3588S board DSDTs
  get it. It has:
  - `_HID "RKCP7410"`, `_UID 0`, `_CCA 0`
  - `_STA`, patched by AcpiPlatformDxe
  - `_CRS`: 0xFB000000/0x200000 plus Level/ActiveHigh/Exclusive interrupts 124/125/126 in the order
    job/mmu/gpu
  - `_PS0`/`_PS3`/`_PSC` driving the PMU2/CRU registers through OperationRegions
  - `_DSM`: SCMI clock get/set bounded by the board's safe maximum, plus diagnostics
- **DXE power-up:** `RK3588Dxe/Gpu.c` powers the GPU at boot, gated by a new setup option
  (*Rockchip Platform Configuration → GPU*, `GpuState`, default Enabled). The sequence:
  1. Set the clock to 200 MHz via SCMI.
  2. Ungate the CRU gates and release the resets.
  3. Power on PD_GPU, then wait for BISR.
  4. Release the NIU idle request, then wait for ACK/ST.
  5. Log GPU_ID.
- **Build:** full rock-5b and orangepi-5 firmware images (`RK3588_NOR_FLASH.img`) build with the
  system `aarch64-linux-gnu-gcc` 16.2 and iasl built from source. Two GCC 16 workarounds are
  needed, both upstream issues.
- **Hardware:** untested on hardware.

## What was built and how to build/test it

Everything lives in `firmware/edk2-rk3588/`:

- `patches/0001-RK3588-add-ACPI-device-for-the-Mali-G610-GPU.patch`: `Gpu.asl`, the
  `DsdtCommon.asl` include, and the Orange Pi 5 1 GHz cap.
- `patches/0002-RK3588Dxe-power-on-the-Mali-G610-GPU-at-boot.patch`: `Gpu.c`/`Gpu.h`, plus the
  setup page (VFR/UNI/VarStoreData/PCD) and the `_STA` fixup in AcpiPlatformDxe.
- `patches/README.md`: what the patches do, the register sequence, the clock choice, the `_DSM`
  ABI, build and flash steps, the UEFI-shell recipe and the safety note.
- `tests/gpu_aml_test.asl` and `tests/run_aml_test.sh`: an acpiexec test of the AML.

Build outputs and tools are in `references/edk2-build/`:

- `acpica/generate/unix/bin/{iasl,acpiexec}`: built from ACPICA source, e85aa3b (20260930).
- `venv/`: pyelftools 0.33.
- `xgcc/`: a gcc wrapper that adds `-std=gnu17`.
- `out/rock-5b/`: `RK3588_NOR_FLASH.img`, `rock-5b_EFI.itb`, and the built DSDT `.aml`/`.dsl`.
- `build-*.log`: the build logs.

Commands:

```sh
cd references/edk2-rk3588           # branch panwin-gpu (or apply patches with: git am --keep-cr)
B=$(realpath ../edk2-build)         # must be absolute: build.sh changes directory
PATH=$B/venv/bin:$B/acpica/generate/unix/bin:$PATH \
CROSS_COMPILE=$B/xgcc/aarch64-linux-gnu- \
  ./build.sh -d rock-5b --tfa-flags "E=0"
# AML test
B=references/edk2-build/acpica/generate/unix/bin
IASL=$B/iasl ACPIEXEC=$B/acpiexec repo/firmware/edk2-rk3588/tests/run_aml_test.sh \
  references/edk2-rk3588/edk2-rockchip/Silicon/Rockchip/RK3588/AcpiTables/Gpu.asl
```

## Verified

What I actually ran, and the result:

- **iasl:** `Gpu.asl` compiles with 0 errors and 0 warnings, standalone and inside the real
  ROCK5B DSDT (edk2 build).
  - I disassembled the built ROCK5B `Dsdt.aml`. It contains `Device (GPU0)`, `_HID "RKCP7410"`,
    `Memory32Fixed 0xFB000000/0x200000` and interrupts 0x7C/0x7D/0x7E. The `_DSM` cap is
    0x2FAF0800 (800 MHz).
- **acpiexec:** `run_aml_test.sh` passes 31/31 checks. They cover:
  - the PD-off and PD-on paths of `_PSC`, `_PS0` and `_PS3`
  - the exact PMU/CRU write values
  - the timeout paths (no idle release after a PD-on timeout; `_PS3` aborts and re-releases idle
    if the NIU does not drain)
  - idempotence (no SCMI call when the device is already in the target state)
  - the `_DSM` functions 0-4, bounds and rejects, and a bad UUID
  - The PMU/CRU memory is emulated (zeros plus values the test presets) and SCMI is a stub.
- **Interrupts:** checked against `rk3588-base.dtsi:459-462`: `GIC_SPI 92/93/94 IRQ_TYPE_LEVEL_HIGH`,
  names job/mmu/gpu, so GSIV = 124/125/126, Level, ActiveHigh. The OPP table is
  `rk3588-opp.dtsi:118-153`.
- **PMU sequence:** matches TF-A (BSD) `pmu_set_power_domain()`, `pmu_power_domain_ctr()` and
  `pmu_bus_idle_req()`:
  - PD_GPU = 0, BUS_ID_GPU = 0, PD_RPR_GPU = 1
  - `PMU_BASE` = 0xFD8D0000, registers at +0x8xxx
- **Clock:** the 200 MHz path matches TF-A `clk_gpu_set_rate()`: GPLL 1188 MHz / 6, no
  PVTPLL/GPU_GRF access.
- **Gate/reset bits:** taken from the HAL IDs in `RK3588.h` (GATE_CON66/67, SOFTRST_CON66/67).
- **Full build:** `./build.sh -d rock-5b --tfa-flags "E=0"` (with the gnu17 wrapper) completes.
  - TF-A, edk2 (including `Gpu.obj`), the FIT and the 8 MB NOR image are all produced.
  - `RK3588_NOR_FLASH.img` is 6999040 bytes, sha256 4af6718e…3993.
  - The HII strings and `GpuState` varstore are present in `RK3588Dxe.efi`.
- **Orange Pi 5:** `./build.sh -d orangepi-5 --tfa-flags "E=0"` also completes. Its DSDT `_DSM`
  cap is 0x3B9ACA00 (1 GHz), so the per-board `BOARD_GPU_MAX_FREQ_HZ` override works. Outputs are
  in `references/edk2-build/out/orangepi-5/`.
- **Patches:** both apply with `git am --keep-cr` on a fresh checkout of `2f3f826`, and the
  resulting tree is identical to the branch. They also apply with `patch -p1`.

## Not verified / needs hardware

- **Default power state:** whether PD_GPU is off at BL31 exit. If it is off, the PD-on path runs;
  otherwise only the "already on" path does. Check with
  `mm FD8D8290 -w 4 -MMIO -n` / `mm FD8D8180 ...`.
- **Whole sequence on silicon:** none of it has run on hardware. That includes:
  - poll bits and timeouts
  - the GPU_ID value (expected `0xA8670005`, taken from public panthor logs)
  - the SCMI rate set from DXE and from AML (`\_SB.SCMI.CLRS` from `_PS0`/`_PS3`)
- **Windows behaviour:** whether Windows calls `_PS3`/`_PS0` for a driverless device or under PoFx,
  and whether `_PS3`'s idle and PD-off path is clean while the GPU is quiesced.
- **Other boards:** whether every board's BUCK1 really supplies vdd_gpu at the value its
  `RockchipPlatformLib.c` sets. The 800 MHz cap relies on 750 mV.
- **QoS:** the qos_gpu_m0..m3 registers (0xFDF35000+) are not restored after PD on. Reset
  defaults are assumed fine.
- **Runtime:** no Windows boot was attempted. There is no RK3588 board here.

## Known issues & TODO (prioritised)

1. **Hardware bring-up.** Run the UEFI-shell recipe in `patches/README.md` and capture the UART
   log line `GPU: powered on, GPU_ID=...`.
2. **Default is Enabled** (as the task asked). Upstream would probably want Disabled until a
   driver ships. A driverless `ACPI\RKCP7410` shows as "Unknown device" in Windows.
3. **vdd_gpu DVFS.** Add a TF-A SCMI voltage domain for RK806 BUCK1 (extend
   `arm-trusted-firmware-patches/0006`) and `_DSM` function 5 (VDLS). Today the clock is capped by
   the fixed boot voltage.
4. **Voltage-dependent GPU SRAM read margin** (vendor-kernel "volt-mem-read-margin" in GPU_GRF).
   This is not needed at fixed 750/850 mV. It would be needed for sub-750 mV DVFS.
5. **QoS save/restore** across `_PS3`/`_PS0` (4 × 0x20 bytes at 0xFDF35000/5200/5400/5600).
6. **`_S0W` / wake:** not declared. Add it if the KMD wants runtime D3 with PoFx.
7. **GCC 16 build breakage** (upstream, not ours):
   - TF-A patch 0006 `-Werror=unterminated-string-initialization` (worked around with `E=0`)
   - `uboot-env.h` `typedef BOOLEAN bool` versus C23 (worked around with the `-std=gnu17`
     wrapper)

   Small upstream fixes are possible.
8. **HID coordination.** Coordinate `RKCP7410` with the edk2-rk3588 maintainers and with
   gahingwoo (who uses `RKCP7402` for RK3576 G52).

Packages: none was strictly required. Optional, instead of the venv and the source-built ACPICA:
`sudo pacman -S python-pyelftools acpica`.

## Requested contract changes

- Add the GPU `_DSM` ABI to `include/panwin/panwin_ioctl.h` (or a KMD-private header), so that
  kmd/win can call it with `IOCTL_ACPI_EVAL_METHOD`:
  - `PANWIN_GPU_DSM_GUID` = `102bc718-f3be-4e3c-8e12-dc78892ef9a5`, revision 0
  - functions: 1 = get clock (Hz), 2 = set clock (`Package{Hz}`; 0 ok / SCMI status /
    0xFFFFFFFF rejected), 3 = max safe clock (Hz), 4 = diagnostics (`Package{PWR_GATE_ST0,
    BISR_STATUS4, BUS_IDLE_ST0, BUS_IDLE_ACK0, CLKGATE_CON66, CLKGATE_CON67, GERR}`)
- No change is needed to `PANWIN_ACPI_HID` ("RKCP7410" matches).
- Recommendation for kmd/win: before the first MMIO access, evaluate `_PSC` (0 = safe) or
  `_DSM` function 4 (BISR bit 1 = 1 and BUS_IDLE_ST0 bit 0 = 0), instead of touching 0xFB000000
  blindly. Also expect resources in the order MEM, then IRQ job, mmu, gpu.

## Review

Independent review, 2026-10-30. Nothing here ran on hardware; it is all untested on hardware.

### What I re-ran, and the results

- **AML test, as documented:** it **failed** with 53 iasl errors (`#endif` "Unknown directive").
  The tree's `Gpu.asl` uses CRLF line endings, and iasl's own preprocessor rejects `#endif\r`.
  The original 31/31 result must have come from an LF copy. After my fix (below), it passes
  31/31, with iasl 20260930 reporting 0 errors and 0 warnings.
- **rock-5b build, as documented:** it **failed** silently. edk2 printed `- Failed -` after 5 s
  with no error message, then `build.py` hung until I killed it. The cause is the relative
  `B=../edk2-build` in `PATH`/`CROSS_COMPILE`, which stops resolving once `build.sh` changes
  directory.
- **rock-5b build, clean (`./build.sh -C`) with absolute paths:** it completed in 3m26s.
  - `RK3588_NOR_FLASH.img` is 6999040 bytes, the same size as reported. The sha256 is
    8bf2c08e…83a2; the reported hash differs because of build timestamps.
  - `Gpu.obj` was built with no `Gpu.c` warnings.
  - The disassembled DSDT has `GPU0`, `RKCP7410`, 0xFB000000, IRQs 0x7C/0x7D/0x7E, and the
    0x2FAF0800 cap.
  - The image is saved in `references/edk2-build/out/review-rock-5b/`, and the log is
    `review-rock-5b.log`.
- **itx-3588j build** (to check my fix): it completed, and the DSDT cap is 0x29B92700 (700 MHz).
  The log is `review-itx-3588j.log`. The `RK3588_NOR_FLASH.img` at the tree root is now this
  ITX-3588J image.
- **Patches:** `git am --keep-cr` of all three patches onto a fresh worktree at `2f3f826` gives
  the same tree as `panwin-gpu` (tree `a4a443a4`).

### Checked against the references (all consistent)

- **PMU2:** base 0xFD8D8000. The offsets match both TF-A `pmu.h` (BSD) and the Linux
  `rk3588_pmu` table:
  - req 0x10C, ack 0x118, idle 0x120, pwr 0x14C, status 0x180, repair 0x290
  - PD_GPU = 0, BUS_ID_GPU = 0, PD_RPR_GPU = BISR bit 1
  - Order: PD on, then idle release. Off is the reverse. Both match TF-A `pmu_set_power_domain`.
- **CRU:**
  - HAL gate IDs 1057-1075 map to CON66 bits 1,3,4,6-14 and CON67 bits 1-3.
  - The ASL masks 0x7FDA, 0xE and 0xD0 are right.
  - `CLKSEL_CON158` (0x578): div [4:0], mux [7:5] (0 = GPLL), bit 14 = PVTPLL. The direct path
    equals TF-A `clk_gpu_set_rate()` at 200 MHz (div 5).
- **TF-A SCMI:** the 200 MHz entry has length 0, so it takes the GPLL path and never touches
  GPU_GRF. The `_DSM` guard against rates above 200 MHz while PD_GPU is off is therefore needed
  and correct. `SCMI_CLK_GPU` = 5.
- **Interrupts and attributes:** GIC_SPI 92/93/94 LEVEL_HIGH, names job/mmu/gpu, so GSIV
  124/125/126. `_CCA 0` is right (no `dma-coherent` in the DT).
- **GPU_ID decode:** the 0xF00F0000/0xA0070000 mask and value are right.
- **Device mapping:** 0xF0000000-0xFFFFFFFF is mapped as device memory in `Rk3588Mem.c`.
- **License:** the new files are MIT and written fresh. No GPL-only source (Rockchip pmdomain,
  `clk-rk3588.c`) was copied; it was used only to cross-check register meaning.

### Bugs found and fixed

1. `tests/run_aml_test.sh` now strips CR from the input ASL. Without this, the documented test
   cannot pass against the tree.
2. The status build command now uses `B=$(realpath ../edk2-build)`. With the relative path, the
   documented build fails.
3. **New patch `0003-ITX-3588J-cap-the-GPU-_DSM-clock-at-700-MHz.patch`** (commit `4f9f268` on
   `panwin-gpu`). The Firefly ITX-3588J is an RK3588J, whose GPU OPP table (`rk3588j.dtsi`)
   stops at 700 MHz. The default 800 MHz cap would let `_DSM` function 2 request an
   out-of-spec rate. The patch is re-exported, and `patches/README.md` is updated: patch table,
   cap list, and the `_DSM` return codes.

### Remaining concerns, most severe first

1. **(High, needs hardware)** The whole power sequence is still unproven on silicon. This includes
   whether PD_GPU is on at hand-off, the poll bits, GPU_ID, and whether Windows ACPI.sys runs
   `_PS0`/`_PS3` the way the design assumes. If any of these is wrong, the first KMD MMIO access
   raises an SError.
2. **(Medium)** `_PS3` assumes the KMD has quiesced the GPU (MCU halted, L2 off). ACPI cannot
   enforce this. GPU QoS registers are not restored after `_PS3`/`_PS0`.
3. **(Medium)** The `_DSM` cap assumes BUCK1 = vdd_gpu at the voltage `RockchipPlatformLib.c`
   sets. I checked the `Rk806Configure` tables: BUCK1 is 750 mV on 21 boards and 850 mV on Orange Pi 5, but not the board schematics.
4. **(Low)** `_DSM` function 2 returns 0xFFFFFFFF both for "rejected" and for SCMI
   NOT_SUPPORTED (-1), and an SMT transport error returns 1. The KMD should treat any non-zero
   value as failure. This is documented in the README.
5. **(Low)** If the SCMI clock protocol is not yet installed when the DXE code runs, it falls back
   to direct CRU writes. TF-A's cached `gpu_rate` may then be stale for `CLRG`; this is cosmetic.
6. **(Low)** `GpuTryPowerOn()` logs "already on" and then runs the full sequence anyway. This is
   harmless and idempotent.

**Verdict:** the code-level claims held up: register offsets, bits, IRQs, clock path and DSDT
contents. The reproducibility claims did not: the AML test and the build both fail when run
exactly as documented. Both are fixed now.
