"""Expose registered input character devices in diagnostic /dev tmpfs only."""
import json,re,subprocess
from pathlib import Path
root=Path(__file__).resolve().parent
def shell(command):
    p=subprocess.run(['adb','-s','HLTE730T-PROBE','shell',command],capture_output=True,text=True,timeout=10)
    assert p.returncode==0,(command,p.stderr)
    return p.stdout
assert shell('getprop ro.a6l.ramdiag; uname -r; getprop ro.adb.secure').splitlines()==['v38','7.2.3-a6l-probe+','1']
assert shell('cat /proc/device-tree/chosen/hisense,a6l-controls').strip('\0\r\n')=='v45'
mounts=shell('cat /proc/mounts')
assert 'tmpfs /dev tmpfs ' in mounts and 'tmpfs /tmp tmpfs ' in mounts
assert all(l.split()[2] in {'rootfs','tmpfs','proc','sysfs','devpts','selinuxfs','configfs','functionfs','debugfs'} for l in mounts.splitlines())
events=shell('for e in /sys/class/input/event*; do echo NODE=$e; cat $e/dev; cat $e/device/name; done')
rows=re.findall(r'NODE=/sys/class/input/(event\d+)\r?\n(\d+):(\d+)\r?\n([^\r\n]+)',events)
assert rows
allowed={'pm8941_pwrkey','pm8941_resin','A6L side keys','generic ft5x06 (8d)','spmi_haptics'}
assert all(int(major)==13 and 64<=int(minor)<96 and name in allowed for node,major,minor,name in rows)
shell('/system/bin/toybox mkdir -p -m 0700 /dev/input')
for node,major,minor,name in rows:
    path='/dev/input/'+node
    shell('if [ ! -e '+path+' ]; then /system/bin/toybox mknod -m 0600 '+path+' c '+major+' '+minor+'; fi')
caps=shell('/system/bin/toolbox getevent -lp')
out=root/'capture-controls-user-v45/input-node-repair';out.mkdir(exist_ok=True)
(out/'capabilities.txt').write_text(caps)
(out/'nodes.json').write_text(json.dumps(rows,indent=2))
print(caps)
