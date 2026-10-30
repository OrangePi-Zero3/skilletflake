#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# usage: seh_scopes.py <image.sys> <image.map> [function ...]
# Decode ARM64 .pdata/.xdata C_SCOPE_TABLEs of a PE image (for verifying clang SEH lowering).
import struct, sys
data = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', data, 0x3c)[0]
nsec = struct.unpack_from('<H', data, pe + 6)[0]
optsz = struct.unpack_from('<H', data, pe + 20)[0]
opt = pe + 24
base = struct.unpack_from('<Q', data, opt + 24)[0]
secs = []
for i in range(nsec):
    o = opt + optsz + 40 * i
    name = data[o:o+8].rstrip(b'\0').decode()
    vsz, va, rsz, rptr = struct.unpack_from('<IIII', data, o + 8)
    secs.append((name, va, vsz, rptr))
def rva2off(r):
    for n, va, vsz, rp in secs:
        if va <= r < va + vsz:
            return rp + r - va
    raise ValueError(hex(r))
exc_rva, exc_sz = struct.unpack_from('<II', data, opt + 112 + 3 * 8)
syms = {}
for line in open(sys.argv[2]):
    p = line.split()
    if len(p) >= 3 and p[0].startswith('0001:'):
        try: syms[int(p[2], 16) - base] = p[1]
        except ValueError: pass
want = sys.argv[3:]
for i in range(exc_sz // 8):
    begin, unw = struct.unpack_from('<II', data, rva2off(exc_rva) + 8 * i)
    name = syms.get(begin, '?')
    if want and name not in want:
        continue
    if unw & 3:
        continue  # packed, no handler
    o = rva2off(unw)
    h = struct.unpack_from('<I', data, o)[0]
    flen = (h & 0x3ffff) * 4; X = (h >> 20) & 1; E = (h >> 21) & 1
    ecount = (h >> 22) & 0x1f; cwords = h >> 27; p = o + 4
    if ecount == 0 and cwords == 0:
        ext = struct.unpack_from('<I', data, p)[0]; ecount = ext & 0xffff; cwords = (ext >> 16) & 0xff; p += 4
    if not E:
        p += 4 * ecount
    p += 4 * cwords
    if not X:
        print(name, 'no handler'); continue
    hrva = struct.unpack_from('<I', data, p)[0]; cnt = struct.unpack_from('<I', data, p + 4)[0]
    print(f'{name} @0x{base+begin:x} len {flen}: handler 0x{base+hrva:x}, {cnt} scope(s)')
    for k in range(cnt):
        b, e, hd, jt = struct.unpack_from('<IIII', data, p + 8 + 16 * k)
        print(f'   try [0x{base+b:x}, 0x{base+e:x}) filter {hd:#x} -> except at 0x{base+jt:x}')
