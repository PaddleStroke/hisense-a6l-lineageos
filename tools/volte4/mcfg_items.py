#!/usr/bin/env python3
# volte4 (27 Sep 2026): exact MCFG_SW/HW item walker (ELF MBN -> MCFG segment -> items).
# Format (derived from the stock MBNs; field meanings cross-checked with Biktorgj/mcfg_tools, EfsTools ItemType):
#   MCFG header: 'MCFG' u16 format u16 type(0 HW,1 SW) u32 n_items u16 carrier u16 pad, then {u16 0x1383?, u16 len=4, u32 ver}
#   item: u32 total_len | u8 type | u8 attrib | u16 pad | body
#     type 1 (NV)    : u16 nv_id  u16 size  payload
#     type 2/4 (EFS) : {u16 sect(1=path,2=data) u16 len bytes}*   (data section = u8 prefix + file content, see below)
#     type 10        : trailer (MCFG_TRL)
# When attrib bit 0x10 is set (0x19/0x39/0x50/...), the first byte of the EFS data section / NV payload is a prefix,
# not file content: 0x07 on almost every item (read as a subscription mask: subs 0|1|2), 0x02/0x06 on a few NV items.
# IMS_enable data=07 01 -> the EFS file is the single byte 01. Attrib 0x09/0x29 items carry no prefix.
# usage: mcfg_items.py FILE.MBN [--json] [--grep REGEX]
import struct, sys, json, re

def parse(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF':
        raise SystemExit('not an ELF MBN: %s' % path)
    phoff = struct.unpack_from('<I', d, 0x1c)[0]; phn = struct.unpack_from('<H', d, 0x2c)[0]
    seg = None
    for i in range(phn):
        t, off, va, pa, fs, ms, fl, al = struct.unpack_from('<8I', d, phoff + i * 32)
        if d[off:off + 4] == b'MCFG':
            seg = (off, fs)
    if not seg:
        raise SystemExit('no MCFG segment')
    off, fs = seg
    magic, fmt, ctype, n, car, pad = struct.unpack_from('<4sHHIHH', d, off)
    p = off + 16
    sub_magic, sub_len = struct.unpack_from('<HH', d, p)
    sub_ver = struct.unpack_from('<I', d, p + 4)[0] if sub_len == 4 else None
    p += 4 + sub_len
    items = []; trailer = {}
    end = off + fs
    for k in range(n):
        if p + 8 > end: break
        ln, typ, attrib, _ = struct.unpack_from('<IBBH', d, p)
        if ln < 8 or p + ln > end:
            raise SystemExit('bad item %d at %#x len %d' % (k, p, ln))
        body = d[p + 8:p + ln]
        it = {'idx': k, 'off': p, 'type': typ, 'attrib': attrib}
        if typ == 1:
            nvid, sz = struct.unpack_from('<HH', body, 0)
            pl = body[4:4 + sz]
            k = 1 if attrib & 0x10 else 0
            it.update(kind='nv', nv=nvid, prefix=pl[:k].hex(), data=pl[k:].hex())
        elif typ in (2, 4):
            q = 0; path_ = None; data = b''; has_data = False
            while q + 4 <= len(body):
                sect, sl = struct.unpack_from('<HH', body, q)
                v = body[q + 4:q + 4 + sl]; q += 4 + sl
                if sect == 1: path_ = v.rstrip(b'\0').decode('latin1')
                elif sect == 2: data = v; has_data = True
            k = 1 if (attrib & 0x10) and data else 0
            it.update(kind='efs' if typ == 2 else 'file', path=path_,
                      prefix=data[:k].hex(), data=data[k:].hex(), has_data=has_data)
        elif typ == 10:
            it.update(kind='trailer'); tr = body
            # trailer: u16 0xa1 u16 len 'MCFG_TRL' then {u8 id u16 len data}
            i2 = tr.find(b'MCFG_TRL')
            q = i2 + 8
            while i2 >= 0 and q + 3 <= len(tr):
                sid, sl = struct.unpack_from('<BH', tr, q); v = tr[q + 3:q + 3 + sl]; q += 3 + sl
                trailer[sid] = v
        else:
            it.update(kind='type%d' % typ, raw=body.hex())
        items.append(it)
        p += ln
    info = {'file': path, 'format': fmt, 'config_type': ctype, 'n_items': n, 'carrier': car, 'sub_version': sub_ver,
            'parsed': len(items)}
    if 3 in trailer: info['name'] = trailer[3].decode('latin1')
    if 1 in trailer and len(trailer[1]) == 4: info['version'] = '0x%08x' % struct.unpack('<I', trailer[1])[0]
    if 2 in trailer: info['mcc_mnc'] = trailer[2].hex()
    return info, items

def text(v):
    b = bytes.fromhex(v)
    if b and all(32 <= c < 127 or c == 0 for c in b) and b.rstrip(b'\0') and b[0] != 0:
        return repr(b.rstrip(b'\0').decode())
    return v if len(v) <= 64 else v[:64] + '...(%d B)' % (len(v) // 2)

if __name__ == '__main__':
    info, items = parse(sys.argv[1])
    rx = None
    if '--grep' in sys.argv: rx = re.compile(sys.argv[sys.argv.index('--grep') + 1])
    if '--json' in sys.argv:
        print(json.dumps({'info': info, 'items': items}, indent=1)); sys.exit(0)
    print('# ' + ' '.join('%s=%s' % kv for kv in info.items()))
    for it in items:
        if it['kind'] in ('efs', 'file'):
            line = '%3d %-4s a=%02x %s = %s' % (it['idx'], it['kind'], it['attrib'], it['path'], text(it['data']) if it['has_data'] else '(no data)')
        elif it['kind'] == 'nv':
            line = '%3d nv   a=%02x NV#%d = %s' % (it['idx'], it['attrib'], it['nv'], text(it['data']))
        else:
            line = '%3d %s' % (it['idx'], it['kind'])
        if rx and not rx.search(line): continue
        print(line)
