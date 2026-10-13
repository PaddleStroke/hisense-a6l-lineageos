#!/usr/bin/env python3
"""Patch the kernel cmdline of an Android boot image (header v0/v1/v2) in place into a new file.
usage: patch-bootimg-cmdline.py in.img out.img 'old' 'new' [...pairs]   (30 Sep 2026, attended r6b debug: fbcon=nodefer)"""
import sys, hashlib, struct
src, dst, pairs = sys.argv[1], sys.argv[2], sys.argv[3:]
b = bytearray(open(src, 'rb').read())
assert b[:8] == b'ANDROID!', 'not a boot image'
ver = struct.unpack_from('<I', b, 40)[0]
assert ver <= 2, f'header v{ver} unsupported'
main = b[64:64 + 512].split(b'\0', 1)[0].decode()
extra = b[608:608 + 1024].split(b'\0', 1)[0].decode()
full = main + extra
new = full
for i in range(0, len(pairs), 2):
    assert pairs[i] in new, f'{pairs[i]!r} not in cmdline'
    new = new.replace(pairs[i], pairs[i + 1], 1)
nb = new.encode()
assert len(nb) < 512 + 1024, 'cmdline too long'
b[64:64 + 512] = nb[:511].ljust(512, b'\0') if len(nb) <= 511 else nb[:512]
b[608:608 + 1024] = (nb[512:] if len(nb) > 511 else b'').ljust(1024, b'\0')
if len(nb) <= 511:
    b[64:64 + 512] = nb.ljust(512, b'\0')
open(dst, 'wb').write(b)
print('old:', full); print('new:', new); print('sha256', hashlib.sha256(b).hexdigest())
