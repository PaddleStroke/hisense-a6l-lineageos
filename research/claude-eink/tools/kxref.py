#!/usr/bin/env python3
"""Find code references (adrp+add / adrp+ldr) to a string or address in the
stock kernel ELF, using the pre-generated full.dis. Read-only analysis."""
import sys, re, bisect, struct
ELF = '/mnt/user-data/uploads/A6L/firmware/extracted/stock-symbolized.elf'
KS = '/mnt/user-data/uploads/A6L/firmware/extracted/stock.kallsyms'
syms = sorted((int(l.split()[0], 16), l.split()[2]) for l in open(KS) if len(l.split()) >= 3)
addrs = [s[0] for s in syms]
def symat(x):
    i = bisect.bisect_right(addrs, x) - 1
    return syms[i][1] + (f'+0x{x-syms[i][0]:x}' if x != syms[i][0] else '')
f = open(ELF, 'rb'); hdr = f.read(64)
e_phoff, = struct.unpack_from('<Q', hdr, 32); e_phentsize, e_phnum = struct.unpack_from('<HH', hdr, 54)
segs = []
for i in range(e_phnum):
    f.seek(e_phoff + i * e_phentsize)
    t, fl, off, va, pa, fsz, msz, al = struct.unpack('<IIQQQQQQ', f.read(56))
    if t == 1: segs.append((va, fsz, off))
targets = []
for arg in sys.argv[1:]:
    if arg.startswith('0x'):
        targets.append(int(arg, 16))
    else:
        pat = arg.encode() + b'\0'
        for va, sz, off in segs:
            f.seek(off); data = f.read(sz)
            i = data.find(pat)
            while i >= 0:
                if i == 0 or data[i-1] == 0:
                    targets.append(va + i)
                i = data.find(pat, i + 1)
print('targets:', [f'0x{t:x} {symat(t)}' for t in targets])
pages = {t & ~0xfff for t in targets}
lines = open('/home/claude/eink/full.dis').read().splitlines()
regs = {}
last_pc = 0
for ln in lines:
    m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)', ln)
    if not m: continue
    pc, mn, ops = int(m.group(1), 16), m.group(2), m.group(3)
    if pc - last_pc > 4: regs = {}
    last_pc = pc
    if mn == 'adrp':
        r, v = re.match(r'(\w+),\s*0x([0-9a-f]+)', ops).groups()
        v = int(v, 16)
        if v in pages: regs[r] = (v, pc)
        elif r in regs: del regs[r]
    elif regs:
        mm = re.match(r'(\w+),\s*(\w+),\s*#(0x[0-9a-f]+|\d+)', ops) if mn == 'add' else None
        ml = re.match(r'(\w+),\s*\[(\w+)(?:,\s*#(0x[0-9a-f]+|\d+))?\]', ops) if mn in ('ldr', 'str', 'ldrb', 'strb', 'ldrh', 'strh', 'ldrsw') else None
        if mm and mm.group(2) in regs:
            v = regs[mm.group(2)][0] + int(mm.group(3), 0)
            if v in targets: print(f'{pc:x} {symat(pc)}: {mn} {ops}  -> 0x{v:x} {symat(v)}')
        if ml and ml.group(2) in regs:
            v = regs[ml.group(2)][0] + int(ml.group(3) or '0', 0)
            if v in targets: print(f'{pc:x} {symat(pc)}: {mn} {ops}  -> 0x{v:x} {symat(v)}')
        if mn in ('bl',): pass
        if mn in ('add','ldr','mov') and not (mm and mm.group(2) in regs) and not ml:
            d = re.match(r'(\w+),', ops)
            if d and d.group(1) in regs and pc - regs[d.group(1)][1] > 0x100: del regs[d.group(1)]
