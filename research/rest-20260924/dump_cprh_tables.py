#!/usr/bin/env python3
"""Dump SDM660/SDM630 CPRh KBSS + CPR4 fuse parameter tables from the stock symbolized kernel ELF.
Downstream layout (msm-4.4 cprh-kbss-regulator.c): struct cpr3_fuse_param {u32 row, bit_start, bit_end};
arrays are {row,start,end} lists terminated by {0,0,0}."""
import struct, sys
ELF, KSYMS = sys.argv[1], sys.argv[2]
segs = [(0xffffff8008080000, 0x240, 0x18268c8), (0xffffff8009f2f710, 0x1826b08, 0x3408f8)]
data = open(ELF, 'rb').read()
def rd(va, n):
    for base, off, sz in segs:
        if base <= va < base + sz:
            return data[off + va - base: off + va - base + n]
    raise ValueError(hex(va))
syms = []
for line in open(KSYMS):
    p = line.split()
    if len(p) >= 3: syms.append((int(p[0], 16), p[1], p[2]))
syms.sort()
addr = {n: a for a, t, n in syms}
def size(name):
    a = addr[name]
    for b, t, n in syms:
        if b > a: return b - a
want = [n for n in addr if (n.startswith('sdm660_') or n.startswith('sdm630_') or n.startswith('cprh_sdm6') or n.startswith('kbss_speed')) and ('param' in n or 'volt' in n or 'corner_name' in n)]
for n in sorted(want, key=lambda x: addr[x]):
    a, s = addr[n], size(n)
    raw = rd(a, s)
    print(f"== {n} @ {a:#x} size {s}")
    if 'corner_name' in n:
        ptrs = struct.unpack('<%dQ' % (s // 8), raw[: s // 8 * 8])
        names = []
        for p in ptrs:
            try:
                b = rd(p, 32); names.append(b.split(b'\0')[0].decode())
            except Exception: names.append(hex(p))
        print('  ', names)
    elif 'fuse_ref_volt' in n:
        print('  ', struct.unpack('<%di' % (s // 4), raw))
    else:
        w = struct.unpack('<%dI' % (s // 4), raw[: s // 4 * 4])
        trip = [w[i:i + 3] for i in range(0, len(w) - len(w) % 3, 3)]
        print('  ', ' '.join('{%d,%d,%d}' % t for t in trip))
