#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""eink-round10 (user, 7 Oct 19:24:47: LCD page 2 -> e-ink: the launcher ended on page 1, no swipe made on purpose): a
finger on the rear panel while the phone is turned over must never reach Android.
  run 1 (default guard): a contact already down at mirror ON is not forwarded even when it moves (before: a new DOWN +
        MOVEs = a page swipe); a contact that begins before the first page is shown on the e-ink is not forwarded; a
        contact after the first page is; the guard end is logged.
  run 2 (--touch-guard-ms 0): the contact down at mirror ON is still ignored; a new contact right after mirror ON is
        forwarded (the guard is what holds it back in run 1).
usage: touch_guard_test.py MIRROR_BINARY FRAMES_PATTERN WORKDIR"""
import os, struct, subprocess, sys, time
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0x2f, 0x35, 0x36, 0x39
def ev(t, c, v): return struct.pack("llHHi", 0, 0, t, c, v)
mirror, frames, w = sys.argv[1], sys.argv[2], sys.argv[3]
fails = 0
def check(c, what):
    global fails
    print(("ok   " if c else "FAIL ") + what, flush=True); fails += 0 if c else 1
def wait_for(log, text, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if text in open(log).read(): return True
        time.sleep(0.02)
    return False
def tids(log, start):  # forwarded contacts (new tracking ids) after byte offset start
    return sum(1 for l in open(log).read()[start:].splitlines() if l.startswith("A6L_MIRROR TOUCH_OUT 3 57 ") and not l.endswith(" -1"))
def run(tag, extra):
    kf_p, tf_p, log = [os.path.join(w, "tg_%s.%s" % (tag, n)) for n in ("key", "touch", "log")]
    for f in (kf_p, tf_p):
        if os.path.exists(f): os.unlink(f)
        os.mkfifo(f)
    p = subprocess.Popen([mirror, "--dry", "--source", "files:" + frames, "--no-props", "--mode", "off", "--key-dev", kf_p,
                          "--touch-dev", tf_p, "--touch-debug", *extra], stdout=open(log, "w"), stderr=subprocess.STDOUT)
    kf = os.open(kf_p, os.O_WRONLY); tf = os.open(tf_p, os.O_WRONLY); time.sleep(0.5)
    def key(v): os.write(kf, ev(EV_KEY, 616, v) + ev(EV_SYN, 0, 0))
    def touch(slot, tid, x=None, y=None):
        b = ev(EV_ABS, ABS_MT_SLOT, slot) + (ev(EV_ABS, ABS_MT_TRACKING_ID, tid) if tid is not None else b"")
        if x is not None: b += ev(EV_ABS, ABS_MT_POSITION_X, x) + ev(EV_ABS, ABS_MT_POSITION_Y, y)
        os.write(tf, b + ev(EV_SYN, 0, 0))
    touch(0, 1, 500, 700); time.sleep(0.2)                     # finger on the rear panel, mirror off (phone being turned)
    key(1); time.sleep(0.15); key(0)                            # e-ink key -> mirror ON
    on = wait_for(log, "mirror ON", 3); off0 = len(open(log).read())
    for k in range(10): touch(0, None, 500 - 30 * k, 700); time.sleep(0.015)   # the held finger slides = a swipe before
    touch(0, -1); time.sleep(0.05)
    held = tids(log, off0)
    off1 = len(open(log).read())
    touch(1, 2, 360, 720); time.sleep(0.1); touch(1, None, 300, 720); time.sleep(0.05); touch(1, -1)   # new tap ~0.3 s after ON
    early = tids(log, off1)
    shown = wait_for(log, "done (simulated)", 4); time.sleep(0.2)
    off2 = len(open(log).read())
    touch(0, 3, 360, 720); time.sleep(0.1); touch(0, -1); time.sleep(0.3)   # after the first page
    late = tids(log, off2)
    p.terminate()
    try: p.wait(5)
    except subprocess.TimeoutExpired:	# eink-round11: seen once in ~10 host runs under load; init SIGKILLs after its timeout
        print("WARN mirror took > 5 s to exit after SIGTERM: killed", flush=True); p.kill(); p.wait(5)
    os.close(kf); os.close(tf)
    return on, shown, held, early, late, open(log).read()
on, shown, held, early, late, out = run("guard", [])
check(on and shown, "run 1: mirror ON and first page shown (simulated)")
check(held == 0, "run 1: contact already down at mirror ON, then sliding: not forwarded (%d)" % held)
check(early == 0, "run 1: contact that begins before the first page is shown: not forwarded (%d)" % early)
check(late == 1, "run 1: contact after the first page: forwarded (%d)" % late)
check("rear touch: guard ended (first page shown on the e-ink), 1 contact(s) held back" in out, "run 1: guard end logged")
check("contacts already down ignored until lifted" in out, "run 1: held contact logged")
on, shown, held, early, late, out = run("noguard", ["--touch-guard-ms", "0"])
check(held == 0, "run 2 (--touch-guard-ms 0): contact down at mirror ON still not forwarded (%d)" % held)
check(early == 1, "run 2 (--touch-guard-ms 0): new contact right after mirror ON forwarded (%d): the guard holds it in run 1" % early)
print("TOUCH_GUARD_TEST %s" % ("FAIL" if fails else "PASS")); sys.exit(1 if fails else 0)
