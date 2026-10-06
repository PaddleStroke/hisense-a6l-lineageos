#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""eink-round3 0016: a6l_dualux's main-loop watchdog must survive a system suspend longer than the watchdog period but
still kill a really hung loop; after an unexpected death on the e-ink the restarted daemon comes back on the e-ink
(at most once per 60 s), while a clean stop and a reboot still start on the LCD.
A suspend is simulated by SIGSTOP (the timer keeps running, as it did in s2idle on the phone) plus a bumped
/sys/power/suspend_stats/success in the fake sysroot; a hang is SIGSTOP without a suspend. usage: e2e_watchdog.py WORKDIR BINARY"""
import os, signal, subprocess, sys, time
W, BIN = sys.argv[1], sys.argv[2]
R = os.path.join(W, "root_wd"); P = os.path.join(W, "props_wd")
def w(path, v):
    os.makedirs(os.path.dirname(path), exist_ok=True); open(path, "w").write(str(v) + "\n")
def r(path):
    try: return open(path).read().strip()
    except FileNotFoundError: return ""
BL = R + "/sys/class/backlight/backlight"; FL = R + "/sys/class/leds/epd-backlight"; DRM = R + "/sys/class/drm/card0-DSI-1"
SS = R + "/sys/power/suspend_stats"
for p in (R, P): subprocess.run(["rm", "-rf", p]); os.makedirs(p)
w(BL + "/max_brightness", 4095); w(BL + "/brightness", 2000); w(BL + "/bl_power", 0); w(BL + "/scale", "non-linear")
w(FL + "/max_brightness", 255); w(FL + "/brightness", 0)
w(DRM + "/modes", "1080x2340"); w(DRM + "/dpms", "On")
w(SS + "/success", 3); w(SS + "/fail", 0)
fails = 0
def check(c, what):
    global fails
    print(("PASS " if c else "FAIL ") + what); fails += 0 if c else 1
def prop(k): return r(os.path.join(P, k))
logs = []
def start(tag):
    log = open(os.path.join(W, "dualux_wd_%s.log" % tag), "w"); logs.append(log.name)
    d = subprocess.Popen([BIN, "--sysroot", R, "--prop-dir", P, "--key-dev", "none", "--power-dev", "none", "--front-dev", "none",
                          "--no-uinput", "--watchdog-s", "1", "--exit-after", "30"], stdout=log, stderr=subprocess.STDOUT)
    time.sleep(0.7); return d
def logtext(): return open(logs[-1]).read()

d = start("a")
check(prop("vendor.dualux.state") == "lcd", "boot (no previous state): starts on the LCD")
w(P + "/sys.a6l.dualux.req", "1 eink"); time.sleep(0.7)
check(prop("vendor.dualux.state") == "eink" and r(BL + "/bl_power") == "4", "switched to the e-ink")
# 1) suspend longer than the watchdog: frozen 2.5 s (timer runs), the kernel suspend counter moved
d.send_signal(signal.SIGSTOP); time.sleep(2.5); w(SS + "/success", 4); d.send_signal(signal.SIGCONT); time.sleep(1.0)
check(d.poll() is None, "suspend 2.5 s > watchdog 1 s: daemon still alive")
check("expiry(ies) during a system suspend forgiven" in logtext(), "suspend: watchdog expiry forgiven and logged")
check(prop("vendor.dualux.state") == "eink", "suspend: still on the e-ink")
# 2) a second suspend counted by 'fail' only (aborted suspend also freezes tasks)
d.send_signal(signal.SIGSTOP); time.sleep(2.2); w(SS + "/fail", 1); d.send_signal(signal.SIGCONT); time.sleep(1.0)
check(d.poll() is None, "aborted suspend (fail counter): daemon still alive")
# 3) a real hang: frozen without any suspend -> killed by SIGALRM, state left on the e-ink
d.send_signal(signal.SIGSTOP); time.sleep(2.5); d.send_signal(signal.SIGCONT)
try: rc = d.wait(5)
except subprocess.TimeoutExpired: rc = None; d.kill()
check(rc == -signal.SIGALRM, "real hang (no suspend): killed by SIGALRM (rc %s)" % rc)
check(prop("vendor.dualux.state") == "eink", "hang: published state still eink (no clean exit)")
# 4) init restarts it: back on the e-ink
d = start("b")
check(prop("vendor.dualux.state") == "eink" and r(BL + "/bl_power") == "4" and prop("persist.vendor.eink.mode") == "mirror",
      "restart after the hang: e-ink restored (backlight blanked, mirror on)")
check("restoring the e-ink screen" in logtext(), "restart: restore logged")
# 5) it dies again within 60 s (crash loop) -> fail safe on the LCD
d.kill(); d.wait(5)
d = start("c")
check(prop("vendor.dualux.state") == "lcd" and r(BL + "/bl_power") == "0", "second unexpected death < 60 s later: LCD (crash-loop guard)")
check("already restored < 60 s ago" in logtext(), "crash-loop guard logged")
# 6) clean stop on the e-ink publishes lcd -> next start on the LCD even after the window
w(P + "/sys.a6l.dualux.req", "2 eink"); time.sleep(0.7)
check(prop("vendor.dualux.state") == "eink", "back on the e-ink")
d.terminate(); d.wait(5); w(P + "/vendor.dualux.restored_at", 1)
d = start("d")
check(prop("vendor.dualux.state") == "lcd", "clean stop (SIGTERM) on the e-ink: next start on the LCD")
# 7) restore disabled by setting
w(P + "/sys.a6l.dualux.req", "3 eink"); time.sleep(0.7); d.kill(); d.wait(5)
w(P + "/persist.sys.a6l.dualux.restore", 0); w(P + "/vendor.dualux.restored_at", 1)
d = start("e")
check(prop("vendor.dualux.state") == "lcd" and "restore is disabled" in logtext(), "persist.sys.a6l.dualux.restore=0: LCD")
d.terminate(); d.wait(5)
print("E2E_WATCHDOG %s (%d failed)" % ("PASS" if not fails else "FAIL", fails)); sys.exit(1 if fails else 0)
