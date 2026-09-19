"""One explicitly selected, bounded V46 key/haptic stage; no firmware/block writes.

Run on the laptop only after authenticated V46 readiness, while user is present.
Each stage has an independent report and is one-shot. Failure leaves evidence;
it never automatically reboots or runs another stage.
"""
import argparse,hashlib,json,re,subprocess
from datetime import datetime,timezone
from pathlib import Path
ROOT=Path(__file__).resolve().parent
PACKAGE=ROOT/'controls-v46';SERIAL='HLTE730T-PROBE';REMOTE='/tmp/a6l-controls-v46'
VIRTUAL={'rootfs','tmpfs','proc','sysfs','devpts','selinuxfs','configfs','functionfs','debugfs'}
def main():
    ap=argparse.ArgumentParser();ap.add_argument('stage',choices=['baseline','keys','vibration']);args=ap.parse_args()
    output=ROOT/'capture-controls-user-v46/stages'/args.stage;output.mkdir(parents=True,exist_ok=False)
    report={'stage':args.stage,'started':datetime.now(timezone.utc).isoformat(),'commands':[],'passed':False,'physical_confirmation_required':args.stage == 'vibration'}
    def save():(output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    def run(argv,timeout=12,required=True):
        p=subprocess.run(argv,capture_output=True,text=True,timeout=timeout)
        item={'argv':argv,'exit':p.returncode,'stdout':p.stdout,'stderr':p.stderr};report['commands'].append(item);save()
        if required and p.returncode:raise RuntimeError(item)
        return item
    def shell(*cmd,**kw):return run(['adb','-s',SERIAL,'shell',*cmd],**kw)
    def snapshot(name,cmd):
        r=shell(*cmd,required=False);(output/(name+'.txt')).write_text(r['stdout']+r['stderr']);return r
    def load(filename,module):
        names=shell('/system/bin/toybox','cat','/proc/modules')['stdout']
        if not re.search(r'^'+re.escape(module)+r' ',names,re.M):
            shell('/system/bin/toybox','insmod',REMOTE+'/'+filename,timeout=15)
        names=shell('/system/bin/toybox','cat','/proc/modules')['stdout']
        assert re.search(r'^'+re.escape(module)+r' ',names,re.M)
    try:
        files=json.loads((PACKAGE/'manifest.json').read_text())['files']
        for n,item in files.items():
            assert re.fullmatch(r'[a-zA-Z0-9_.-]+',n)
            assert hashlib.sha256((PACKAGE/'payload'/n).read_bytes()).hexdigest()==item['sha256']
        check=shell('getprop ro.a6l.ramdiag; uname -r; getprop ro.adb.secure; id')['stdout'].splitlines()
        assert check[:3]==['v38','7.2.3-a6l-probe+','1'] and 'uid=0(root)' in check[3]
        assert shell('/system/bin/toybox','cat','/proc/device-tree/chosen/hisense,a6l-controls')['stdout'].strip('\x00\r\n')=='v46'
        mounts=shell('/system/bin/toybox','cat','/proc/mounts')['stdout']
        assert all(len(l.split())>=3 and l.split()[2] in VIRTUAL for l in mounts.splitlines())
        assert 'tmpfs /tmp tmpfs ' in mounts
        shell('/system/bin/toybox','mkdir','-p','-m','0700',REMOTE)
        # Copy only the reviewed payload, never remote path text from a device.
        run(['adb','-s',SERIAL,'push',str(PACKAGE/'payload')+'/.',REMOTE+'/'],timeout=25)
        actual=shell('/system/bin/toybox','sha256sum',*[REMOTE+'/'+n for n in files])['stdout']
        assert {l.split()[1]:l.split()[0] for l in actual.splitlines()}=={REMOTE+'/'+n:i['sha256'] for n,i in files.items()}
        shell('/system/bin/toybox','chmod','0755',REMOTE+'/a6l_haptic_probe')
        run(['/usr/bin/python3',str(ROOT/'Create-V46InputNodes.py')])
        snapshot('dmesg-before',['/system/bin/toybox','dmesg'])
        snapshot('pm660-pinconf',["for p in /sys/kernel/debug/pinctrl/*800f000.spmi:pmic@0:gpio@c000*/pinconf-groups; do if [ -r \"$p\" ]; then /system/bin/toybox cat \"$p\"; fi; done"])
        if args.stage=='baseline':
            snapshot('inputs',['/system/bin/toolbox','getevent','-lp'])
            snapshot('interrupts',['/system/bin/toybox','cat','/proc/interrupts'])
            snapshot('modules',['/system/bin/toybox','cat','/proc/modules'])
            snapshot('backlight',['/system/bin/toybox','ls','-l','/sys/class/backlight'])
        elif args.stage=='keys':
            caps=snapshot('inputs',['/system/bin/toolbox','getevent','-lp'])['stdout']
            blocks=re.split(r'(?=add device \d+:)',caps)
            nodes=[]
            for block in blocks:
                match=re.search(r'add device \d+: (/dev/input/event\d+)',block)
                if match and '"A6L side keys"' in block:nodes.append(match[1])
            assert len(nodes)==1,nodes
            snapshot('interrupts-before',['/system/bin/toybox','cat','/proc/interrupts'])
            print('KEY_CAPTURE_ARMED: briefly press only the e-ink side key repeatedly for 20 seconds.',flush=True)
            r=run(['adb','-s',SERIAL,'shell','-tt','/system/bin/toybox','timeout','20','/system/bin/toolbox','getevent','-t',nodes[0]],timeout=30,required=False)
            assert r['exit'] in [0,124,143]
            (output/'events.txt').write_text(r['stdout'])
            downs=len(re.findall(r'\b0001\s+0268\s+00000001\b',r['stdout']))
            ups=len(re.findall(r'\b0001\s+0268\s+00000000\b',r['stdout']))
            report.update(key_down_count=downs,key_up_count=ups,physical_key_passed=downs>0 and ups>0)
            snapshot('interrupts-after',['/system/bin/toybox','cat','/proc/interrupts'])
            # An observed zero is a completed diagnostic, not a collector failure.
            # Keep the hardware result separate from successful capture/ADB.
        elif args.stage=='vibration':
            assert not re.search(r'^qcom_spmi_haptics ',shell('/system/bin/toybox','cat','/proc/modules')['stdout'],re.M),'Unexpected preloaded haptic module'
            load('qcom-spmi-haptics.ko','qcom_spmi_haptics')
            run(['/usr/bin/python3',str(ROOT/'Create-V46InputNodes.py')])
            caps=snapshot('inputs',['/system/bin/toolbox','getevent','-lp'])['stdout'];nodes=[]
            for block in re.split(r'(?=add device \d+:)',caps):
                m=re.search(r'add device \d+: (/dev/input/event\d+)',block)
                if m and '"spmi_haptics"' in block:nodes.append(m[1])
            assert len(nodes)==1,nodes
            print('One low-strength 100 ms vibration pulse now.',flush=True)
            r=shell(REMOTE+'/a6l_haptic_probe','--pulse',nodes[0],timeout=8)
            assert 'A6L_HAPTIC_COMMANDS_PASS physical_confirmation_required=1' in r['stdout']
        report['passed']=True
    except Exception as e:report['error']=repr(e)
    finally:
        try:
            snapshot('dmesg-after',['/system/bin/toybox','dmesg'])
            report['adb_alive']=shell('getprop','ro.a6l.ramdiag')['stdout'].strip()=='v38'
            report['passed']=report['passed'] and report['adb_alive']
        except Exception as e:report['cleanup_error']=repr(e);report['passed']=False
        report['finished']=datetime.now(timezone.utc).isoformat();save()
    print(json.dumps({k:v for k,v in report.items() if k!='commands'},indent=2));return 0 if report['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
