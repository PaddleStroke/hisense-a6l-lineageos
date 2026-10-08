#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""eink-round9: a6l_eink_mirror idle capture back-off end to end (--dry, a static file source). Counts the captures in
12 s with the back-off (default 1000 ms max, knee 10) and without it (--idle-max-ms 0), and the CPU time the mirror
used in each run (host numbers: a file capture is far cheaper than a phone drm capture, see README for the phone).
usage: idle_backoff_test.py MIRROR_BINARY FRAME_RAW WORKDIR"""
import os, re, resource, subprocess, sys, time, shutil
mirror, frame, w = sys.argv[1], sys.argv[2], sys.argv[3]
d = os.path.join(w, "idle_frames"); shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
shutil.copy(frame, os.path.join(d, "f0.raw"))
fails = 0
def check(c, what):
    global fails
    print(("ok   " if c else "FAIL ") + what, flush=True); fails += 0 if c else 1
def run(tag, extra, secs=12.0):
    log = os.path.join(w, "idle_%s.log" % tag)
    r0 = resource.getrusage(resource.RUSAGE_CHILDREN)
    p = subprocess.Popen([mirror, "--dry", "--source", "files:" + d + "/f%d.raw", "--no-props", "--mode", "mirror", "--key-dev", "none",
                          "--touch-dev", "none", "--interval", "100", *extra], stdout=open(log, "w"), stderr=subprocess.STDOUT)
    time.sleep(secs); p.terminate(); p.wait(5)
    r1 = resource.getrusage(resource.RUSAGE_CHILDREN)
    txt = open(log).read(); m = re.search(r"exit after (\d+) frames", txt)
    return (int(m.group(1)) if m else -1), (r1.ru_utime - r0.ru_utime) + (r1.ru_stime - r0.ru_stime), txt
n_bo, cpu_bo, log_bo = run("backoff", [])
n_off, cpu_off, _ = run("off", ["--idle-max-ms", "0"])
print("captures in 12 s: back-off %d, off %d; mirror CPU: back-off %.2f s, off %.2f s" % (n_bo, n_off, cpu_bo, cpu_off))
check(n_off >= 90, "without back-off: a capture every 100 ms (%d in 12 s)" % n_off)
check(0 < n_bo <= 40, "with back-off: 10 x 100 ms + 10 x 250 ms + 10 x 500 ms, then 1 per second (%d in 12 s)" % n_bo)
check("idle: capture interval 250 ms" in log_bo and "idle: capture interval 1000 ms" in log_bo, "back-off steps logged (250 ... 1000 ms)")
check(cpu_bo < cpu_off, "less mirror CPU with the back-off (%.2f s vs %.2f s)" % (cpu_bo, cpu_off))
print("IDLE_BACKOFF_TEST %s" % ("FAIL" if fails else "PASS")); sys.exit(1 if fails else 0)
