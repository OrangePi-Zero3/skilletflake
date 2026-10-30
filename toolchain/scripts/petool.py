#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
petool.py - small, dependency-free PE helper for the panfrost-win kernel toolchain.

Subcommands
  approx-defs   Build the APPROXIMATE ARM64 ntoskrnl/hal .def files from mingw-w64's
                lib64 + libarm32 .def files (what we use until we have real ARM64 binaries).
  defs-from-pe  Build .def files from REAL binaries (ntoskrnl.exe, hal.dll, WDFLDR.SYS copied
                from the target ARM64 Windows install). This is the day-one hardware task.
  exports       Print the export names of a PE image (sanity check / diffing).
  check-sys     Verify a built driver: ARM64, native subsystem, WDM_DRIVER, exception table,
                non-zero checksum, imports only from allowed DLLs, and every imported symbol
                present in our .def files. Exits non-zero on any error.

Run with `python3 -I toolchain/scripts/petool.py <subcommand> --help`.
"""
import argparse
import os
import struct
import sys

IMAGE_FILE_MACHINE_ARM64 = 0xAA64
MACHINE_NAMES = {0xAA64: "ARM64", 0x8664: "AMD64", 0x14C: "I386", 0x1C4: "ARMNT"}
IMAGE_SUBSYSTEM_NATIVE = 1
IMAGE_DLLCHARACTERISTICS_WDM_DRIVER = 0x2000

# Exports that are variables, not functions: import libs must mark them DATA, otherwise the
# linker would generate a code thunk for them. Carried over from mingw-w64 .def files when
# available; this list is the fallback for names that only appear in a real binary.
KNOWN_DATA = {
    "CcFastMdlReadWait", "ExEventObjectType", "ExSemaphoreObjectType", "ExWindowStationObjectType",
    "ExDesktopObjectType", "HalDispatchTable", "HalPrivateDispatchTable", "InitSafeBootMode",
    "IoAdapterObjectType", "IoDeviceObjectType", "IoDriverObjectType", "IoFileObjectType",
    "IoReadOperationCount", "IoReadTransferCount", "IoStatisticsLock", "IoWriteOperationCount",
    "IoWriteTransferCount", "KdComPortInUse", "KdDebuggerEnabled", "KdDebuggerNotPresent",
    "KdEnteredDebugger", "KeLoaderBlock", "KeNumberProcessors", "KeServiceDescriptorTable",
    "KeTickCount", "LpcPortObjectType", "MmBadPointer", "MmHighestUserAddress",
    "MmSectionObjectType", "MmSystemRangeStart", "MmUserProbeAddress", "NlsAnsiCodePage",
    "NlsMbCodePageTag", "NlsMbOemCodePageTag", "NlsOemLeadByteInfo", "PsInitialSystemProcess",
    "PsJobType", "PsProcessType", "PsThreadType", "SeExports", "SePublicDefaultDacl",
    "SeSystemDefaultDacl", "SeTokenObjectType", "TmEnlistmentObjectType",
    "TmResourceManagerObjectType", "TmTransactionManagerObjectType", "TmTransactionObjectType",
    "PsLoadedModuleList", "KiBugCheckData", "KdDebuggerDataBlock", "IoBatteryDeviceType",
    "ExRaiseHardErrorLock", "POGOBuffer", "PsUILanguageComitted", "ExActivationObjectType",
}

# Imports that must never appear in an ARM64 driver built with our headers: on ARM64 the WDK
# inlines these (or they do not exist), so an import would make the image fail to load.
FORBIDDEN_IMPORTS = {
    "KeGetCurrentIrql": "inline (KPCR via x18) in the ARM64 WDK, export unverified",
    "KfRaiseIrql": "inline in the ARM64 WDK, export unverified",
    "KeLowerIrql": "inline in the ARM64 WDK, export unverified",
    "KfLowerIrql": "inline in the ARM64 WDK, export unverified",
    "KeRaiseIrqlToDpcLevel": "inline in the ARM64 WDK, export unverified",
    "KeGetCurrentThread": "inline in the ARM64 WDK, export unverified",
    "KeGetCurrentProcessorNumber": "inline in the ARM64 WDK, export unverified",
    "__chkstk": "a stack frame > 4 KiB was generated; kernel stacks are small, shrink it",
}
FORBIDDEN_PREFIXES = ("READ_REGISTER_", "WRITE_REGISTER_", "READ_PORT_", "WRITE_PORT_")

DEFAULT_ALLOWED_DLLS = ("ntoskrnl.exe", "hal.dll", "wdfldr.sys")


class PEError(Exception):
    pass


class PE:
    """Minimal PE32/PE32+ reader: headers, sections, exports, imports."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if len(d) < 0x40 or d[:2] != b"MZ":
            raise PEError(f"{path}: not an MZ image")
        (lfanew,) = struct.unpack_from("<I", d, 0x3C)
        if d[lfanew:lfanew + 4] != b"PE\0\0":
            raise PEError(f"{path}: no PE signature")
        coff = lfanew + 4
        (self.machine, nsect, _ts, _sym, _nsym, opt_size, self.characteristics) = \
            struct.unpack_from("<HHIIIHH", d, coff)
        opt = coff + 20
        (magic,) = struct.unpack_from("<H", d, opt)
        if magic == 0x20B:
            self.pe32plus = True
            dd_off, nrva_off = opt + 112, opt + 108
        elif magic == 0x10B:
            self.pe32plus = False
            dd_off, nrva_off = opt + 96, opt + 92
        else:
            raise PEError(f"{path}: unknown optional header magic {magic:#x}")
        (self.checksum,) = struct.unpack_from("<I", d, opt + 64)
        (self.subsystem, self.dll_characteristics) = struct.unpack_from("<HH", d, opt + 68)
        (nrva,) = struct.unpack_from("<I", d, nrva_off)
        self.dirs = [struct.unpack_from("<II", d, dd_off + 8 * i) for i in range(min(nrva, 16))]
        self.sections = []
        sh = opt + opt_size
        for i in range(nsect):
            name, vsize, va, rawsize, rawptr = struct.unpack_from("<8sIIII", d, sh + 40 * i)
            self.sections.append((name.rstrip(b"\0").decode("ascii", "replace"),
                                  va, max(vsize, rawsize), rawptr, rawsize))

    def directory(self, idx):
        return self.dirs[idx] if idx < len(self.dirs) else (0, 0)

    def off(self, rva):
        for _n, va, size, rawptr, rawsize in self.sections:
            if va <= rva < va + size:
                if rva - va >= rawsize:
                    raise PEError(f"RVA {rva:#x} is in uninitialised data")
                return rawptr + (rva - va)
        raise PEError(f"RVA {rva:#x} outside all sections")

    def cstr(self, rva):
        o = self.off(rva)
        e = self.data.index(b"\0", o)
        return self.data[o:e].decode("ascii", "replace")

    def exports(self):
        """[(name, forwarder_or_None)] for named exports."""
        rva, size = self.directory(0)
        if not rva:
            return []
        o = self.off(rva)
        (_c, _t, _maj, _min, _name, _base, nfunc, nnames, afuncs, anames, aords) = \
            struct.unpack_from("<IIHHIIIIIII", self.data, o)
        out = []
        for i in range(nnames):
            (nrva,) = struct.unpack_from("<I", self.data, self.off(anames + 4 * i))
            (ordi,) = struct.unpack_from("<H", self.data, self.off(aords + 2 * i))
            if ordi >= nfunc:
                continue
            (frva,) = struct.unpack_from("<I", self.data, self.off(afuncs + 4 * ordi))
            fwd = self.cstr(frva) if rva <= frva < rva + size else None
            out.append((self.cstr(nrva), fwd))
        return out

    def imports(self):
        """{dll_name: [symbol names or '#ordinal']}"""
        rva, _size = self.directory(1)
        res = {}
        if not rva:
            return res
        o = self.off(rva)
        tsz, ordflag = (8, 1 << 63) if self.pe32plus else (4, 1 << 31)
        tfmt = "<Q" if self.pe32plus else "<I"
        while True:
            oft, _ts, _fc, name, ft = struct.unpack_from("<IIIII", self.data, o)
            if not (oft or name or ft):
                break
            dll = self.cstr(name)
            syms = []
            t = oft or ft
            while True:
                (v,) = struct.unpack_from(tfmt, self.data, self.off(t))
                if not v:
                    break
                if v & ordflag:
                    syms.append(f"#{v & 0xFFFF}")
                else:
                    syms.append(self.cstr((v & 0x7FFFFFFF) + 2))
                t += tsz
            res[dll] = syms
            o += 20
        return res


# ---------------------------------------------------------------------------- .def helpers

def read_def(path):
    """{name: is_data} from a .def file (ignores LIBRARY/EXPORTS/comments/ordinal-only)."""
    out = {}
    with open(path) as f:
        for line in f:
            line = line.split(";", 1)[0].strip()
            if not line or line.startswith("LIBRARY") or line == "EXPORTS":
                continue
            name = line.split()[0]
            if "=" in name:  # alias=target
                name = name.split("=", 1)[0]
            if name.startswith("ord_"):
                continue
            out[name] = " DATA" in f" {line} "
    return out


def def_library(path):
    with open(path) as f:
        for line in f:
            s = line.strip()
            if s.upper().startswith("LIBRARY"):
                return s.split(None, 1)[1].strip().strip('"')
    return None


def write_def(path, dll, names, header):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w") as f:
        for h in header:
            f.write(f"; {h}\n")
        f.write(f'LIBRARY "{dll}"\nEXPORTS\n')
        for n in sorted(names, key=lambda s: (s.lower(), s)):
            f.write(n + (" DATA" if names[n] else "") + "\n")


# ---------------------------------------------------------------------------- subcommands

def cmd_approx_defs(a):
    crt = a.mingw_crt
    for lib, dll in (("ntoskrnl", "ntoskrnl.exe"), ("hal", "hal.dll")):
        merged = read_def(os.path.join(crt, "lib64", f"{lib}.def"))
        merged.update(read_def(os.path.join(crt, "libarm32", f"{lib}.def")))
        out = os.path.join(a.out, f"{lib}.def")
        write_def(out, dll, merged, [
            f"ARM64 APPROXIMATION of {dll} exports: union of mingw-w64 lib64 + libarm32 {lib}.def.",
            "NOT generated from a real ARM64 binary. Regenerate with",
            "  petool.py defs-from-pe --pe <ntoskrnl.exe> --pe <hal.dll> --pe <WDFLDR.SYS>",
            "as soon as the target's binaries are available (see toolchain/README.md).",
        ])
        print(f"wrote {out} ({len(merged)} names)")


def cmd_defs_from_pe(a):
    data_hint = {n: True for n in KNOWN_DATA}
    for d in a.data_from or []:
        data_hint.update({k: v for k, v in read_def(d).items() if v})
    for path in a.pe:
        pe = PE(path)
        if pe.machine != IMAGE_FILE_MACHINE_ARM64 and not a.any_machine:
            raise SystemExit(f"{path}: machine {MACHINE_NAMES.get(pe.machine, hex(pe.machine))}, "
                             "expected ARM64 (use --any-machine to override)")
        exp = pe.exports()
        if not exp:
            raise SystemExit(f"{path}: no named exports")
        base = os.path.basename(path)
        lower = base.lower()
        dll = {"ntoskrnl.exe": "ntoskrnl.exe", "hal.dll": "hal.dll",
               "wdfldr.sys": "WDFLDR.SYS"}.get(lower, base)
        stem = {"ntoskrnl.exe": "ntoskrnl", "hal.dll": "hal", "wdfldr.sys": "wdfldr"}.get(
            lower, os.path.splitext(lower)[0])
        names = {n: data_hint.get(n, False) for n, _f in exp}
        nfwd = sum(1 for _n, f in exp if f)
        out = os.path.join(a.out, f"{stem}.def")
        write_def(out, dll, names, [
            f"Generated by petool.py defs-from-pe from {base} "
            f"(machine {MACHINE_NAMES.get(pe.machine, hex(pe.machine))}, {len(names)} names, "
            f"{nfwd} forwarders).",
            "DATA flags come from mingw-w64 .def files / petool KNOWN_DATA (PE exports do not record them).",
        ])
        print(f"wrote {out} ({len(names)} names, {nfwd} forwarded)")


def cmd_exports(a):
    pe = PE(a.pe)
    for n, f in pe.exports():
        print(n if not f else f"{n} -> {f}")


def cmd_check_sys(a):
    errs, warns = [], []
    pe = PE(a.sys)
    m = MACHINE_NAMES.get(pe.machine, hex(pe.machine))
    if pe.machine != IMAGE_FILE_MACHINE_ARM64:
        errs.append(f"machine is {m}, expected ARM64")
    if pe.subsystem != IMAGE_SUBSYSTEM_NATIVE:
        errs.append(f"subsystem {pe.subsystem}, expected NATIVE (1)")
    if not pe.dll_characteristics & IMAGE_DLLCHARACTERISTICS_WDM_DRIVER:
        errs.append("IMAGE_DLLCHARACTERISTICS_WDM_DRIVER not set")
    exc_rva, exc_size = pe.directory(3)
    if not exc_size:
        errs.append("no exception table (.pdata): build with -funwind-tables")
    if not pe.checksum:
        errs.append("PE checksum is 0 (link with /release)")
    for name, va, size, _rp, _rs in pe.sections:
        if va % 0x1000:
            errs.append(f"section {name} not 4 KiB aligned")

    defs = {}
    for d in a.defs:
        lib = (def_library(d) or os.path.basename(d)).lower()
        defs[lib] = read_def(d)
    approx = {}  # per-DLL names known from a real ARM (32-bit) binary
    for d in a.approx_ref or []:
        lib = (def_library(d) or os.path.basename(d)).lower()
        approx.setdefault(lib, {}).update(read_def(d))
    allowed = {x.lower() for x in (a.allow or DEFAULT_ALLOWED_DLLS)}

    imports = pe.imports()
    total = 0
    for dll, syms in sorted(imports.items()):
        key = dll.lower()
        if key not in allowed:
            errs.append(f"imports from disallowed DLL {dll}")
        known = defs.get(key)
        for s in syms:
            total += 1
            if s in FORBIDDEN_IMPORTS:
                errs.append(f"{dll}!{s} must not be imported: {FORBIDDEN_IMPORTS[s]}")
            elif s.startswith(FORBIDDEN_PREFIXES):
                errs.append(f"{dll}!{s} must not be imported: register/port accessors are inline on ARM64")
            if known is None:
                errs.append(f"{dll}!{s}: no .def file for {dll}")
            elif s not in known:
                errs.append(f"{dll}!{s} is not in the .def for {dll}")
            elif key in approx and s not in approx[key]:
                warns.append(f"{dll}!{s} is only known from x64 (not in the ARM32 list): verify on ARM64")
    if a.verbose:
        print(f"{a.sys}: machine {m}, subsystem {pe.subsystem}, dllchars {pe.dll_characteristics:#06x}, "
              f"checksum {pe.checksum:#x}, exception table {exc_size:#x} bytes")
        for dll, syms in sorted(imports.items()):
            print(f"  {dll}: {len(syms)} imports")
            for s in syms:
                print(f"    {s}")
    for w in warns:
        print(f"warning: {w}")
    for e in errs:
        print(f"error: {e}")
    print(f"{os.path.basename(a.sys)}: {total} imports from {len(imports)} DLLs, "
          f"{len(errs)} errors, {len(warns)} warnings")
    return 1 if errs else 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("approx-defs", help="approximate ARM64 defs from mingw-w64 defs")
    s.add_argument("--mingw-crt", required=True, help="path to mingw-w64/mingw-w64-crt")
    s.add_argument("--out", required=True)
    s.set_defaults(fn=cmd_approx_defs)

    s = sub.add_parser("defs-from-pe", help=".def files from real binaries")
    s.add_argument("--pe", action="append", required=True, help="ntoskrnl.exe / hal.dll / WDFLDR.SYS")
    s.add_argument("--out", required=True)
    s.add_argument("--data-from", action="append", help="existing .def to copy DATA flags from")
    s.add_argument("--any-machine", action="store_true", help="accept non-ARM64 images (testing)")
    s.set_defaults(fn=cmd_defs_from_pe)

    s = sub.add_parser("exports", help="list exports of a PE image")
    s.add_argument("pe")
    s.set_defaults(fn=cmd_exports)

    s = sub.add_parser("check-sys", help="verify a built .sys")
    s.add_argument("sys")
    s.add_argument("--def", dest="defs", action="append", required=True)
    s.add_argument("--approx-ref", action="append",
                   help="ARM32 .def(s): warn for imports only known from x64")
    s.add_argument("--allow", action="append", help="allowed import DLL (repeatable)")
    s.add_argument("-v", "--verbose", action="store_true")
    s.set_defaults(fn=cmd_check_sys)

    a = p.parse_args()
    try:
        return a.fn(a) or 0
    except PEError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
