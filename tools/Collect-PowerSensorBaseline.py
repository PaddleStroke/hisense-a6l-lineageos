"""Read stock sensor identities and battery/thermal metadata; never configure hardware."""
import datetime,json,re,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'research/power-sensors-20260917';OUT.mkdir(parents=True,exist_ok=False)
ssh=['C:/Windows/System32/OpenSSH/ssh.exe','-F',str(ROOT/'tools/a6l-laptop-ssh.conf'),'a6l-laptop']
def run(command):
    p=subprocess.run(ssh+[command],capture_output=True,text=True,timeout=25)
    return {'exit':p.returncode,'stdout':p.stdout,'stderr':p.stderr}
identity=run('adb -s 1e529013 shell getprop ro.build.fingerprint; adb -s 1e529013 shell getprop sys.boot_completed')
assert 'Hisense/HLTE730T/HLTE730T:9/' in identity['stdout'] and identity['stdout'].splitlines()[-1]=='1'
sensor=run('adb -s 1e529013 shell dumpsys sensorservice')
names=[line for line in sensor['stdout'].splitlines() if re.match(r'^0x[0-9a-fA-F]+\).*\|.*type:',line)]
assert len(names)>0
# Only retain identity metadata, not client lists, app names, event history or step counts.
(OUT/'stock-sensor-list.txt').write_text('\n'.join(names)+'\n')
commands={
 'battery-service':'adb -s 1e529013 shell dumpsys battery',
 'power-supplies':"adb -s 1e529013 shell 'for s in /sys/class/power_supply/*; do echo SUPPLY=$s; for n in type status health present capacity voltage_now current_now temp charge_full_design voltage_max constant_charge_current_max input_current_limit; do if [ -r $s/$n ]; then echo FIELD=$n; cat $s/$n; fi; done; done'",
 'thermal-zones':"adb -s 1e529013 shell 'for s in /sys/class/thermal/thermal_zone*; do echo ZONE=$s; for n in type temp mode; do if [ -r $s/$n ]; then echo FIELD=$n; cat $s/$n; fi; done; done'",
 'i2c-names':"adb -s 1e529013 shell 'for n in /sys/bus/i2c/devices/*/name; do echo NODE=$n; cat $n; done'",
}
results={'identity':identity}
for name,command in commands.items():
    results[name]=run(command)
    (OUT/(name+'.txt')).write_text(results[name]['stdout']+results[name]['stderr'])
(OUT/'report.json').write_text(json.dumps({'time':datetime.datetime.now(datetime.timezone.utc).isoformat(),'read_only':True,'sensor_entries':len(names),'results':results},indent=2)+'\n')
print('STOCK_BASELINE_CAPTURED',len(names),'sensor entries')
print('\n'.join(names[-8:]))
