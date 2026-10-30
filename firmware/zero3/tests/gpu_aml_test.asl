// SPDX-License-Identifier: MIT
// Loaded by acpiexec next to the patched DSDT. Emulated memory starts at zero.
DefinitionBlock ("gpu_aml_test.aml", "SSDT", 2, "PANWIN", "GPUTEST ", 1)
{
  External (\_SB.GPU0, DeviceObj)
  External (\_SB.GPU0._PSC, MethodObj)
  External (\_SB.GPU0._PS0, MethodObj)
  External (\_SB.GPU0._PS3, MethodObj)
  External (\_SB.GPU0.GCK0, FieldUnitObj)
  External (\_SB.GPU0.GCK1, FieldUnitObj)
  External (\_SB.GPU0.GBGR, FieldUnitObj)
  External (\_SB.GPU0.PGAT, FieldUnitObj)
  Name (FAIL, 0)
  Method (CHK, 3) {
    If (Arg1 != Arg2) { Concatenate ("FAIL ", Arg0, Local0); Debug = Local0; Debug = Arg1; FAIL++ }
    Else { Concatenate ("ok   ", Arg0, Local0); Debug = Local0 }
  }
  Method (MAIN, 0, Serialized) {
    // Reset state of the SoC: PPU gate set (GPU off), gpu1 M = 0, other bits we must keep.
    \_SB.GPU0.PGAT = 0x00000003
    \_SB.GPU0.GCK1 = 0x00000001
    \_SB.GPU0.GCK0 = 0x00000302
    \_SB.GPU0.GBGR = 0x00000000
    CHK ("psc off", \_SB.GPU0._PSC (), 3)
    \_SB.GPU0._PS0 ()
    CHK ("ps0 ppu on, bit 1 kept", \_SB.GPU0.PGAT, 0x00000002)
    CHK ("ps0 gpu1 400MHz gated on", \_SB.GPU0.GCK1, 0x80000002)
    CHK ("ps0 gpu0 <- gpu1, M=0, on", \_SB.GPU0.GCK0, 0x81000300)
    CHK ("ps0 bus gate + reset released", \_SB.GPU0.GBGR, 0x00010001)
    CHK ("psc on", \_SB.GPU0._PSC (), 0)
    \_SB.GPU0._PS3 ()
    CHK ("ps3 reset + bus gate", \_SB.GPU0.GBGR, 0)
    CHK ("ps3 gpu0 off", \_SB.GPU0.GCK0, 0x01000300)
    CHK ("ps3 gpu1 stays on", \_SB.GPU0.GCK1, 0x80000002)
    CHK ("ps3 ppu off", \_SB.GPU0.PGAT, 0x00000003)
    CHK ("psc off again", \_SB.GPU0._PSC (), 3)
    \_SB.GPU0._PS0 ()
    CHK ("ps0 again", \_SB.GPU0._PSC (), 0)
    Debug = "FAILURES:"
    Debug = FAIL
  }
}
