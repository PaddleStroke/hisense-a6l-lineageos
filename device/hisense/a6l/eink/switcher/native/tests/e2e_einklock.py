#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""eink-lockscreen: a6l_einklock (host build) end to end, against
  part 1: a fake a6l_epdd (records every command + payload) — entry delay, minute ticks (period 2 s here), redraw on a
          settings / background change, "lock restore" on wake-up ("off" when the LCD is the screen), nothing in LCD mode
          or when disabled, startup restore after a restart;
  part 2: the real a6l_epdd (host build, --dry, fake TCON) — the mirror's page, lock frames, restore of that page.
usage: e2e_einklock.py WORKDIR EINKLOCK_BIN [EPDD_BIN FAKE_TCON_SO]"""
import os, socket, struct, subprocess, sys, threading, time
W, BIN = sys.argv[1], sys.argv[2]
EPDD = sys.argv[3] if len(sys.argv) > 4 else None; TCON = sys.argv[4] if len(sys.argv) > 4 else None
FR = 720 * 1440
fails = 0
def check(c, what):
    global fails
    print(("PASS " if c else "FAIL ") + what); fails += 0 if c else 1
def w(path, v):
    os.makedirs(os.path.dirname(path), exist_ok=True); open(path, "w").write(str(v) + "\n")

R = os.path.join(W, "elk_root"); P = os.path.join(W, "elk_props"); D = os.path.join(W, "elk_dump"); S = os.path.join(W, "elk_epd.sock")
for p in (R, P, D): subprocess.run(["rm", "-rf", p]); os.makedirs(p)
w(R + "/sys/class/power_supply/qcom-battery/capacity", 64); w(R + "/sys/class/power_supply/qcom-battery/status", "Discharging")
w(P + "/persist.sys.locale", "fr-FR"); w(P + "/vendor.dualux.state", "eink")

# ---------------- part 1: fake a6l_epdd ----------------
cmds = []; lock = threading.Lock()
def serve(srv):
    while True:
        try: c, _ = srv.accept()
        except OSError: return
        threading.Thread(target=client, args=(c,), daemon=True).start()
def client(c):
    f = c.makefile("rb")
    while True:
        line = f.readline()
        if not line: return
        line = line.decode().strip(); n = 0
        if line.startswith("lockframe "):
            parts = line.split(); n = int(parts[1]) * int(parts[2]); data = f.read(n)
            if len(data) != n: return
        with lock: cmds.append((time.monotonic(), line, n))
        rep = "OK lock picture shown in 900 ms (update 1), crtc off" if line.startswith("lockframe") else "OK lock restore: nothing to do" if line.startswith("lock restore") else "ERR unknown command"
        c.sendall((rep + "\n").encode())
if os.path.exists(S): os.unlink(S)
srv = socket.socket(socket.AF_UNIX); srv.bind(S); srv.listen(4); threading.Thread(target=serve, args=(srv,), daemon=True).start()
def newest():
    return max((os.path.join(D, f) for f in os.listdir(D)), key=os.path.getmtime)
def since(t0, prefix):
    with lock: return [c for c in cmds if c[0] >= t0 and c[1].startswith(prefix)]
def start(extra=()):
    log = open(os.path.join(W, "einklock_e2e.log"), "a")
    return subprocess.Popen([BIN, "--sysroot", R, "--prop-dir", P, "--epd-socket", S, "--period-s", "2", "--lead-ms", "300", "--clock", "realtime",
                             "--no-wakelock", "--dump", D, "--bg", "/data/a6l_eink_lock/bg.pgm", *extra], stdout=log, stderr=subprocess.STDOUT)
d = start()
time.sleep(1.6)
check(len(since(0, "lock restore")) == 1 and not since(0, "lockframe"), "start awake on the e-ink: one restore after 1 s (a previous instance may have died locked), no picture")
t = time.monotonic(); w(P + "/vendor.dualux.state", "eink-asleep"); time.sleep(1.4)
lf = since(t, "lockframe")
check(1 <= len(lf) <= 2 and lf[0][1] == "lockframe 720 1440 reading" and lf[0][2] == FR, "asleep on the e-ink: REGAL lock frame of 720x1440 bytes (%d, a tick may follow)" % len(lf))
check(lf and lf[0][0] - t >= 0.75, "entry delay >= 0.8 s (round 5: the LCD-off commit and the mirror's power off go first; no tick shortcut): %.2f s" % (lf[0][0] - t if lf else -1))
t = time.monotonic(); time.sleep(4.3)
n = len(since(t, "lockframe"))
check(2 <= n <= 3, "minute ticks while locked (period 2 s): %d frames in 4.3 s" % n)
t = time.monotonic(); w(P + "/persist.sys.a6l.eink.lock_bg", "black"); time.sleep(0.6)
check(len(since(t, "lockframe")) >= 1, "background setting changed: redrawn at once")
last = newest(); px = open(last, "rb").read()[-FR:]
check(px[0] == 0 and px[-1] == 0, "black background drawn (%s)" % last)
hdr = b"P5\n# a6l-lock seq=5\n720 1440\n255\n"
os.makedirs(R + "/data/a6l_eink_lock", exist_ok=True); open(R + "/data/a6l_eink_lock/bg.pgm", "wb").write(hdr + bytes([77]) * FR)
t = time.monotonic(); w(P + "/persist.sys.a6l.eink.lock_bg", "image"); w(P + "/vendor.dualux.lock_bg_seq", 5); time.sleep(0.6)
last = newest(); px = open(last, "rb").read()[-FR:]
check(len(since(t, "lockframe")) >= 1 and px[0] == 77 and px[-1] == 77, "new picture copied by init (vendor.dualux.lock_bg_seq): redrawn with it")
t = time.monotonic(); w(P + "/vendor.dualux.state", "eink"); time.sleep(0.6)
check(not since(t, "lock"), "round 5: wake-up on the e-ink: nothing during the first second (the LCD CRTC comes up too)")
time.sleep(1.0)
check([c[1] for c in since(t, "lock")] == ["lock restore"], "wake-up on the e-ink: 'lock restore' after 1 s (CRTC kept for the mirror), no more frames")
t = time.monotonic(); time.sleep(2.5)
check(not since(t, "lock"), "awake: nothing sent on the ticks")
w(P + "/vendor.dualux.state", "eink-asleep"); time.sleep(1.4)
t = time.monotonic(); w(P + "/vendor.dualux.state", "lcd"); time.sleep(1.3)  # round 6d: < the 1.5 s settle (LCD-mode ticks may follow after it)
check(not since(t, "lock"), "round 5: woken on the LCD: nothing sent (no e-ink modeset during the LCD wake-up; the lock picture stays)")
t = time.monotonic(); w(P + "/vendor.dualux.state", "eink"); w(P + "/persist.vendor.eink.mode", "mirror"); time.sleep(1.6)
check(not since(t, "lock"), "round 10: back on the e-ink later with the mirror (it was off: LCD-mode lock): no restore, the mirror's first frame replaces the lock picture")
check("no restore, the mirror's first frame replaces the lock picture" in open(os.path.join(W, "einklock_e2e.log")).read(), "round 10: logged")
t = time.monotonic(); w(P + "/persist.vendor.eink.mode", "off"); w(P + "/vendor.dualux.state", "lcd"); time.sleep(2.6)
check(len(since(t, "lockframe")) >= 1, "round 10: LCD-mode lock again")
t = time.monotonic(); w(P + "/vendor.dualux.state", "eink"); time.sleep(3.0)
check(not since(t, "lock restore"), "round 10: e-ink without the mirror: no restore before 4 s")
time.sleep(1.6)
check([c[1] for c in since(t, "lock restore")] == ["lock restore"], "round 10: e-ink without the mirror 4 s: 'lock restore' (fallback)")
w(P + "/persist.vendor.eink.mode", "off"); t = time.monotonic(); w(P + "/vendor.dualux.state", "lcd"); time.sleep(2.6)
lf = since(t, "lockframe")
check(len(lf) >= 1 and 1.9 <= lf[0][0] - t <= 2.6, "round 6: LCD mode, e-ink not mirroring: lock picture %.2f s after (2 s)" % (lf[0][0] - t if lf else -1))
t = time.monotonic(); time.sleep(4.3)
n = len(since(t, "lockframe")); check(2 <= n <= 3, "round 6: LCD mode: the clock follows the ticks (%d frames in 4.3 s)" % n)
t = time.monotonic(); w(P + "/vendor.dualux.awake", "0"); time.sleep(4.3)
n = len(since(t, "lockframe")); check(1 <= n <= 3, "round 6: LCD asleep: ticks go on after the 1.5 s settle (%d frames in 4.3 s)" % n)
w(P + "/persist.vendor.eink.mode", "mirror"); time.sleep(0.4)
t = time.monotonic(); time.sleep(2.5)
check(not since(t, "lock"), "LCD mode with the e-ink mirroring the LCD: nothing")
w(P + "/vendor.dualux.awake", "1")
# round 6d: no lock frame while a6l_dualux holds the lights for a screen switch (vendor.dualux.prepare), until 1.5 s after
w(P + "/persist.vendor.eink.mode", "off"); w(P + "/vendor.dualux.prepare", "123 lcd"); t = time.monotonic(); time.sleep(4.5)
check(not since(t, "lockframe"), "round 6d: LCD mode entry and ticks held while vendor.dualux.prepare is set (screen switch in progress)")
w(P + "/vendor.dualux.prepare", ""); t = time.monotonic(); time.sleep(2.3)
lf = since(t, "lockframe")
check(len(lf) >= 1 and lf[0][0] - t >= 1.15, "round 6d: lock picture %.2f s after the switch completed (1.5 s settle, prepare polled every 0.25 s)" % (lf[0][0] - t if lf else -1))
w(P + "/persist.vendor.eink.mode", "mirror"); time.sleep(0.4)
w(P + "/persist.sys.a6l.eink.lock", 0); t = time.monotonic(); w(P + "/vendor.dualux.state", "eink-asleep"); time.sleep(2.5)
check(not since(t, "lockframe"), "lock screen disabled: asleep on the e-ink shows nothing new")
w(P + "/persist.sys.a6l.eink.lock", 1); time.sleep(1.4)
t = time.monotonic(); w(P + "/persist.sys.a6l.eink.lock_clock", 0); time.sleep(0.6); time.sleep(2.5)
n = len(since(t, "lockframe"))
check(n == 1, "clock off: one redraw for the change, then no ticks (%d)" % n)
d.terminate(); d.wait(5)
# restart while asleep: picture again (the panel may still show it, REGAL drives nothing new then)
t = time.monotonic(); d = start(); time.sleep(1.6)
check(len(since(t, "lockframe")) == 1 and not since(t, "lock restore"), "restarted while asleep on the e-ink: lock picture redrawn, no restore first")
d.terminate(); d.wait(5)
# RTC wake fallback: freeze the daemon (SIGSTOP = suspended without an RTC wake) across a tick -> late tick ->
# "updated HH:MM" picture (no big digits) + persist.vendor.eink.lock_rtc=late; two on-time ticks -> live clock + ok
def big_digits(path):
    px = open(path, "rb").read()[-FR:]
    return sum(1 for y in range(300, 410, 4) for x in range(60, 660, 4) if px[y * 720 + x] < 128)
def lockrtc():
    p = P + "/persist.vendor.eink.lock_rtc"
    return open(p).read().strip() if os.path.exists(p) else ""
w(P + "/persist.sys.a6l.eink.lock_bg", "white"); w(P + "/persist.sys.a6l.eink.lock_clock", 1)
d = start(["--late-s", "1.5"]); time.sleep(1.6)
check(big_digits(newest()) > 50, "live clock picture first (unknown RTC state)")
os.kill(d.pid, 19); time.sleep(4.5); t = time.monotonic(); os.kill(d.pid, 18); time.sleep(0.8)
last = newest()
check(lockrtc() == "late", "late tick after a frozen period: persist.vendor.eink.lock_rtc=late")
check(bool(since(t, "lockframe")) and big_digits(last) == 0, "late tick: 'updated HH:MM' picture drawn at once, no big digits (%s)" % last)
time.sleep(5.2)  # round 6d: two 2 s ticks + redraw need margin (the 4.6 s window was tight)
check(lockrtc() == "ok" and big_digits(newest()) > 50, "two on-time ticks: live clock again, lock_rtc=ok")
w(P + "/persist.sys.a6l.eink.lock_clock_mode", "updated"); time.sleep(0.8)
check(big_digits(newest()) == 0, "lock_clock_mode=updated forces the variant")
os.unlink(P + "/persist.sys.a6l.eink.lock_clock_mode")
d.terminate(); d.wait(5)
# eink-round10: wall-clock notifications (SIGUSR1 = simulated TFD_TIMER_CANCEL_ON_SET cancel, sent by the kernel at every
# resume) inside one displayed minute: no redraw (before: a whole lock frame each time, 7 Oct 19:24:07/13/19/24)
while not (3 <= time.time() % 60 <= 48): time.sleep(0.5)
w(P + "/persist.vendor.eink.mode", "off"); w(P + "/vendor.dualux.state", "lcd")
d = start(["--period-s", "60"]); time.sleep(4.0)
lf0 = len(since(0, "lockframe")); t = time.monotonic()
for _ in range(3): os.kill(d.pid, 10); time.sleep(0.5)
time.sleep(0.5)
check(not since(t, "lockframe"), "round 10: 3 wall-clock notifications in the same minute: no redraw (%d frames)" % len(since(t, "lockframe")))
d.terminate(); d.wait(5)
log = open(os.path.join(W, "einklock_e2e.log")).read()
check("displayed minute unchanged: timer re-armed, no redraw (3)" in log, "round 10: logged, timer re-armed")
w(P + "/vendor.dualux.state", "eink")
srv.close()
log = open(os.path.join(W, "einklock_e2e.log")).read()
check("CLOCK_REALTIME" in log, "timer clock logged")

# ---------------- part 2: real a6l_epdd (--dry) ----------------
if EPDD:
    E = os.path.join(W, "elk_epdd"); subprocess.run(["rm", "-rf", E]); os.makedirs(E)
    open(E + "/wf.bin", "wb").write(os.urandom(0x70080))
    elog = open(E + "/epdd.log", "w")
    ep = subprocess.Popen([EPDD, "--dry", E + "/u", "--lib", TCON, "--waveform", E + "/wf.bin", "--listen", E + "/sock"], stdout=elog, stderr=subprocess.STDOUT)
    time.sleep(1.0)
    m = socket.socket(socket.AF_UNIX); m.connect(E + "/sock"); mf = m.makefile("rwb")
    mf.write(b"frame 720 1440 quality\n" + bytes([128]) * FR); mf.flush(); r1 = mf.readline().decode().strip()
    check(r1.startswith("OK shown"), "mirror page shown by the real a6l_epdd (%s)" % r1)
    w(P + "/vendor.dualux.state", "eink"); w(P + "/persist.sys.a6l.eink.lock_clock", 1)
    log2 = open(os.path.join(W, "einklock_e2e_real.log"), "w")
    d = subprocess.Popen([BIN, "--sysroot", R, "--prop-dir", P, "--epd-socket", E + "/sock", "--period-s", "2", "--lead-ms", "300", "--clock", "realtime", "--no-wakelock"],
                         stdout=log2, stderr=subprocess.STDOUT)
    time.sleep(0.8); w(P + "/vendor.dualux.state", "eink-asleep"); time.sleep(4.2)  # round 6d: a tick inside the 1.5 s settle is drawn after it
    w(P + "/vendor.dualux.state", "eink"); time.sleep(1.8)
    mf.write(b"status\n"); mf.flush(); st = mf.readline().decode().strip()
    d.terminate(); d.wait(5); mf.write(b"quit\n"); mf.flush(); mf.readline(); ep.wait(5)
    el = open(E + "/epdd.log").read(); kl = open(os.path.join(W, "einklock_e2e_real.log")).read()
    check("lock screen: first lock picture (the current picture is kept for the restore)" in el, "real epdd: first lock picture keeps the mirror's page")
    check(el.count("(lock)") >= 2, "real epdd: entry + minute lock frames driven (%d)" % el.count("(lock)"))
    check("(lock-restore)" in el and "earlier picture redrawn" in kl, "real epdd: the mirror's page redrawn at wake-up")
    check("lock=off" in st and "lock_frames=" in st, "real epdd status after wake-up: %s" % st[st.find("lock="):])
    # eink-round10 (7 Oct 19:24:34): LCD-mode lock -> switch to the e-ink. Before: "lock restore" redrew the page cached at
    # the first lock picture (stale page 1, GC16), then the mirror's page 2. Now: no restore, the mirror's first frame is
    # the GC16 clean that follows a lock picture (--lock-clean 1, the rc default).
    elog = open(E + "/epdd3.log", "w")
    ep = subprocess.Popen([EPDD, "--dry", E + "/u3", "--lib", TCON, "--waveform", E + "/wf.bin", "--listen", E + "/sock3", "--lock-clean", "1"], stdout=elog, stderr=subprocess.STDOUT)
    time.sleep(1.0)
    m = socket.socket(socket.AF_UNIX); m.connect(E + "/sock3"); mf = m.makefile("rwb")
    mf.write(b"frame 720 1440 reading\n" + bytes([200]) * FR); mf.flush(); r1 = mf.readline().decode().strip()  # "page 1" on the e-ink
    w(P + "/vendor.dualux.state", "lcd"); w(P + "/persist.vendor.eink.mode", "off"); w(P + "/vendor.dualux.awake", "1")
    for f in ("vendor.dualux.prepare", "persist.sys.a6l.eink.lock_clock_mode"):
        if os.path.exists(P + "/" + f): os.unlink(P + "/" + f)
    log3 = open(os.path.join(W, "einklock_e2e_real3.log"), "w")
    d = subprocess.Popen([BIN, "--sysroot", R, "--prop-dir", P, "--epd-socket", E + "/sock3", "--period-s", "60", "--lead-ms", "300", "--clock", "realtime", "--no-wakelock"],
                         stdout=log3, stderr=subprocess.STDOUT)
    time.sleep(4.0)  # LCD-mode lock entry (2 s, after the 1.5 s settle of the start)
    w(P + "/vendor.dualux.state", "eink"); time.sleep(0.6); w(P + "/persist.vendor.eink.mode", "mirror")  # dualux, then the mirror ON
    mf.write(b"frame 720 1440 reading\n" + bytes([90]) * FR); mf.flush(); r2 = mf.readline().decode().strip()  # mirror's first frame: "page 2"
    time.sleep(5.0)  # past the 4 s fallback
    d.terminate(); d.wait(5); mf.write(b"quit\n"); mf.flush(); mf.readline(); ep.wait(5)
    el = open(E + "/epdd3.log").read(); kl = open(os.path.join(W, "einklock_e2e_real3.log")).read()
    check("(lock)" in el and r1.startswith("OK"), "round 10 real epdd: LCD-mode lock picture over the e-ink page")
    check(r2.startswith("OK") and "first page after a lock picture: GC16 clean" in el, "round 10 real epdd: the mirror's first frame after the LCD-mode lock is the GC16 clean (%s)" % r2)
    check("lock-restore" not in el and "lock restore" not in el and "no restore, the mirror's first frame replaces the lock picture" in kl,
          "round 10 real epdd: no restore of the stale cached page")

print("E2E_EINKLOCK %s (%d failed)" % ("PASS" if not fails else "FAIL", fails)); sys.exit(1 if fails else 0)
