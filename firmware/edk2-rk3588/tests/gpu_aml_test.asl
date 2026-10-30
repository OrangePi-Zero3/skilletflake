DefinitionBlock ("gpu_aml_test.aml", "DSDT", 2, "RKCP  ", "RK3588  ", 2)
{
  Scope (\_SB_) {
    Device (SCMI) {
      Name (_HID, "PNP0C02")
      Name (LRAT, 0)
      Name (NCLS, 0)
      Method (CLRG, 1, Serialized) { Return (Package (2) { 0, 200000000 }) }
      Method (CLRS, 2, Serialized) { LRAT = Arg1; NCLS++; Return (Package (1) { 0 }) }
    }
#include "Gpu.asl"
  }
  Method (CHK, 3) {
    If (Arg1 != Arg2) { Concatenate ("FAIL ", Arg0, Local0); Debug = Local0; Debug = Arg1; \FAIL++ }
    Else { Concatenate ("ok   ", Arg0, Local0); Debug = Local0 }
  }
  Name (FAIL, 0)
  Name (U, ToUUID ("102bc718-f3be-4e3c-8e12-dc78892ef9a5"))
  Method (MAIN, 0, Serialized) {
    // --- scenario 1: all zeros = PD off, PD never comes up
    CHK ("psc off", \_SB.GPU0._PSC (), 3)
    \_SB.GPU0._PS0 ()
    CHK ("ps0 scmi 200M", \_SB.SCMI.LRAT, 200000000)
    CHK ("ps0 pgcn on", \_SB.GPU0.PGCN, 0x00010000)
    CHK ("ps0 gt66", \_SB.GPU0.GT66, 0x7FDA0000)
    CHK ("ps0 gt67", \_SB.GPU0.GT67, 0x000E0000)
    CHK ("ps0 gerr timeout", \_SB.GPU0.GERR, 1)
    CHK ("ps0 no idle release on failure", \_SB.GPU0.IREQ, 0)
    CHK ("dsm set 300M while off rejected", \_SB.GPU0._DSM (U, 0, 2, Package () { 300000000 }), 0xFFFFFFFF)
    // --- scenario 2: PD on
    \_SB.GPU0.BISR = 2
    \_SB.GPU0.GERR = 0
    CHK ("psc on", \_SB.GPU0._PSC (), 0)
    \_SB.SCMI.NCLS = 0
    \_SB.GPU0._PS0 ()
    CHK ("ps0 when on: no scmi", \_SB.SCMI.NCLS, 0)
    CHK ("dsm 0", DerefOf (\_SB.GPU0._DSM (U, 0, 0, Package () {})[0]), 0x1F)
    CHK ("dsm get", \_SB.GPU0._DSM (U, 0, 1, Package () {}), 200000000)
    CHK ("dsm set 300M", \_SB.GPU0._DSM (U, 0, 2, Package () { 300000000 }), 0)
    CHK ("dsm set 300M scmi", \_SB.SCMI.LRAT, 300000000)
    CHK ("dsm set 800M", \_SB.GPU0._DSM (U, 0, 2, Package () { 800000000 }), 0)
    CHK ("dsm set 900M rejected", \_SB.GPU0._DSM (U, 0, 2, Package () { 900000000 }), 0xFFFFFFFF)
    CHK ("dsm set 250M rejected", \_SB.GPU0._DSM (U, 0, 2, Package () { 250000000 }), 0xFFFFFFFF)
    CHK ("dsm set 100M rejected", \_SB.GPU0._DSM (U, 0, 2, Package () { 100000000 }), 0xFFFFFFFF)
    CHK ("dsm max", \_SB.GPU0._DSM (U, 0, 3, Package () {}), 800000000)
    Local0 = \_SB.GPU0._DSM (U, 0, 4, Package () {})
    CHK ("dsm diag count", SizeOf (Local0), 7)
    CHK ("dsm diag bisr", DerefOf (Local0[1]), 2)
    CHK ("dsm bad uuid", DerefOf (\_SB.GPU0._DSM (ToUUID ("00000000-0000-0000-0000-000000000000"), 0, 0, Package () {})[0]), 0)
    // --- scenario 3: PS3 with NIU never acking idle
    \_SB.GPU0._PS3 ()
    CHK ("ps3 idle timeout", \_SB.GPU0.GERR, 4)
    CHK ("ps3 idle released after timeout", \_SB.GPU0.IREQ, 0x00010000)
    CHK ("ps3 pd not switched off", \_SB.GPU0.PGCN, 0x00010000)
    // --- scenario 4: PS3 with idle acked, PD never reports off
    \_SB.GPU0.GERR = 0
    \_SB.GPU0.IACK = 1
    \_SB.GPU0.IDST = 1
    CHK ("psc idle = off", \_SB.GPU0._PSC (), 3)
    \_SB.GPU0._PS3 ()
    CHK ("ps3 pd off written", \_SB.GPU0.PGCN, 0x00010001)
    CHK ("ps3 idle requested", \_SB.GPU0.IREQ, 0x00010001)
    CHK ("ps3 pd off timeout", \_SB.GPU0.GERR, 8)
    CHK ("ps3 core gated", \_SB.GPU0.GT66, 0x00D000D0)
    // --- scenario 5: PS3 when already off is a no-op
    \_SB.GPU0.BISR = 0
    \_SB.GPU0.GERR = 0
    \_SB.SCMI.NCLS = 0
    \_SB.GPU0._PS3 ()
    CHK ("ps3 off no-op", \_SB.SCMI.NCLS, 0)
    Debug = "FAILURES:"
    Debug = FAIL
  }
}
