"""Read stock peripheral identities without activation or personal radio data."""
import json,subprocess,datetime
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'research/controls-radio-20260917';OUT.mkdir(exist_ok=False)
ssh=['C:/Windows/System32/OpenSSH/ssh.exe','-F',str(ROOT/'tools/a6l-laptop-ssh.conf'),'a6l-laptop']
def run(command):
    p=subprocess.run(ssh+[command],capture_output=True,text=True,timeout=25)
    return {'exit':p.returncode,'stdout':p.stdout,'stderr':p.stderr}
identity=run('adb -s 1e529013 shell getprop ro.build.fingerprint; adb -s 1e529013 shell getprop sys.boot_completed')
assert 'Hisense/HLTE730T/HLTE730T:9/' in identity['stdout'] and identity['stdout'].splitlines()[-1]=='1'
commands={
 'bluetooth-uart':"adb -s 1e529013 shell 'ls -l /sys/class/tty/ttyHS0/device; getprop qcom.bluetooth.soc; getprop vendor.qcom.bluetooth.soc; getprop vendor.bluetooth.soc'",
 'led-metadata':"adb -s 1e529013 shell 'for s in /sys/class/leds/*; do echo NODE=$s; for n in brightness max_brightness; do if [ -r $s/$n ]; then echo FIELD=$n; cat $s/$n; fi; done; done'",
 'input-capabilities':'adb -s 1e529013 shell getevent -pl',
 'audio-card':"adb -s 1e529013 shell 'for s in /sys/class/sound/card*/id; do echo NODE=$s; cat $s; done'",
 'stock-gnss-services':"adb -s 1e529013 shell 'getprop init.svc.vendor.gnss_service; getprop init.svc.gnss_service; getprop ro.hardware'",
}
results={'identity':identity}
for name,command in commands.items():
    results[name]=run(command)
    (OUT/(name+'.txt')).write_text(results[name]['stdout']+results[name]['stderr'])
(OUT/'report.json').write_text(json.dumps({'time':datetime.datetime.now(datetime.timezone.utc).isoformat(),'read_only':True,'results':results},indent=2)+'\n')
print('CONTROL_BASELINE_SAVED',[(n,r['exit']) for n,r in results.items()])
