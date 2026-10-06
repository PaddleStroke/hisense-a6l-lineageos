#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""r5 bug hunt eink-display (E2): the rear touchscreen disappears (read error / EOF, e.g. driver re-probe) while a
forwarded contact is down. The mirror must lift that contact on its uinput touchscreen (else Android keeps a finger
pressed for ever) and must pick the device up again when it comes back (it was never reopened).
usage: touch_loss_test.py MIRROR_BINARY FRAMES_PATTERN WORKDIR"""
import os, struct, subprocess, sys, time
EV_SYN, EV_ABS = 0, 3
ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0x2f, 0x35, 0x36, 0x39
def ev(t, c, v): return struct.pack("llHHi", 0, 0, t, c, v)
def touch(fd, slot, tid, x=None, y=None):
    b = ev(EV_ABS, ABS_MT_SLOT, slot) + ev(EV_ABS, ABS_MT_TRACKING_ID, tid)
    if x is not None: b += ev(EV_ABS, ABS_MT_POSITION_X, x) + ev(EV_ABS, ABS_MT_POSITION_Y, y)
    os.write(fd, b + ev(EV_SYN, 0, 0))
mirror, frames, w = sys.argv[1], sys.argv[2], sys.argv[3]
fifo = os.path.join(w, "touch_loss.fifo"); log = os.path.join(w, "touch_loss.log")
if os.path.exists(fifo): os.unlink(fifo)
os.mkfifo(fifo)
p = subprocess.Popen([mirror, "--dry", "--source", "files:" + frames, "--no-props", "--mode", "mirror", "--key-dev", "none",
                      "--touch-dev", fifo, "--touch-debug"], stdout=open(log, "w"), stderr=subprocess.STDOUT)
time.sleep(0.5)
fd = os.open(fifo, os.O_WRONLY)
time.sleep(0.5)
touch(fd, 0, 5, 360, 720); time.sleep(0.3)
mark1 = open(log).read().count("TOUCH_OUT 3 57 -1")
os.close(fd)					# device gone with the finger down
time.sleep(0.5)
out = open(log).read()
lifted = out.count("TOUCH_OUT 3 57 -1") > mark1
time.sleep(5.5)					# device comes back
def open_writer(timeout):			# ENXIO while nobody has the FIFO open for reading (device not reopened)
    end = time.time() + timeout
    while time.time() < end:
        try: return os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
        except OSError: time.sleep(0.1)
    return -1
fd = open_writer(3); time.sleep(0.3)
n_before = open(log).read().count("TOUCH_IN")
if fd >= 0:
    touch(fd, 0, 9, 360, 720); time.sleep(0.3); touch(fd, 0, -1); time.sleep(0.3)
    os.close(fd)
p.terminate(); p.wait()
out = open(log).read()
reopened = out.count("TOUCH_IN") > n_before
fails = 0
for ok, what in [(lifted, "contact lifted on uinput when the rear touch device is lost"),
                 (reopened, "rear touch device reopened after it came back")]:
    print(("ok   " if ok else "FAIL ") + what); fails += not ok
print("TOUCH_LOSS_TEST %s" % ("FAIL" if fails else "PASS"))
sys.exit(1 if fails else 0)
