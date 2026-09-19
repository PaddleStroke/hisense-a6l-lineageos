#!/usr/bin/env python3
"""Dump 8-byte words from the reconstructed stock kernel ELF with kallsyms
annotation. Addresses are link-time virtual addresses. Read-only."""
import sys, bisect, struct
ELF = '/mnt/user-data/uploads/A6L/firmware/extracted/stock-symbolized.elf'
KS = '/mnt/user-data/uploads/A6L/firmware/extracted/stock.kallsyms'
syms = sorted((int(l.split()[0], 16), l.split()[2]) for l in open(KS) if len(l.split()) >= 3)
addrs = [s[0] for s in syms]
def symat(x):
    i = bisect.bisect_right(addrs, x) - 1
    if i < 0 or x - syms[i][0] > 0x10000:
        return ''
    return syms[i][1] + (f'+0x{x-syms[i][0]:x}' if x != syms[i][0] else '')
f = open(ELF, 'rb'); hdr = f.read(64)
e_phoff, = struct.unpack_from('<Q', hdr, 32); e_phentsize, e_phnum = struct.unpack_from('<HH', hdr, 54)
segs = []
for i in range(e_phnum):
    f.seek(e_phoff + i * e_phentsize)
    t, fl, off, va, pa, fsz, msz, al = struct.unpack('<IIQQQQQQ', f.read(56))
    if t == 1: segs.append((va, fsz, off))
def read(x, n):
    for va, sz, off in segs:
        if va <= x < va + sz:
            f.seek(off + x - va); return f.read(min(n, va + sz - x))
    return b''
def cstr(x):
    d = read(x, 120).split(b'\0')[0]
    return d.decode() if len(d) >= 2 and all(32 <= c < 127 for c in d) else None
addr = int(sys.argv[1], 0); n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x100
mode = sys.argv[3] if len(sys.argv) > 3 else 'q'
if mode == 'q':
    for off in range(0, n, 8):
        d = read(addr + off, 8)
        if len(d) < 8: break
        v = int.from_bytes(d, 'little')
        s = cstr(v) if v else None
        print(f'{addr+off:x} +0x{off:03x}: {v:016x}  {symat(v) if v else ""}' + (f'  "{s}"' if s else ''))
elif mode == 's':
    print(cstr(addr))
elif mode == 'x':
    d = read(addr, n)
    for i in range(0, len(d), 16):
        print(f'{addr+i:x}: ' + ' '.join(f'{b:02x}' for b in d[i:i+16]) + '  ' + ''.join(chr(b) if 32 <= b < 127 else '.' for b in d[i:i+16]))
