#!/usr/bin/env python3
"""Laptop: boot installed r6f from recovery, radio-off, GPU 3D-copy UI test.
No flash. Private off-screen executable/libraries compare old/new Mesa on
Android's actual kernel; they do not replace vendor libraries. User checks UI.
"""
import json
import subprocess
import time
from pathlib import Path

KIT=Path.home()/'A6L-usb-20260915/rom-r6f'
LOG=KIT/('logs/gpu-copy-android-20261002-'+time.strftime('%H%M%S',time.gmtime()))
LOG.mkdir(exist_ok=False)
A=['adb','-s','1e529013']
FLAGS='sysmem,noblit'

def run(cmd,name,timeout=10,check=False):
    r=subprocess.run(A+['shell',cmd],capture_output=True,text=True,timeout=timeout)
    (LOG/(name+'.txt')).write_text(r.stdout+r.stderr)
    if check and r.returncode:raise RuntimeError((name,r.returncode,r.stdout,r.stderr))
    return r

# Required user action has already returned the phone to the explicit recovery.
r=subprocess.run(['adb','-s','HLTE730T-PROBE','shell',
                  '/system/bin/toybox tr -d "\\000" < /proc/device-tree/chosen/hisense,a6l-image'],
                 capture_output=True,text=True,timeout=10)
assert r.returncode==0 and 'v74' in r.stdout,'wrong recovery'
subprocess.run(['adb','-s','HLTE730T-PROBE','shell','echo b > /proc/sysrq-trigger'],timeout=10,check=True)
deadline=time.monotonic()+300
while time.monotonic()<deadline:
    try:
        r=run('cat /proc/sys/kernel/random/boot_id; getprop ro.vendor.a6l.rom.build; getprop ro.persistent_properties.ready', 'initial-state')
        v=r.stdout.replace('\r','').splitlines()
        if r.returncode==0 and len(v)==3 and v[1:]==['r6f','true']:break
    except subprocess.TimeoutExpired:pass
    time.sleep(2)
else:raise RuntimeError('Android did not return')
boot=v[0];print('NEW_ANDROID_BOOT',boot,flush=True)
r=run(f'setprop persist.vendor.a6l.radio 0; setprop debug.mesa.fd.mesa.debug {FLAGS}; '
      'getprop persist.vendor.a6l.radio; getprop debug.mesa.fd.mesa.debug','isolation',check=True)
assert r.stdout.replace('\r','').splitlines()==['0',FLAGS]
pids={}
for name,args in [('kmsg-live.txt',['shell','dmesg','-w']),('logcat-live.txt',['logcat','-b','all','-v','threadtime'])]:
    with (LOG/name).open('wb') as f:
        p=subprocess.Popen(['timeout','1800']+A+args,stdout=f,stderr=subprocess.STDOUT,start_new_session=True)
        pids[name]=p.pid
(LOG/'observer-pids.json').write_text(json.dumps(pids,indent=2)+'\n')
deadline=time.monotonic()+300
while time.monotonic()<deadline:
    try:
        r=run('getprop sys.boot_completed; getprop init.svc.surfaceflinger; '
              'if [ -e /sys/class/remoteproc/remoteproc1/state ]; then cat /sys/class/remoteproc/remoteproc1/state; else echo absent; fi', 'boot-state')
        state=r.stdout.replace('\r','').splitlines()
        if state[:2]==['1','running']:break
    except subprocess.TimeoutExpired:pass
    time.sleep(2)
else:raise RuntimeError('Android boot incomplete')
assert state[2] in ['offline','absent'],'modem isolation invalid'
run(f'setprop debug.mesa.fd.mesa.debug {FLAGS}; setprop persist.graphics.egl mesa; '
    'setprop ctl.restart surfaceflinger', 'restart-gpu',check=True)
deadline=time.monotonic()+300
while time.monotonic()<deadline:
    try:
        r=run('dumpsys SurfaceFlinger','renderer',timeout=12)
        if 'FD512' in r.stdout:break
    except subprocess.TimeoutExpired:pass
    time.sleep(2)
else:raise RuntimeError('hardware renderer unconfirmed')
print('RENDERER_FD512_CONFIRMED',flush=True)
run('settings put global stay_on_while_plugged_in 7; input keyevent KEYCODE_WAKEUP; '
    'am force-stop com.android.settings','stay-awake',timeout=20)
deadline=time.monotonic()+240
while time.monotonic()<deadline:
    try:
        r=run('am start -a android.settings.DISPLAY_SETTINGS','display-start',timeout=15)
        if r.returncode==0 and 'Starting:' in r.stdout and 'Exception' not in r.stdout+r.stderr:break
    except subprocess.TimeoutExpired:pass
    time.sleep(3)
else:raise RuntimeError('Settings not ready')
run('cat /proc/sys/kernel/random/boot_id; getprop debug.mesa.fd.mesa.debug; '
    'getprop persist.vendor.a6l.radio; wm size; cat /proc/uptime', 'ready-state',check=True)
(LOG/'ready.json').write_text(json.dumps({'boot_id':boot,'radio':0,'flags':FLAGS,
    'renderer':'FD512','flashed':False,'result':'awaiting attended Display/clock comparison'},indent=2)+'\n')
print('GPU_UI_COMPARISON_READY',LOG,flush=True)
