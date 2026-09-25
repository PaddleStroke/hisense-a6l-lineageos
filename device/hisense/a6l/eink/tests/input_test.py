#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Feeds synthetic evdev events into a6l_eink_mirror through FIFOs (--key-dev FIFO --touch-dev FIFO --touch-debug) and
checks the key state machine and the rear-touch -> front mapping end to end. usage: input_test.py KEYFIFO TOUCHFIFO"""
import struct, sys, time, os
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0x2f, 0x35, 0x36, 0x39
def ev(t, c, v): return struct.pack("llHHi", 0, 0, t, c, v)
kf = os.open(sys.argv[1], os.O_WRONLY); tf = os.open(sys.argv[2], os.O_WRONLY)
def key(v): os.write(kf, ev(EV_KEY, 616, v) + ev(EV_SYN, 0, 0))
def touch(slot, tid, x=None, y=None):
    b = ev(EV_ABS, ABS_MT_SLOT, slot) + ev(EV_ABS, ABS_MT_TRACKING_ID, tid)
    if x is not None: b += ev(EV_ABS, ABS_MT_POSITION_X, x) + ev(EV_ABS, ABS_MT_POSITION_Y, y)
    os.write(tf, b + ev(EV_SYN, 0, 0))
print("T0 touch while mirror OFF (must be dropped)", flush=True); touch(0, 1, 360, 720); time.sleep(0.2); touch(0, -1); time.sleep(0.3)
print("T1 short key press -> mirror", flush=True); key(1); time.sleep(0.2); key(0); time.sleep(1.0)
print("T2 touch centre (360,720) -> expect ~(540,1170)", flush=True); touch(0, 5, 360, 720); time.sleep(0.2)
print("T3 move to the picture top-left (27,0) -> (0,0)", flush=True); touch(0, 5, 27, 0); time.sleep(0.2); touch(0, -1); time.sleep(0.2)
print("T4 touch in the left white bar (5,700) -> ignored", flush=True); touch(1, 6, 5, 700); time.sleep(0.2); touch(1, -1); time.sleep(0.2)
print("T5 long key press -> clear", flush=True); key(1); time.sleep(1.2); key(0); time.sleep(0.5)
print("T6 short key press -> off", flush=True); key(1); time.sleep(0.1); key(0); time.sleep(0.8)
print("T7 touch after off -> dropped", flush=True); touch(0, 7, 360, 720); time.sleep(0.2); touch(0, -1); time.sleep(0.3)
print("DONE", flush=True)
