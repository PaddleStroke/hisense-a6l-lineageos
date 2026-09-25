#!/usr/bin/env python3
"""raw10_to_png.py <in.raw> <width> <height> <bpl> <bayer RGGB|GRBG|GBRG|BGGR> <out.png> [gain]
MIPI-packed RAW10 -> half-resolution RGB PNG (each 2x2 Bayer quad -> 1 pixel), gray-world AWB,
gamma 2.2. Pure Python (no numpy) so it runs on the laptop as-is. A6L camera bring-up helper."""
import sys, struct, zlib
fn, W, H, bpl, pat, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5].upper(), sys.argv[6]
ugain = float(sys.argv[7]) if len(sys.argv) > 7 else 1.0
data = open(fn, 'rb').read()
def row(y):
    b = data[y*bpl:y*bpl + W*5//4]
    r = []
    for i in range(0, len(b) - 4, 5):
        lo = b[i+4]
        r += [(b[i] << 2) | (lo & 3), (b[i+1] << 2) | ((lo >> 2) & 3), (b[i+2] << 2) | ((lo >> 4) & 3), (b[i+3] << 2) | ((lo >> 6) & 3)]
    return r
pos = {c: (i // 2, i % 2) for i, c in enumerate(pat)}  # colour -> (dy,dx); G appears twice, take first
gpos = [(i // 2, i % 2) for i, c in enumerate(pat) if c == 'G']
w2, h2 = W // 2, H // 2
R = []; G = []; B = []
for y in range(h2):
    r0, r1 = row(2*y), row(2*y+1)
    rows = (r0, r1)
    for x in range(w2):
        q = lambda p: rows[p[0]][2*x + p[1]] if 2*x + p[1] < len(rows[p[0]]) else 0
        R.append(q(pos['R'])); B.append(q(pos['B'])); G.append((q(gpos[0]) + q(gpos[1])) / 2)
blk = 64  # sensor black level (10-bit), typical
mR = sum(R)/len(R) - blk; mG = sum(G)/len(G) - blk; mB = sum(B)/len(B) - blk
kr = mG/mR if mR > 0 else 1; kb = mG/mB if mB > 0 else 1
print('mean R/G/B', round(mR+blk,1), round(mG+blk,1), round(mB+blk,1), 'awb gains', round(kr,3), round(kb,3))
def px(v, k):
    v = max(0.0, (v - blk) * k * ugain / (1023 - blk))
    return min(255, int(255 * (min(v, 1.0) ** (1/2.2))))
raw = bytearray()
for y in range(h2):
    raw.append(0)
    for x in range(w2):
        i = y*w2 + x
        raw += bytes((px(R[i], kr), px(G[i], 1), px(B[i], kb)))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w2, h2, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b'')
open(out, 'wb').write(png)
print('wrote', out, w2, 'x', h2)
