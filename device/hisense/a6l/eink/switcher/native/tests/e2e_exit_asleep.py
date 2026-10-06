#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""r5 bug hunt eink-display E3: a6l_dualux stopped (SIGTERM) while Android sleeps on the e-ink must hand the LCD back
(state lcd, lcd_blank 0, frontlight 0) WITHOUT switching the LCD backlight on over the dark panel (bl_power stays 4: it
belongs to the composer while asleep, which unblanks at the next wake-up). usage: e2e_exit_asleep.py WORKDIR BINARY"""
import os, subprocess, sys, time
W, BIN = sys.argv[1], sys.argv[2]
R = os.path.join(W, "root_ea"); P = os.path.join(W, "props_ea")
def w(path, v):
    os.makedirs(os.path.dirname(path), exist_ok=True); open(path, "w").write(str(v) + "\n")
def r(path):
    try: return open(path).read().strip()
    except FileNotFoundError: return ""
BL = R + "/sys/class/backlight/backlight"; FL = R + "/sys/class/leds/epd-backlight"; DRM = R + "/sys/class/drm/card0-DSI-1"
for p in (R, P): subprocess.run(["rm", "-rf", p]); os.makedirs(p)
w(BL + "/max_brightness", 4095); w(BL + "/brightness", 2000); w(BL + "/bl_power", 0); w(BL + "/scale", "non-linear")
w(FL + "/max_brightness", 255); w(FL + "/brightness", 0)
w(DRM + "/modes", "1080x2340"); w(DRM + "/dpms", "On"); w(R + "/sys/power/wake_lock", "")
log = open(os.path.join(W, "dualux_exit_asleep.log"), "w")
d = subprocess.Popen([BIN, "--sysroot", R, "--prop-dir", P, "--key-dev", "none", "--power-dev", "none", "--front-dev", "none",
                      "--no-uinput", "--exit-after", "20"], stdout=log, stderr=subprocess.STDOUT)
fails = 0
def check(c, what):
    global fails
    print(("PASS " if c else "FAIL ") + what); fails += 0 if c else 1
def prop(k): return r(os.path.join(P, k))
time.sleep(0.6)
w(P + "/sys.a6l.dualux.req", "1 eink"); time.sleep(0.6)
check(prop("vendor.dualux.state") == "eink" and r(BL + "/bl_power") == "4", "on the e-ink, LCD backlight blanked")
w(DRM + "/dpms", "Off"); time.sleep(0.6)            # Android goes to sleep (composer: bl_power 4)
check(prop("vendor.dualux.state") == "eink-asleep", "asleep on the e-ink")
check("a6l_dualux_lock 2000000000" in open(R + "/sys/power/wake_lock").read(), "eink-lockscreen: 2 s timed wakelock taken when falling asleep on the e-ink")
d.terminate(); d.wait(5)
check(prop("vendor.dualux.state") == "lcd" and prop("vendor.dualux.lcd_blank") == "0" and r(FL + "/brightness") == "0", "SIGTERM while asleep: LCD handed back, frontlight off")
check(r(BL + "/bl_power") == "4", "SIGTERM while asleep: backlight NOT switched on over the sleeping panel (bl_power %s)" % r(BL + "/bl_power"))
print("E2E_EXIT_ASLEEP %s (%d failed)" % ("PASS" if not fails else "FAIL", fails)); sys.exit(1 if fails else 0)
