#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Synthetic raw screencap frames (u32 w,h,format=1 RGBA,dataspace + pixels) for the mirror/epdd host test.
Scenario (frame index -> content): 0 static page A, 8 page B (a tap), 16..30 a list scrolling 60 px per frame,
31.. static (missing indices repeat the last existing file, as a6l_eink_mirror --source files: does)."""
import os, struct, sys
import numpy as np
W, H = 1080, 2340
out = sys.argv[1] if len(sys.argv) > 1 else "frames"
os.makedirs(out, exist_ok=True)
def page(seed, shift=0):
    img = np.full((H, W), 245, np.uint8)
    rng = np.random.default_rng(seed)
    for row in range(0, 4000, 120):          # "text lines" of a long list
        y = row - shift
        if 0 <= y < H - 40:
            n = rng.integers(200, 900)
            img[y:y + 36, 60:60 + n] = 30
    img[0:90, :] = 60                        # status bar
    return img
def save(i, g):
    rgba = np.dstack([g, g, g, np.full_like(g, 255)])
    with open(os.path.join(out, "f%d.raw" % i), "wb") as f:
        f.write(struct.pack("<4I", W, H, 1, 0)); f.write(rgba.tobytes())
save(0, page(1)); save(8, page(2))
for k, i in enumerate(range(16, 31)): save(i, page(3, shift=60 * (k + 1)))
print("frames in", out)
