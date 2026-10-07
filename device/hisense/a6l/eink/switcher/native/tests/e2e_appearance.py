#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real daemon against fake sysfs; no phone access."""
import pathlib,subprocess,time,sys
w=pathlib.Path(sys.argv[1])/'appearance';w.mkdir(parents=True,exist_ok=True)
r=w/'root';p=w/'props';p.mkdir(exist_ok=True)
b=r/'sys/class/backlight/backlight';f=r/'sys/class/leds/epd-backlight';d=r/'sys/class/drm/card0-DSI-1'
def put(path,v):path.parent.mkdir(parents=True,exist_ok=True);path.write_text(str(v)+'\n')
def get(path):return path.read_text().strip() if path.exists() else ''
def prop(k):return get(p/k)
for path,v in [(b/'max_brightness',4095),(b/'brightness',4095),(b/'bl_power',0),(b/'scale','linear'),(f/'max_brightness',255),(f/'brightness',0),(d/'modes','1080x2340'),(d/'dpms','On'),(p/'sys.a6l.dualux.theme_sync',1)]:put(path,v)
log=(w/'daemon.log').open('w');proc=subprocess.Popen([sys.argv[2],'--sysroot',str(r),'--prop-dir',str(p),'--key-dev','none','--power-dev','none','--front-dev','none','--fake-uinput',str(w/'keys'),'--exit-after','20'],stdout=log,stderr=subprocess.STDOUT)
checks=0
def check(c,msg):
 global checks
 checks+=1
 if not c:raise AssertionError(msg)
 print('PASS',msg)
def wait(fn,secs=2):
 end=time.monotonic()+secs
 while time.monotonic()<end:
  if fn():return
  time.sleep(.025)
 raise AssertionError('timed out: '+str(fn))
try:
 wait(lambda:prop('vendor.dualux.state')=='lcd')
 put(p/'sys.a6l.dualux.req','1 eink');wait(lambda:prop('vendor.dualux.prepare').endswith(' eink') and get(b/'bl_power')=='4' and prop('persist.vendor.eink.mode')=='off')  # round 6f: prepare is published first, the lights follow
 a=prop('vendor.dualux.prepare')
 check(get(b/'bl_power')=='4' and get(f/'brightness')=='0' and prop('persist.vendor.eink.mode')=='off','prepare holds lights and stops mirror')
 put(p/'sys.a6l.dualux.ready','1 eink');time.sleep(.15)
 check(prop('persist.vendor.eink.mode')=='off','stale theme acknowledgement rejected')
 put(p/'sys.a6l.dualux.ready',a);wait(lambda:prop('persist.vendor.eink.mode')=='mirror')
 check(get(f/'brightness')=='0','theme ready starts capture while frontlight held')
 put(p/'vendor.eink.ready','1 eink');time.sleep(.15)
 check(get(f/'brightness')=='0','stale first-frame acknowledgement rejected')
 put(p/'vendor.eink.ready',a);wait(lambda:prop('vendor.dualux.prepare')=='')
 check(get(f/'brightness')=='255' and get(b/'bl_power')=='4','matching first frame releases rear light')
 put(p/'sys.a6l.dualux.req','2 lcd');wait(lambda:prop('vendor.dualux.prepare').endswith(' lcd'))
 c=prop('vendor.dualux.prepare');check(c!=a and get(b/'bl_power')=='4','LCD transition has fresh token and stays blank')
 put(p/'sys.a6l.dualux.ready',a);time.sleep(.15);check(get(b/'bl_power')=='4','earlier target acknowledgement cannot light LCD')
 put(p/'sys.a6l.dualux.ready',c);wait(lambda:get(b/'bl_power')=='0')
 check(get(f/'brightness')=='0','prepared LCD restores light with rear light off')
 put(p/'sys.a6l.dualux.req','3 eink');wait(lambda:prop('vendor.dualux.prepare').endswith(' eink'))
 wait(lambda:prop('vendor.dualux.prepare')=='',4)
 check(prop('persist.vendor.eink.mode')=='mirror' and get(f/'brightness')=='255','missing controller acknowledgement fails open after deadline')
 put(p/'sys.a6l.dualux.req','4 lcd');wait(lambda:prop('vendor.dualux.prepare').endswith(' lcd'))
 z=prop('vendor.dualux.prepare');put(d/'dpms','Off');time.sleep(.15);put(p/'sys.a6l.dualux.ready',z);wait(lambda:prop('vendor.dualux.prepare')=='')
 check(get(b/'bl_power')=='4' and get(f/'brightness')=='0','ready LCD while asleep does not override Android blank')
 # eink-round2: prepare begun while Android sleeps on the e-ink; the app can only answer after the wake-up
 put(d/'dpms','On');time.sleep(.2)
 put(p/'sys.a6l.dualux.req','5 eink');wait(lambda:prop('vendor.dualux.prepare').endswith(' eink'));e=prop('vendor.dualux.prepare')
 put(p/'sys.a6l.dualux.ready',e);put(p/'vendor.eink.ready',e);wait(lambda:prop('vendor.dualux.prepare')=='')
 put(d/'dpms','Off');wait(lambda:prop('vendor.dualux.state')=='eink-asleep')
 t0=time.monotonic();put(p/'sys.a6l.dualux.req','6 lcd');wait(lambda:prop('vendor.dualux.prepare').endswith(' lcd'));g=prop('vendor.dualux.prepare')
 time.sleep(2.5);put(d/'dpms','On')
 time.sleep(max(0,t0+3.5-time.monotonic()))
 check(prop('vendor.dualux.prepare')==g,'prepare begun asleep: deadline restarted at wake-up, no fail-open 3 s after the request')
 put(p/'sys.a6l.dualux.ready',g);wait(lambda:prop('vendor.dualux.prepare')=='')
 check(get(b/'bl_power')=='0','app acknowledgement after the wake-up lights the LCD normally')
 print('APPEARANCE_E2E PASS',checks)
finally:
 proc.terminate();proc.wait(timeout=3);log.close()
