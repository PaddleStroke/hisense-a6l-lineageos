#!/usr/bin/env python3
"""Extract FDT blobs appended after a gzip kernel; no firmware mutation."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

p = argparse.ArgumentParser()
p.add_argument('kernel', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
b = a.kernel.read_bytes()
z = zlib.decompressobj(16 + zlib.MAX_WBITS)
z.decompress(b)
if not z.eof:
    raise SystemExit('Incomplete gzip kernel')
offset = len(b) - len(z.unused_data)
a.output.mkdir(parents=True, exist_ok=True)
entries = []
while offset < len(b):
    if b[offset] == 0:
        offset += 1
        continue
    if offset + 40 > len(b):
        raise SystemExit(f'Truncated FDT header at {offset}')
    h = struct.unpack_from('>10I', b, offset)
    magic, size, off_struct, off_strings, off_reserve, version, compatible, cpu, sz_strings, sz_struct = h
    if (magic != 0xd00dfeed or size < 40 or offset + size > len(b)
            or off_struct + sz_struct > size or off_strings + sz_strings > size
            or off_reserve >= size):
        raise SystemExit(f'Invalid appended FDT at {offset}')
    blob = b[offset:offset+size]
    name = f'stock-{len(entries):02d}.dtb'
    dest = a.output / name
    if dest.exists() and dest.read_bytes() != blob:
        raise SystemExit(f'Refusing to replace different output {dest}')
    dest.write_bytes(blob)
    entries.append(dict(file=name, kernel_offset=offset, size=size,
                        sha256=hashlib.sha256(blob).hexdigest()))
    offset += size
(a.output / 'manifest.json').write_text(json.dumps(entries, indent=2)+'\n')
print(json.dumps(entries, indent=2))
