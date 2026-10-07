#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""eink-round11: e-ink warm-up during the LCD -> e-ink switch handshake (a6l_eink_mirror --dry, host properties from a
directory, A6L_PROP_DIR). Checks: an e-ink switch request (vendor.dualux.prepare "<seq> eink") while the mirror is off
sends one "warm"; the mirror turned on meanwhile captures during the warm-up and sends its first frame at the warm reply;
no warm-up for an LCD request, right after a wake-up (the LCD may be powering up), while asleep, or with
persist.sys.a6l.eink.prewarm=0; a warm-up is not an update (no "panel picture unknown", no min-gap).
usage: prewarm_test.py MIRROR_BINARY FRAMES_PATTERN WORKDIR"""
import os, re, subprocess, sys, time
mirror, frames, w = sys.argv[1], sys.argv[2], sys.argv[3]
P = os.path.join(w, "prewarm_props"); log = os.path.join(w, "prewarm.log")
subprocess.run(["rm", "-rf", P]); os.makedirs(P)
def prop(k, v):
    with open(os.path.join(P, k), "w") as f: f.write(v + "\n")
fails = 0
def check(c, what):
    global fails
    print(("ok   " if c else "FAIL ") + what, flush=True); fails += 0 if c else 1
def text(): return open(log).read()
def wait_for(pat, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if re.search(pat, text()): return True
        time.sleep(0.02)
    return False
def stamp(line_pat):  # "[12.345] A6L_MIRROR ..." -> 12.345 of the first match
    m = re.search(r"\[([0-9.]+)\] A6L_MIRROR " + line_pat, text())
    return float(m.group(1)) if m else None
prop("persist.vendor.eink.mode", "off"); prop("vendor.dualux.awake", "1"); prop("sys.a6l.dualux.theme_sync", "1")
env = dict(os.environ, A6L_PROP_DIR=P)
p = subprocess.Popen([mirror, "--dry", "--source", "files:" + frames, "--key-dev", "none", "--touch-dev", "none"],
                     stdout=open(log, "w"), stderr=subprocess.STDOUT, env=env)
time.sleep(1.5)
prop("vendor.dualux.prepare", "100 eink")
check(wait_for(r"CMD warm", 0.5), "e-ink switch request with the mirror off: one 'warm' sent")
time.sleep(0.1)
prop("sys.a6l.dualux.ready", "100 eink"); prop("persist.vendor.eink.mode", "mirror")   # handshake done: dualux turns the mirror on
check(wait_for(r"CMD frame", 2.0), "mirror ON during the warm-up: first frame sent")
t_on, t_warm_done, t_frame = stamp(r"mirror ON"), stamp(r"done \(simulated\): warm"), stamp(r"appearance 100 eink: frame queued")
t_cap = stamp(r"frame 1: .*\(staged during the drive\)")
check(None not in (t_on, t_warm_done, t_frame, t_cap) and t_cap <= t_warm_done <= t_frame and t_frame - t_warm_done < 1.0,
      "first frame captured during the warm-up and queued at the warm reply, no new capture after it: capture %.3f, warm done %.3f, frame %.3f"
      % (t_cap or -1, t_warm_done or -1, t_frame or -1))
check("panel picture unknown" not in text(), "the warm-up is not a picture: no 'panel picture unknown'")
prop("persist.vendor.eink.mode", "off"); prop("vendor.dualux.prepare", ""); time.sleep(0.6)
prop("vendor.dualux.prepare", "101 lcd"); time.sleep(0.4)
prop("vendor.dualux.awake", "0"); time.sleep(0.3); prop("vendor.dualux.prepare", "102 eink"); time.sleep(0.3)
check("102 eink: no e-ink warm-up (Android asleep" in text(), "request while Android sleeps: no warm-up")
prop("vendor.dualux.awake", "1"); time.sleep(0.2); prop("vendor.dualux.prepare", "103 eink"); time.sleep(0.3)
check("103 eink: no e-ink warm-up (Android just woke" in text(), "request right after a wake-up: no warm-up (the LCD may be powering up)")
time.sleep(1.2); prop("persist.sys.a6l.eink.prewarm", "0"); prop("vendor.dualux.prepare", "104 eink"); time.sleep(0.4)
check(len(re.findall(r"CMD warm", text())) == 1, "LCD request, asleep, just woke, prewarm=0: no other 'warm' (%d in total)" % len(re.findall(r"CMD warm", text())))
p.terminate(); p.wait(5)
print("PREWARM_TEST %s" % ("FAIL" if fails else "PASS")); sys.exit(1 if fails else 0)
