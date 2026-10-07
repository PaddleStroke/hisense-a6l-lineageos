#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""eink-round11: persist.sys.a6l.eink.invert = 1 renders the captured page inverted (a6l_eink_mirror --dry --out, same
source frame with and without the property). Inside the picture every output grey must be the inverted-tone value of the
normal one's input: dark input -> white paper, light input -> black; letterbox bars stay paper white.
usage: invert_test.py MIRROR_BINARY FRAMES_PATTERN WORKDIR"""
import os, subprocess, sys
mirror, frames, w = sys.argv[1], sys.argv[2], sys.argv[3]
def run(inv):
    out = os.path.join(w, "invert_out_%d" % inv); subprocess.run(["rm", "-rf", out]); os.makedirs(out)
    env = dict(os.environ); env["persist.sys.a6l.eink.invert"] = str(inv)
    subprocess.run([mirror, "--dry", "--source", "files:" + frames, "--mode", "mirror", "--key-dev", "none", "--touch-dev", "none",
                    "--frames", "3", "--out", out], stdout=open(out + ".log", "w"), stderr=subprocess.STDOUT, env=env, timeout=30)
    f = open(os.path.join(out, "f0.pgm"), "rb").read()
    hdr_end = 0
    for _ in range(3): hdr_end = f.index(b"\n", hdr_end) + 1
    return f[hdr_end:], open(out + ".log").read()
a, la = run(0); b, lb = run(1)
fails = 0
def check(c, what):
    global fails
    print(("ok   " if c else "FAIL ") + what); fails += 0 if c else 1
check(len(a) == len(b) == 720 * 1440, "both runs wrote a 720x1440 first frame")
check("inverted rendering on" in lb and "inverted rendering on" not in la, "the property is applied and logged")
pairs = [(x, y) for x, y in zip(a, b) if x not in (0, 255) or y not in (0, 255)]
dark_in = sum(1 for x, y in zip(a, b) if x == 0 and y == 255); light_in = sum(1 for x, y in zip(a, b) if x == 255 and y == 0)
both_white = sum(1 for x, y in zip(a, b) if x == 255 and y == 255)
check(light_in > 1000, "white paper becomes black (%d px), dark input becomes white (%d px)" % (light_in, dark_in))
groups = {}
for x, y in zip(a, b):
    if 0 < x < 255: groups.setdefault(x, []).append(y)
med = [(x, sorted(v)[len(v) // 2]) for x, v in sorted(groups.items()) if len(v) >= 20]
viol = sum(1 for (x1, m1), (x2, m2) in zip(med, med[1:]) if m2 > m1 + 2)
check(len(med) >= 10 and viol == 0, "inverted tone: output grey falls as the normal output rises (%d grey levels, %d violations)" % (len(med), viol))
print("INVERT_TEST %s" % ("FAIL" if fails else "PASS")); sys.exit(1 if fails else 0)
