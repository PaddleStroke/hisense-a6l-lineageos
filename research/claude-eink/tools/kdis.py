#!/usr/bin/env python3
"""Offline disassembly helper for the reconstructed stock A6L kernel ELF.

Never executes stock code. Uses llvm-objdump for AArch64 decoding and
kallsyms for symbolization. Annotates:
  * bl/b targets with kallsyms names
  * adrp+add / adrp+ldr pairs with the resolved address, symbol and, when the
    target lies in a loaded segment and looks printable, the C string.
Addresses are ELF virtual addresses (link-time), not file offsets.
"""
import argparse, bisect, re, subprocess, sys
import struct

ap = argparse.ArgumentParser()
ap.add_argument('--elf', default='/mnt/user-data/uploads/A6L/firmware/extracted/stock-symbolized.elf')
ap.add_argument('--kallsyms', default='/mnt/user-data/uploads/A6L/firmware/extracted/stock.kallsyms')
ap.add_argument('--sym', action='append', default=[])
ap.add_argument('--addr', type=lambda x: int(x, 0))
ap.add_argument('--size', type=lambda x: int(x, 0))
a = ap.parse_args()

syms = []
for line in open(a.kallsyms):
    p = line.split()
    if len(p) >= 3:
        syms.append((int(p[0], 16), p[1], p[2]))
syms.sort()
addrs = [s[0] for s in syms]
byname = {}
for s in syms:
    byname.setdefault(s[2], s[0])

def symat(x):
    i = bisect.bisect_right(addrs, x) - 1
    if i < 0:
        return None
    base, t, n = syms[i]
    return n if base == x else f'{n}+0x{x-base:x}'

def bounds(name):
    base = byname[name]
    i = bisect.bisect_right(addrs, base)
    while i < len(addrs) and addrs[i] == base:
        i += 1
    return base, addrs[i] - base

f = open(a.elf, 'rb')
hdr = f.read(64)
assert hdr[:4] == b'\x7fELF' and hdr[4] == 2 and hdr[5] == 1
e_phoff, = struct.unpack_from('<Q', hdr, 32)
e_phentsize, e_phnum = struct.unpack_from('<HH', hdr, 54)
segs = []
for i in range(e_phnum):
    f.seek(e_phoff + i * e_phentsize)
    p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack('<IIQQQQQQ', f.read(56))
    if p_type == 1:
        segs.append((p_vaddr, p_filesz, p_offset))

def read(x, n):
    for va, sz, off in segs:
        if va <= x < va + sz:
            f.seek(off + x - va)
            return f.read(min(n, va + sz - x))
    return None

def cstr(x):
    d = read(x, 96)
    if not d:
        return None
    s = d.split(b'\0')[0]
    if len(s) >= 3 and all(32 <= c < 127 or c in (9, 10) for c in s):
        return s.decode()
    return None

ranges = []
for n in a.sym:
    for name in [s[2] for s in syms if re.fullmatch(n, s[2])]:
        ranges.append((name,) + bounds(name))
if a.addr is not None:
    ranges.append((f'addr_{a.addr:x}', a.addr, a.size or 256))

for name, base, size in ranges:
    print(f'\n==== {name} @ 0x{base:x} size 0x{size:x} ({size}) [next-symbol bound]')
    out = subprocess.run(['llvm-objdump', '-d', '--no-show-raw-insn', f'--start-address=0x{base:x}',
                          f'--stop-address=0x{base+size:x}', a.elf], capture_output=True, text=True).stdout
    regs = {}
    for line in out.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)', line)
        if not m:
            continue
        pc, mn, ops = int(m.group(1), 16), m.group(2), m.group(3)
        ann = ''
        if mn in ('bl', 'b') or mn.startswith('b.') or mn in ('cbz', 'cbnz', 'tbz', 'tbnz'):
            t = re.findall(r'0x([0-9a-f]+)', ops)
            if t:
                s = symat(int(t[-1], 16))
                if s:
                    ann = s
        if mn == 'adrp':
            r, v = re.match(r'(\w+),\s*0x([0-9a-f]+)', ops).groups()
            regs[r] = int(v, 16)
        elif mn == 'add':
            mm = re.match(r'(\w+),\s*(\w+),\s*#(0x[0-9a-f]+|\d+)', ops)
            if mm and mm.group(2) in regs:
                v = regs[mm.group(2)] + int(mm.group(3), 0)
                regs[mm.group(1)] = v
                s = cstr(v)
                ann = f'=0x{v:x} {symat(v) or ""}' + (f' "{s}"' if s else '')
        elif mn in ('ldr', 'ldrb', 'ldrh', 'str', 'strb', 'strh', 'ldrsw'):
            mm = re.match(r'(\w+),\s*\[(\w+)(?:,\s*#(0x[0-9a-f]+|\d+))?\]', ops)
            if mm and mm.group(2) in regs:
                v = regs[mm.group(2)] + int(mm.group(3) or '0', 0)
                ann = f'[0x{v:x}] {symat(v) or ""}'
                if mn == 'ldr' and mm.group(1).startswith('x'):
                    d = read(v, 8)
                    if d:
                        pv = int.from_bytes(d, 'little')
                        ann += f' -> 0x{pv:x} {symat(pv) or ""}'
                        s = cstr(pv)
                        if s:
                            ann += f' "{s}"'
        if mn in ('bl', 'ret') or mn.startswith('b'):
            pass
        print(f'{pc:x}: {mn:8} {ops:44} {ann}')
