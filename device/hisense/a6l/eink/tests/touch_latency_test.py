#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""eink-round6: a flick on the rear touchscreen must reach Android with its own timing while the mirror is capturing.
On the phone one capture + resize blocks the capture loop ~200 ms (7 Oct: capture=148 ms resize=48 ms); here
--capture-delay-ms 250 --interval 20 keeps the loop inside a capture almost all the time. A flick of 9 reports 12 ms
apart (DOWN, 7 moves, UP) is written to the FIFO "touchscreen"; every forwarded SYN_REPORT is time-stamped by the mirror
(--touch-debug: A6L_MIRROR TOUCH_SYN t=<CLOCK_MONOTONIC>), the same clock as time.monotonic() here.
Pass: every report forwarded within 40 ms of being written, and the forwarded reports keep their spacing (the span from
the first to the last forwarded report is 70-130 % of the written span: no burst, no gap).
usage: touch_latency_test.py MIRROR FRAMES_PATTERN WORKDIR THREAD(0|1)
prints TOUCH_LATENCY max_ms=.. span_ratio=.. and TOUCH_LATENCY_TEST PASS|FAIL (THREAD=0 is expected to FAIL)"""
import os, struct, subprocess, sys, time
EV_SYN, EV_ABS = 0, 3
ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0x2f, 0x35, 0x36, 0x39
def ev(t, c, v): return struct.pack("llHHi", 0, 0, t, c, v)
mirror, frames, w, thread = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
fifo = os.path.join(w, f"touch_lat{thread}.fifo"); log = os.path.join(w, f"touch_lat{thread}.log")
if os.path.exists(fifo): os.unlink(fifo)
os.mkfifo(fifo)
p = subprocess.Popen([mirror, "--dry", "--source", "files:" + frames, "--no-props", "--mode", "mirror", "--key-dev", "none",
                      "--touch-dev", fifo, "--touch-debug", "--touch-thread", thread, "--capture-delay-ms", "250", "--interval", "20"],
                     stdout=open(log, "w"), stderr=subprocess.STDOUT)
time.sleep(0.6)
fd = os.open(fifo, os.O_WRONLY)
time.sleep(1.0)
sent = []
xs = [360 - 20 * k for k in range(8)]			# a right-to-left flick around the picture centre
for k, x in enumerate(xs):
    b = ev(EV_ABS, ABS_MT_SLOT, 0)
    if k == 0: b += ev(EV_ABS, ABS_MT_TRACKING_ID, 11)
    b += ev(EV_ABS, ABS_MT_POSITION_X, x) + ev(EV_ABS, ABS_MT_POSITION_Y, 720) + ev(EV_SYN, 0, 0)
    os.write(fd, b); sent.append(time.monotonic()); time.sleep(0.012)
os.write(fd, ev(EV_ABS, ABS_MT_SLOT, 0) + ev(EV_ABS, ABS_MT_TRACKING_ID, -1) + ev(EV_SYN, 0, 0)); sent.append(time.monotonic())
time.sleep(0.8)
os.close(fd); time.sleep(0.3); p.terminate()
try: p.wait(3)
except subprocess.TimeoutExpired: p.kill()
got = [float(l.split("t=")[1]) for l in open(log) if "TOUCH_SYN t=" in l]
ok = len(got) == len(sent)
lat = [g - s for g, s in zip(got, sent)] if ok else []
mx = max(lat) * 1000 if lat else -1
ratio = (got[-1] - got[0]) / (sent[-1] - sent[0]) if ok else 0
print(f"TOUCH_LATENCY thread={thread} reports sent={len(sent)} forwarded={len(got)} max_ms={mx:.1f} span_ratio={ratio:.2f}")
print("TOUCH_LATENCY_TEST", "PASS" if ok and mx < 40 and 0.7 <= ratio <= 1.3 else "FAIL")
