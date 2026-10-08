#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""eink-round11 reader sleep, a6l_dualux end to end (host build, fake sysfs, property files, key FIFOs, fake uinput).
On the e-ink with persist.sys.a6l.eink.reader_sleep=1 and the app's sys.a6l.dualux.reader_ok=1: no page change
(vendor.eink.activity) for reader_sleep_s -> vendor.dualux.reader_sleep=1 + KEY_SLEEP; asleep + volume key -> KEY_WAKEUP,
then the volume key re-injected once awake (page turn); power-key wake -> no page turn; Android staying awake -> flag
cleared, idle timer restarted; reader_ok=0 or the LCD -> never. usage: e2e_reader.py WORKDIR DUALUX_BIN"""
import os, pathlib, struct, subprocess, sys, time
w = pathlib.Path(sys.argv[1]) / 'reader'; w.mkdir(parents=True, exist_ok=True)
r = w / 'root'; p = w / 'props'
subprocess.run(['rm', '-rf', str(r), str(p)]); p.mkdir(parents=True)
b = r / 'sys/class/backlight/backlight'; f = r / 'sys/class/leds/epd-backlight'; d = r / 'sys/class/drm/card0-DSI-1'
def put(path, v): path.parent.mkdir(parents=True, exist_ok=True); path.write_text(str(v) + '\n')
def get(path): return path.read_text().strip() if path.exists() else ''
def prop(k): return get(p / k)
for path, v in [(b / 'max_brightness', 4095), (b / 'brightness', 2000), (b / 'bl_power', 0), (b / 'scale', 'linear'), (f / 'max_brightness', 255),
                (f / 'brightness', 0), (d / 'modes', '1080x2340'), (d / 'dpms', 'On'), (p / 'sys.a6l.dualux.theme_sync', 0)]: put(path, v)
for x in ('key', 'voldown'):
    fifo = w / x
    if fifo.exists(): fifo.unlink()
    os.mkfifo(fifo)
keys = w / 'keys'
if keys.exists(): keys.unlink()
log = (w / 'daemon.log').open('w')
proc = subprocess.Popen([sys.argv[2], '--sysroot', str(r), '--prop-dir', str(p), '--key-dev', str(w / 'key'), '--voldown-dev', str(w / 'voldown'),
                         '--power-dev', 'none', '--front-dev', 'none', '--fake-uinput', str(keys), '--exit-after', '60'], stdout=log, stderr=subprocess.STDOUT)
kf = os.open(w / 'key', os.O_WRONLY); vf = os.open(w / 'voldown', os.O_WRONLY)
def ev(t, c, v): return struct.pack('llHHi', 0, 0, t, c, v)
def press(fd, code): os.write(fd, ev(1, code, 1) + ev(0, 0, 0)); time.sleep(0.05); os.write(fd, ev(1, code, 0) + ev(0, 0, 0))
def injected():  # [(code, value)] of EV_KEY events written to the fake uinput
    data = keys.read_bytes() if keys.exists() else b''
    out = []
    for i in range(0, len(data) - 23, 24):
        _, _, t, c, v = struct.unpack('llHHi', data[i:i + 24])
        if t == 1: out.append((c, v))
    return out
def count(code): return sum(1 for c, v in injected() if c == code and v == 1)
SLEEP, WAKEUP, VOLUP, VOLDOWN = 142, 143, 115, 114
checks = 0
def check(c, msg):
    global checks
    checks += 1
    if not c: raise AssertionError(msg)
    print('PASS', msg, flush=True)
def wait(fn, secs=2):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if fn(): return True
        time.sleep(0.025)
    return False
try:
    wait(lambda: prop('vendor.dualux.state') == 'lcd')
    put(p / 'persist.sys.a6l.eink.reader_sleep', 1); put(p / 'persist.sys.a6l.eink.reader_sleep_s', 5); put(p / 'sys.a6l.dualux.reader_ok', 1)
    time.sleep(6.5)
    check(count(SLEEP) == 0, 'LCD screen: no reader sleep')
    put(p / 'sys.a6l.dualux.req', '1 eink'); check(wait(lambda: prop('vendor.dualux.state') == 'eink'), 'switched to the e-ink')
    t0 = time.monotonic()
    for k in range(3):  # page changes every 2 s (the mirror's frame ACKs) keep it awake
        time.sleep(2); put(p / 'vendor.eink.activity', '%.3f' % time.monotonic())
    check(count(SLEEP) == 0, 'page changes every 2 s (vendor.eink.activity): no sleep')
    check(wait(lambda: count(SLEEP) == 1, 7), 'no page change for 5 s: KEY_SLEEP injected')
    check(prop('vendor.dualux.reader_sleep') == '1', 'vendor.dualux.reader_sleep = 1 (a6l_einklock keeps the page)')
    put(d / 'dpms', 'Off'); check(wait(lambda: prop('vendor.dualux.state') == 'eink-asleep'), 'Android asleep on the e-ink')
    nw = count(WAKEUP); press(kf, VOLUP)
    check(wait(lambda: count(WAKEUP) == nw + 1), 'volume up while reader-asleep: KEY_WAKEUP injected')
    time.sleep(0.4); check(count(VOLUP) == 0, 'no page turn before Android is awake')
    put(d / 'dpms', 'On')
    check(wait(lambda: count(VOLUP) == 1), 'awake: the volume-up page turn is re-injected')
    check(prop('vendor.dualux.reader_sleep') == '0', 'reader sleep flag cleared at the wake-up')
    check(wait(lambda: count(SLEEP) == 2, 7), 'idle again: second reader sleep')
    put(d / 'dpms', 'Off'); wait(lambda: prop('vendor.dualux.state') == 'eink-asleep')
    nw = count(WAKEUP); press(vf, VOLDOWN); check(wait(lambda: count(WAKEUP) == nw + 1), 'volume down (pm8941_resin device) also wakes')
    put(d / 'dpms', 'On'); check(wait(lambda: count(VOLDOWN) == 1), 'and turns the page back')
    check(wait(lambda: count(SLEEP) == 3, 7), 'third reader sleep')
    put(d / 'dpms', 'Off'); wait(lambda: prop('vendor.dualux.state') == 'eink-asleep'); time.sleep(0.3)
    put(d / 'dpms', 'On')  # woken by the power key (Android's own wake-up)
    check(wait(lambda: prop('vendor.dualux.reader_sleep') == '0'), 'other wake-up: flag cleared')
    time.sleep(0.5); check(count(VOLUP) == 1 and count(VOLDOWN) == 1, 'other wake-up: no page turn injected')
    check(wait(lambda: count(SLEEP) == 4, 7), 'fourth reader sleep request')
    n = count(SLEEP); time.sleep(3.6)  # Android does not sleep (dpms stays On)
    check(prop('vendor.dualux.reader_sleep') == '0', 'Android stayed awake 3 s after the request: flag cleared')
    time.sleep(1.0); check(count(SLEEP) == n, 'idle timer restarted: no immediate second request')
    put(p / 'sys.a6l.dualux.reader_ok', 0); n = count(SLEEP); time.sleep(6.5)
    check(count(SLEEP) == n, 'reader_ok = 0 (a keyguard is set): never')
    # eink-round9: the foreground app is not in the reader allowlist -> never; allowed again -> it sleeps
    put(p / 'sys.a6l.dualux.reader_ok', 1); put(p / 'sys.a6l.dualux.reader_fg', 0); n = count(SLEEP); time.sleep(6.5)
    check(count(SLEEP) == n, 'reader_fg = 0 (foreground app not a reader app): never')
    put(p / 'sys.a6l.dualux.reader_fg', 1)
    check(wait(lambda: count(SLEEP) == n + 1, 7), 'reader_fg = 1: reader sleep again')
    print('READER_E2E PASS', checks)
finally:
    proc.terminate(); proc.wait(timeout=3); log.close(); os.close(kf); os.close(vf)
