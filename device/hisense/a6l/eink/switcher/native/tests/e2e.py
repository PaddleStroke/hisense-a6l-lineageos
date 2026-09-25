#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""a6l_dualux end-to-end on the host: fake sysfs + property dir + FIFO input devices. Prints PASS/FAIL per check."""
import os, struct, subprocess, sys, time, math
W, BIN = sys.argv[1], sys.argv[2]
R = os.path.join(W, "root"); P = os.path.join(W, "props")
def w(path, v):
    os.makedirs(os.path.dirname(path), exist_ok=True); open(path, "w").write(str(v) + "\n")
def r(path):
    try: return open(path).read().strip()
    except FileNotFoundError: return ""
BL = R + "/sys/class/backlight/backlight"; FL = R + "/sys/class/leds/epd-backlight"; DRM = R + "/sys/class/drm/card0-DSI-1"
for p in (R, P): subprocess.run(["rm", "-rf", p]); os.makedirs(p)
w(BL + "/max_brightness", 4095); w(BL + "/brightness", 2000); w(BL + "/bl_power", 4); w(BL + "/scale", "non-linear")  # 4 = left over by a crashed instance
w(FL + "/max_brightness", 255); w(FL + "/brightness", 0)
w(DRM + "/modes", "1080x2340"); w(DRM + "/dpms", "On")
w(R + "/sys/class/drm/card0-DSI-2/modes", "384x725")
for f in ("key", "power"):
    p = os.path.join(W, f + ".fifo")
    if os.path.exists(p): os.unlink(p)
    os.mkfifo(p)
log = open(os.path.join(W, "dualux.log"), "w")
d = subprocess.Popen([BIN, "--sysroot", R, "--prop-dir", P, "--key-dev", W + "/key.fifo", "--power-dev", W + "/power.fifo",
                      "--front-dev", "none", "--no-uinput", "--exit-after", "30"], stdout=log, stderr=subprocess.STDOUT)
time.sleep(0.3)
kf = open(W + "/key.fifo", "wb", buffering=0); pf = open(W + "/power.fifo", "wb", buffering=0)
def ev(f, code, val):
    t = time.time(); f.write(struct.pack("llHHi", int(t), int((t % 1) * 1e6), 1, code, val) + struct.pack("llHHi", int(t), 0, 0, 0, 0))
def press(f, code, ms):
    ev(f, code, 1); time.sleep(ms / 1000); ev(f, code, 0); time.sleep(0.4)
fails = 0
def check(c, what):
    global fails
    print(("PASS " if c else "FAIL ") + what); fails += 0 if c else 1
def prop(k): return r(os.path.join(P, k))
def hlg(e):
    h = e * 12; return math.sqrt(h) * 0.5 if h <= 1 else 0.17883277 * math.log(h - 0.28466892) + 0.55991073
time.sleep(0.5)
check(prop("vendor.dualux.state") == "lcd" and prop("persist.vendor.eink.mode") == "off", "boot: LCD, mirror off")
check(r(BL + "/bl_power") == "0", "restart after a crash in e-ink mode: LCD unblanked")
press(kf, 616, 100)
check(prop("vendor.dualux.state") == "eink", "e-ink key -> state eink (%s)" % prop("vendor.dualux.state"))
check(prop("persist.vendor.eink.mode") == "mirror", "mirror on")
check(r(BL + "/bl_power") == "4", "LCD backlight blanked")
lin = None
for b in (0.5, 0.1):
    v = int(4095 * hlg(b)); w(BL + "/brightness", v); time.sleep(0.4)
    got = int(r(FL + "/brightness") or -1); check(abs(got - round(255 * b)) <= 2, "slider linear %.1f (LCD %d) -> frontlight %d" % (b, v, got))
w(BL + "/bl_power", 0); time.sleep(0.35); check(r(BL + "/bl_power") == "4", "composer unblank at wake-up is undone")
w(DRM + "/dpms", "Off"); time.sleep(0.4)
check(prop("vendor.dualux.state") == "eink-asleep", "LCD CRTC off -> eink-asleep")
check(r(FL + "/brightness") == "0", "frontlight off while asleep")
press(kf, 616, 100); check("inject WAKEUP" in r(W + "/dualux.log"), "e-ink key while asleep -> WAKEUP")
w(DRM + "/dpms", "On"); time.sleep(0.4)
check(prop("vendor.dualux.state") == "eink" and r(FL + "/brightness") != "0", "awake again on the e-ink, frontlight back")
press(kf, 616, 100); check("inject SLEEP" in r(W + "/dualux.log"), "e-ink key on e-ink -> SLEEP after the double-press window (stock)")
n0 = prop("vendor.eink.clear_req") or "0"
ev(kf, 616, 1); time.sleep(0.08); ev(kf, 616, 0); time.sleep(0.12); ev(kf, 616, 1); time.sleep(0.08); ev(kf, 616, 0); time.sleep(0.6)
check(prop("vendor.eink.clear_req") == str(int(n0) + 1) and r(W + "/dualux.log").count("inject SLEEP down") == 1, "double press on e-ink -> clear, no sleep (clear_req %s, %d sleeps)" % (prop("vendor.eink.clear_req"), r(W + "/dualux.log").count("inject SLEEP down")))
press(pf, 116, 600)
lg = r(W + "/dualux.log")
check("inject POWER (long press handed to Android) down" in lg and "inject POWER up" in lg, "power long press re-injected")
check(prop("vendor.dualux.state") == "eink", "still e-ink after the power long press")
press(pf, 116, 120)
check(prop("vendor.dualux.state") == "lcd" and prop("persist.vendor.eink.mode") == "off", "power short on e-ink -> LCD")
check(r(BL + "/bl_power") == "0" and r(FL + "/brightness") == "0", "LCD unblanked, frontlight off")
press(pf, 116, 120); check(prop("vendor.dualux.state") == "lcd", "power on LCD: untouched")
w(P + "/sys.a6l.dualux.req", "1 toggle"); time.sleep(0.4); check(prop("vendor.dualux.state") == "eink", "request toggle -> e-ink")
w(P + "/sys.a6l.dualux.req", "2 clear"); time.sleep(0.4); check(prop("vendor.eink.clear_req") == "2", "request clear -> clear_req 2")
press(kf, 616, 1100); check(prop("vendor.eink.clear_req") == "3" and prop("vendor.dualux.state") == "eink", "long e-ink key -> clear, no switch")
w(P + "/persist.sys.a6l.dualux.fl_max_pct", 50); w(BL + "/brightness", 4095); time.sleep(1.4)
check(r(FL + "/brightness") == "128", "fl_max_pct 50 -> 128 (%s)" % r(FL + "/brightness"))
w(DRM + "/dpms", "Off"); time.sleep(0.4); press(pf, 116, 120)
check(prop("vendor.dualux.state") == "lcd", "power while e-ink asleep -> LCD (stock wake on primary)")
w(P + "/sys.a6l.dualux.req", "3 eink"); time.sleep(0.4); w(DRM + "/dpms", "On"); time.sleep(0.4)
d.terminate(); d.wait(5)
check(r(BL + "/bl_power") == "0" and prop("vendor.dualux.state") == "lcd" and r(FL + "/brightness") == "0", "SIGTERM restores the LCD")
print("E2E %s (%d failed)" % ("PASS" if not fails else "FAIL", fails)); sys.exit(1 if fails else 0)
