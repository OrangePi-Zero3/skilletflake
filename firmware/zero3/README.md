# Orange Pi Zero3: GPU ACPI node

halal-beef's Zero3 UEFI (`github.com/OrangePi-Zero3/Mu-Silicium`, built with `UEFI-Builder`)
takes its ACPI tables as prebuilt AML from `Project-Silicium/Silicium-ACPI`
(`SoCs/Allwinner/H616/`). Its `DSDT_minimal` has no GPU, and the firmware leaves the GPU's power
domain, clocks and reset off.

`patches/0001-H616-add-Mali-G31-GPU0-node.diff` (against Silicium-ACPI 6f911ab) adds
`\_SB.GPU0`, `_HID "AWGP0031"`:

- `_CRS`: 0x01800000 + 0x40000, GSIV 127 / 128 / 129 (job, mmu, gpu), `_CCA` 0.
- `_PS0`: PPU GPU gate on, `gpu1` = periph0-2x / 3 = 400 MHz, `gpu0` from `gpu1`, bus gate on,
  reset released. `_PS3` reverses it but leaves `gpu1` running (Linux never gates it).
- `_PSC`: 0 only when the domain is on and the bus gate and reset are released. panwin.sys
  checks this before touching a GPU register.

Register facts and sources: [`docs/research/zero3-g31.md`](../../docs/research/zero3-g31.md).

Test (compiles the patched DSDT with iasl and runs the methods under acpiexec on emulated
memory; it checks the register sequence, not the hardware):

    IASL=.../iasl ACPIEXEC=.../acpiexec firmware/zero3/tests/run_aml_test.sh [path/to/Silicium-ACPI]

To use it on the board: apply the patch in the firmware's `Silicium-ACPI` submodule, recompile
`SoCs/Allwinner/H616/DSDT_minimal.dsl` to `DSDT_minimal.aml` (their README: `iasl`), rebuild
with `UEFI-Builder`, and flash. The patch is for halal-beef to take or adapt; nothing has been
sent upstream.

Not done yet: running the GPU from the GPU PLL, DVFS, and checking DCDC1's voltage covers the
G31 at 400 MHz (Linux's DT allows 810–990 mV).
