#!/usr/bin/env python3
"""libcam_rgb_to_png.py <in.bin> <width> <height> <FORMAT> <out.png> [stride] [step]
Convert a `cam --file` frame (SoftISP output) to PNG. FORMAT = libcamera name as printed by cam
(e.g. 2880x2156-ABGR8888): RGB888, BGR888, ABGR8888, ARGB8888, XBGR8888, XRGB8888.
stride defaults to width*bpp; step (default 2) subsamples for speed. Pure Python (laptop as-is)."""
import sys, struct, zlib
fn, W, H, F, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4].upper(), sys.argv[5]
# memory byte order (DRM fourcc naming is little-endian: RGB888 = B,G,R in memory)
order = {'RGB888': (3, 2, 1, 0), 'BGR888': (3, 0, 1, 2), 'ABGR8888': (4, 0, 1, 2), 'XBGR8888': (4, 0, 1, 2),
         'ARGB8888': (4, 2, 1, 0), 'XRGB8888': (4, 2, 1, 0)}
if F not in order: sys.exit('unsupported format %s' % F)
bpp, ri, gi, bi = order[F]
stride = int(sys.argv[6]) if len(sys.argv) > 6 and int(sys.argv[6]) > 0 else W * bpp
step = int(sys.argv[7]) if len(sys.argv) > 7 else 2
d = open(fn, 'rb').read()
if len(d) < stride * (H - 1) + W * bpp:
    # guess the stride from the file size
    stride = len(d) // H
    print('stride from file size:', stride)
w2, h2 = W // step, H // step
raw = bytearray()
s = [0, 0, 0]
for y in range(h2):
    raw.append(0); o = y * step * stride
    for x in range(w2):
        p = o + x * step * bpp
        r, g, b = d[p + ri], d[p + gi], d[p + bi]
        raw += bytes((r, g, b)); s[0] += r; s[1] += g; s[2] += b
n = w2 * h2
print('mean R/G/B', round(s[0] / n, 1), round(s[1] / n, 1), round(s[2] / n, 1), 'size', w2, 'x', h2)
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w2, h2, 8, 2, 0, 0, 0))
                      + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))
print('wrote', out)
