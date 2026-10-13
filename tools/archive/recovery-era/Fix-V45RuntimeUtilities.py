"""Repair missing RAM userspace utility; stage haptic input for attended test."""
import hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parent
def shell(cmd):
    p=subprocess.run(['adb','-s','HLTE730T-PROBE','shell',cmd],capture_output=True,text=True,timeout=15)
    assert p.returncode==0,(cmd,p.stdout,p.stderr)
    return p.stdout
assert shell('getprop ro.a6l.ramdiag; uname -r; getprop ro.adb.secure').splitlines()==['v38','7.2.3-a6l-probe+','1']
assert shell('cat /proc/device-tree/chosen/hisense,a6l-controls').strip('\0\r\n')=='v45'
mounts=shell('cat /proc/mounts')
assert all(l.split()[2] in {'rootfs','tmpfs','proc','sysfs','devpts','selinuxfs','configfs','functionfs','debugfs'} for l in mounts.splitlines())
assert shell('/system/bin/toybox printf UTILITY_OK')=='UTILITY_OK'
shell('if [ ! -e /system/bin/printf ]; then /system/bin/toybox ln -s toybox /system/bin/printf; fi')
assert shell('/system/bin/printf UTILITY_OK')=='UTILITY_OK'
pkg=ROOT/'controls-v45';manifest=json.loads((pkg/'manifest.json').read_text())['files']
item=manifest['qcom-spmi-haptics.ko'];name='/tmp/a6l-controls-v45/qcom-spmi-haptics.ko'
assert shell('/system/bin/toybox sha256sum '+name).split()[0]==item['sha256']
assert 'qcom_spmi_haptics ' not in shell('cat /proc/modules')
shell('/system/bin/toybox insmod '+name)
subprocess.run(['/usr/bin/python3',str(ROOT/'Create-V45InputNodes.py')],check=True)
print('RAM utility repaired; haptic driver loaded; no vibration issued')
