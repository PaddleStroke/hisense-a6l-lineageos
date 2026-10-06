#!/usr/bin/env python3
# volte2 (25 Sep 2026): dump QCCI IDL service objects (message ids + TLV layouts) from a stock
# Android vendor lib (aarch64, APS2 packed RELA applied). Usage: idldump.py libqmiservices.so imsdcm [imsa ...]
import struct, sys, subprocess
f = sys.argv[1]
d = open(f, 'rb').read()
phoff = struct.unpack_from('<Q', d, 0x20)[0]; phn = struct.unpack_from('<H', d, 0x38)[0]
S = []
dyn = None
for i in range(phn):
    t, fl, off, va, pa, fs, ms, al = struct.unpack_from('<IIQQQQQQ', d, phoff + i * 56)
    if t == 1: S.append((va, off, fs, ms))
    if t == 2: dyn = (off, fs)
def v2o(v):
    for va, off, fs, ms in S:
        if va <= v < va + fs: return off + v - va
    return None
mem = {}  # patched 8-byte words: vaddr -> value
def sleb(buf, p):
    r = 0; s = 0
    while True:
        b = buf[p]; p += 1
        r |= (b & 0x7f) << s; s += 7
        if not b & 0x80:
            if b & 0x40: r -= 1 << s
            return r, p
tags = {}
o, n = dyn
for i in range(0, n, 16):
    tg, val = struct.unpack_from('<qQ', d, o + i)
    if tg == 0: break
    tags[tg] = val
if 0x60000011 in tags:  # DT_ANDROID_RELA (APS2)
    p = v2o(tags[0x60000011]); end = p + tags[0x60000012]
    assert d[p:p+4] == b'APS2'; p += 4
    cnt, p = sleb(d, p); off, p = sleb(d, p)
    info = 0; add = 0; done = 0
    while done < cnt:
        gsz, p = sleb(d, p); gfl, p = sleb(d, p)
        if gfl & 2: god, p = sleb(d, p)
        if gfl & 1: info, p = sleb(d, p)
        if (gfl & 8) and (gfl & 4): ad, p = sleb(d, p); add += ad
        elif not gfl & 8: add = 0
        for _ in range(gsz):
            if gfl & 2: off += god
            else: od, p = sleb(d, p); off += od
            if not gfl & 1: info, p = sleb(d, p)
            if (gfl & 8) and not (gfl & 4): ad, p = sleb(d, p); add += ad
            if (info & 0xffffffff) == 1027: mem[off] = add  # R_AARCH64_RELATIVE
            done += 1
def q(v):
    if v in mem: return mem[v]
    o = v2o(v); return struct.unpack_from('<Q', d, o)[0]
def u32(v): return struct.unpack_from('<I', d, v2o(v))[0]
def u16(v): return struct.unpack_from('<H', d, v2o(v))[0]
def byte(v): return d[v2o(v)]
syms = {}
for l in subprocess.run(['nm', '-D', '--defined-only', f], capture_output=True, text=True).stdout.splitlines():
    s = l.split()
    if len(s) == 3: syms[s[2]] = int(s[0], 16)
TN = ['u8', 'u16', 'u32', 'u64', 'enum8', 'enum16', 'string', 'struct']
def fields(p, stop_tlv):
    """decode one element (field) at p; return (desc, p)"""
    fl = byte(p); p += 1
    ext = 0
    if fl & 0x08: ext = byte(p); p += 1
    if ext & 0x80: p += 1
    typ = TN[fl & 7]
    if fl & 0x80: off = u16(p); p += 2
    else: off = byte(p); p += 1
    if ext & 0x02: p += 1
    desc = typ
    if fl & 0x40:  # array
        if ext & 0x40: sz = u32(p) & 0xffffff; p += 3
        elif fl & 0x20: sz = u16(p); p += 2
        else: sz = byte(p); p += 1
        if fl & 0x10 and typ != 'string':
            lo = byte(p); p += 1  # offset of the length field
            if fl & 0x80: p += 1
            desc = '%s[var<=%d%s]' % (typ, sz, ',len16' if fl & 0x20 else ',len8')
        elif typ == 'string': desc = 'string(max %d, u8len-prefix? var)' % sz if fl & 0x10 else 'string[%d]' % sz
        else: desc = '%s[%d]' % (typ, sz)
    if typ == 'struct':
        a, b = byte(p), byte(p + 1); p += 2
        desc += '<T%d.%d>' % (b & 0xf, a | ((b & 0xf0) << 4))
    return desc + '@%d' % off, p
def msg(tab, idx):
    ents = q(tab + 16)
    sz = u32(ents + idx * 16); p = q(ents + idx * 16 + 8)
    out = []
    if p == 0: return sz, out
    while True:
        tf = byte(p); p += 1
        if tf & 0x40: tlv = byte(p); p += 1; opt = 'opt'
        else: tlv = tf & 0x0f; opt = 'req'
        desc, p = fields(p, True)
        out.append('0x%02x %s %s' % (tlv, opt, desc))
        if tf & 0x80: break
    return sz, out
def typedef(tab, idx):
    ents = q(tab + 8); sz = u32(ents + idx * 16); p = q(ents + idx * 16 + 8); out = []
    for _ in range(40):
        if byte(p) == 0x20: break
        desc, p = fields(p, False); out.append(desc)
    return sz, out
for name in sys.argv[2:]:
    so = syms[name + '_qmi_idl_service_object_v01']
    lib, idl, sid, mx = struct.unpack_from('<IIII', d, v2o(so))
    nm = [u16(so + 16 + 2 * i) for i in range(3)]
    tabs = [q(so + 24 + 8 * i) for i in range(3)]
    tt = q(so + 48); minor = u32(so + 56)
    print('== %s service_id=%d (0x%x) idl_major=%d minor=%d lib=%d max_msg=%d n_req/resp/ind=%s' % (name, sid, sid, idl, minor, lib, mx, nm))
    ntypes, nmsgs = u16(tt), u16(tt + 2)
    for k, kind in enumerate(['REQ', 'RESP', 'IND']):
        for i in range(nm[k]):
            e = tabs[k] + 6 * i
            mid, mt, ml = u16(e), u16(e + 2), u16(e + 4)
            tbl, ix = mt >> 12, mt & 0xfff
            if tbl != 0: print('  %s 0x%04x -> table %d msg %d' % (kind, mid, tbl, ix)); continue
            sz, tl = msg(tt, ix)
            print('  %s 0x%04x (msg#%d cstruct=%d maxlen=%d): %s' % (kind, mid, ix, sz, ml, ' | '.join(tl)))
    for i in range(ntypes):
        sz, fl = typedef(tt, i)
        print('  T0.%d (cstruct=%d): %s' % (i, sz, ', '.join(fl)))
