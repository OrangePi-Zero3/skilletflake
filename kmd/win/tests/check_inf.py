#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Static checks of panwin.inf and panwin-zero3.inf (no Windows tools involved).

  check_inf.py kmd/win/panwin.inf kmd/win/panwin-zero3.inf

- every section referenced by CopyFiles/AddReg/AddService/KmdfService and every %string% exists;
- every file in a CopyFiles section is in [SourceDisksFiles];
- the G31 (ACPI AWGP0031) models map to Panwin_Device_Zero3 in both files and their sections are
  identical in both files;
- the G31 sections copy no firmware and turn the RK3588 PMU check off;
- panwin-zero3.inf does not mention mali_csffw.bin at all, panwin.inf still installs it for the
  RK3588 (RKCP7410) exactly as before.
This catches editing mistakes only; whether Windows accepts the INFs is UNTESTED here.
"""
import re
import sys

failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)


def strip_comment(line):
    out, q = [], False
    for c in line:
        if c == '"':
            q = not q
        if c == ';' and not q:
            break
        out.append(c)
    return ''.join(out).strip()


def parse(path):
    sections, order, cur = {}, [], None
    with open(path, encoding='utf-8') as f:
        for raw in f:
            line = strip_comment(raw.rstrip('\n'))
            if not line:
                continue
            m = re.fullmatch(r'\[([^\]]+)\]', line)
            if m:
                cur = m.group(1)
                if cur.lower() in (s.lower() for s in sections):
                    fail(f"{path}: duplicate section [{cur}]")
                sections[cur] = []
                order.append(cur)
                continue
            if cur is None:
                fail(f"{path}: line outside a section: {line}")
                continue
            sections[cur].append(line)
    return sections


def sec(secs, name):
    for k, v in secs.items():
        if k.lower() == name.lower():
            return v
    return None


def kv(line):
    if '=' not in line:
        return None, line
    k, v = line.split('=', 1)
    return k.strip(), v.strip()


def check_file(path, secs):
    strings = {k.lower() for k, _ in (kv(l) for l in sec(secs, 'Strings') or []) if k}
    disk_files = {kv(l)[0].lower() for l in sec(secs, 'SourceDisksFiles') or []}
    for name, lines in secs.items():
        for line in lines:
            for s in re.findall(r'%([A-Za-z_][A-Za-z0-9_]*)%', line):
                if s.lower() not in strings:
                    fail(f"{path}: [{name}] uses undefined %{s}%")
            k, v = kv(line)
            if not k:
                continue
            if k.lower() in ('copyfiles', 'addreg'):
                for ref in (x.strip() for x in v.split(',')):
                    if ref.startswith('@'):
                        continue
                    if sec(secs, ref) is None:
                        fail(f"{path}: [{name}] {k} references missing [{ref}]")
                    elif k.lower() == 'copyfiles':
                        for fl in sec(secs, ref):
                            if fl.lower() not in disk_files:
                                fail(f"{path}: [{ref}] copies {fl}, not in [SourceDisksFiles]")
            if k.lower() == 'addservice':
                parts = [x.strip() for x in v.split(',')]
                if len(parts) < 3 or sec(secs, parts[2]) is None:
                    fail(f"{path}: [{name}] AddService references a missing section")
            if k.lower() == 'kmdfservice':
                parts = [x.strip() for x in v.split(',')]
                if len(parts) < 2 or sec(secs, parts[1]) is None:
                    fail(f"{path}: [{name}] KmdfService references a missing section")
    models = sec(secs, 'Panwin.NTarm64.10.0...19041') or []
    g31 = {kv(l)[1] for l in models if 'AWGP' in l.upper()}
    want = {'Panwin_Device_Zero3, ACPI\\AWGP0031', 'Panwin_Device_Zero3, ACPI\\VEN_AWGP&DEV_0031'}
    if g31 != want:
        fail(f"{path}: G31 models are {sorted(g31)}, expected {sorted(want)}")
    copy = kv((sec(secs, 'Panwin_Device_Zero3.NT') or ['='])[0])[1]
    if 'fw' in copy.lower():
        fail(f"{path}: the G31 install section copies firmware ({copy})")
    hw = sec(secs, 'Panwin_Zero3_HW_AddReg') or []
    if 'HKR,,Rk3588PmuCheck,0x00010001,0' not in hw:
        fail(f"{path}: the G31 HW section does not set Rk3588PmuCheck = 0")


G31_SECTIONS = ['Panwin_Device_Zero3.NT', 'Panwin_Device_Zero3.NT.HW', 'Panwin_Zero3_HW_AddReg',
                'Panwin_Device_Zero3.NT.Services', 'Panwin_Device_Zero3.NT.Wdf', 'Panwin_Service',
                'Panwin_WdfSect', 'Panwin_CopyFiles']


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    full_path, z3_path = sys.argv[1], sys.argv[2]
    full, z3 = parse(full_path), parse(z3_path)
    check_file(full_path, full)
    check_file(z3_path, z3)
    for name in G31_SECTIONS:
        if sec(full, name) != sec(z3, name):
            fail(f"[{name}] differs between {full_path} and {z3_path}")
    with open(z3_path, encoding='utf-8') as f:
        if any('mali_csffw' in strip_comment(l).lower() for l in f):
            fail(f"{z3_path}: mentions mali_csffw.bin")
    # RK3588 (CSF) install path of panwin.inf: unchanged.
    rk = sec(full, 'Panwin_Device.NT') or []
    if rk != ['CopyFiles = Panwin_CopyFiles, Panwin_FwFiles']:
        fail(f"{full_path}: RK3588 install section changed: {rk}")
    if sec(full, 'Panwin_FwFiles') != ['mali_csffw.bin']:
        fail(f"{full_path}: [Panwin_FwFiles] changed")
    hw = sec(full, 'Panwin_HW_AddReg') or []
    for want in ('HKR,,BootFirmware,0x00010001,0', 'HKR,,Rk3588PmuCheck,0x00010001,1'):
        if want not in hw:
            fail(f"{full_path}: [Panwin_HW_AddReg] lost '{want}'")
    rkmodels = [l for l in sec(full, 'Panwin.NTarm64.10.0...19041') or [] if 'RKCP' in l]
    if rkmodels != ['%DeviceDesc% = Panwin_Device, ACPI\\RKCP7410',
                    '%DeviceDesc% = Panwin_Device, ACPI\\VEN_RKCP&DEV_7410']:
        fail(f"{full_path}: RK3588 models changed: {rkmodels}")
    print(f"check_inf: {'FAIL' if failures else 'OK'} ({len(failures)} problem(s))")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
