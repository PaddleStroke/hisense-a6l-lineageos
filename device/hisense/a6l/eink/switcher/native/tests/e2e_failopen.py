#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""r5 review pass2 F20 (28 Sep 2026): the physical power key stays usable by Android whenever a6l_dualux cannot deliver
its replacement events. Real daemon (host build), fake sysfs + property files + FIFO input devices + a fake uinput file.
  A: uinput unavailable at start -> power key not grabbed in e-ink mode; uinput appears -> retried, grabbed
  B: SYN_DROPPED while an injected long press is held -> resync (EVIOCGKEY unreadable = up) -> injected up balanced
  C: power input device lost while an injected long press is held -> cancelled, up balanced
  D: uinput write failure (/dev/full) -> keyboard dropped, power key released to Android
usage: e2e_failopen.py WORKDIR BINARY"""
import os, struct, subprocess, sys, time
W, BIN = sys.argv[1], sys.argv[2]
R = os.path.join(W, "fo-root"); P = os.path.join(W, "fo-props")
def w(path, v):
    os.makedirs(os.path.dirname(path), exist_ok=True); open(path, "w").write(str(v) + "\n")
def r(path):
    try: return open(path).read().strip()
    except FileNotFoundError: return ""
fails = 0
def check(c, what):
    global fails
    print(("PASS " if c else "FAIL ") + what); fails += 0 if c else 1
def fixture():
    for p in (R, P): subprocess.run(["rm", "-rf", p]); os.makedirs(p)
    BL = R + "/sys/class/backlight/backlight"; DRM = R + "/sys/class/drm/card0-DSI-1"
    w(BL + "/max_brightness", 4095); w(BL + "/brightness", 2000); w(BL + "/bl_power", 0); w(BL + "/scale", "non-linear")
    w(DRM + "/modes", "1080x2340"); w(DRM + "/dpms", "On")
    for f in ("key", "power"):
        p = os.path.join(W, "fo-" + f + ".fifo")
        if os.path.exists(p): os.unlink(p)
        os.mkfifo(p)
def start(uinput, logname):
    fixture()
    log = open(os.path.join(W, logname), "w")
    d = subprocess.Popen([BIN, "--sysroot", R, "--prop-dir", P, "--key-dev", W + "/fo-key.fifo", "--power-dev", W + "/fo-power.fifo",
                          "--front-dev", "none", "--fake-uinput", uinput, "--exit-after", "30"], stdout=log, stderr=subprocess.STDOUT)
    time.sleep(0.3)
    return d, open(W + "/fo-key.fifo", "wb", buffering=0), open(W + "/fo-power.fifo", "wb", buffering=0), os.path.join(W, logname)
def ev(f, t, code, val):
    now = time.time(); f.write(struct.pack("llHHi", int(now), int((now % 1) * 1e6), t, code, val))
def key(f, code, val): ev(f, 1, code, val); ev(f, 0, 0, 0)
def press(f, code, ms): key(f, code, 1); time.sleep(ms / 1000); key(f, code, 0); time.sleep(0.4)
def prop(k): return r(os.path.join(P, k))
def uevents(path):
    try: b = open(path, "rb").read()
    except FileNotFoundError: return []
    return [(c, v) for (_, _, t, c, v) in struct.iter_unpack("llHHi", b) if t == 1]

# ---- A + B + C
UD = os.path.join(W, "fo-uinput-dir"); subprocess.run(["rm", "-rf", UD]); UI = UD + "/keys.bin"
d, kf, pf, LOG = start(UI, "fo-a.log")
check("power key left to Android" in r(LOG), "A: uinput unavailable at start is reported")
w(P + "/sys.a6l.dualux.req", "1 eink"); time.sleep(0.4)
check(prop("vendor.dualux.state") == "eink", "A: e-ink active")
check("power key grabbed" not in r(LOG), "A: no uinput -> power key NOT grabbed (Android keeps it)")
press(pf, 116, 120)
check(prop("vendor.dualux.state") in ("eink", "eink-asleep") and "power key: e-ink -> LCD" not in r(LOG), "A: power press not consumed by the daemon")
os.makedirs(UD); time.sleep(5.6)
check("uinput a6l-dualux-keys created" in r(LOG) and "power key grabbed" in r(LOG), "A: uinput retried -> power key grabbed")
press(pf, 116, 120)
check(prop("vendor.dualux.state") == "lcd", "A: grabbed power short press -> LCD")
# B: SYN_DROPPED hides the release of an injected long press
w(P + "/sys.a6l.dualux.req", "2 eink"); time.sleep(0.4)
key(pf, 116, 1); time.sleep(0.7)
check(uevents(UI)[-1:] == [(116, 1)], "B: long press injected (down delivered)")
ev(pf, 0, 3, 0); ev(pf, 1, 116, 0); ev(pf, 0, 0, 0); time.sleep(0.4)   # SYN_DROPPED, the (lost) release, SYN_REPORT
check("SYN_DROPPED (resync)" in r(LOG) and "power key press lost, cancelled" in r(LOG), "B: SYN_DROPPED -> resync cancels the press")
check(uevents(UI)[-1:] == [(116, 0)], "B: injected KEY_POWER down balanced by an up (%s)" % uevents(UI)[-2:])
check(prop("vendor.dualux.state") == "eink", "B: no screen switch from the resync")
# C: power device lost while an injected long press is held
key(pf, 116, 1); time.sleep(0.7)
check(uevents(UI)[-1:] == [(116, 1)], "C: long press injected")
pf.close(); time.sleep(0.4)
check("device lost: power key press lost, cancelled" in r(LOG) and uevents(UI)[-1:] == [(116, 0)], "C: device lost -> injected down balanced by an up")
d.terminate(); d.wait(5); kf.close()
check("power key released" in r(LOG) and prop("vendor.dualux.state") == "lcd", "A-C: SIGTERM restores the LCD")

# ---- D: uinput write failure
d, kf, pf, LOG = start("/dev/full", "fo-d.log")
press(kf, 616, 100)   # LCD -> e-ink + KEY_WAKEUP injection -> write fails (ENOSPC)
lg = r(LOG)
check(prop("vendor.dualux.state") == "eink", "D: e-ink active")
check("keyboard dropped, power key given back to Android" in lg, "D: uinput write failure detected")
check(lg.rfind("power key released") > lg.rfind("power key grabbed") >= 0, "D: power key grab released after the write failure")
d.terminate(); d.wait(5); kf.close(); pf.close()
print("E2E_FAILOPEN %s (%d failed)" % ("PASS" if not fails else "FAIL", fails)); sys.exit(1 if fails else 0)
