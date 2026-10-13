"""One explicitly selected, bounded V45 test stage; no firmware/block writes.

Run on the laptop only after authenticated V45 readiness, while user is present.
Each stage has an independent report and is one-shot. Failure leaves evidence;
it never automatically reboots or runs another stage.
"""
import argparse,hashlib,json,re,subprocess
from datetime import datetime,timezone
from pathlib import Path
ROOT=Path(__file__).resolve().parent
PACKAGE=ROOT/'controls-v45';SERIAL='HLTE730T-PROBE';REMOTE='/tmp/a6l-controls-v45'
VIRTUAL={'rootfs','tmpfs','proc','sysfs','devpts','selinuxfs','configfs','functionfs','debugfs'}
def main():
    ap=argparse.ArgumentParser();ap.add_argument('stage',choices=['baseline','surface','touch-load','touch-events','keys','brightness','vibration','battery']);args=ap.parse_args()
    output=ROOT/'capture-controls-user-v45/stages-followup'/args.stage;output.mkdir(parents=True,exist_ok=False)
    report={'stage':args.stage,'started':datetime.now(timezone.utc).isoformat(),'commands':[],'passed':False,'physical_confirmation_required':args.stage in ['brightness','vibration']}
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
        assert shell('/system/bin/toybox','cat','/proc/device-tree/chosen/hisense,a6l-controls')['stdout'].strip('\x00\r\n')=='v45'
        mounts=shell('/system/bin/toybox','cat','/proc/mounts')['stdout']
        assert all(len(l.split())>=3 and l.split()[2] in VIRTUAL for l in mounts.splitlines())
        assert 'tmpfs /tmp tmpfs ' in mounts
        shell('/system/bin/toybox','mkdir','-p','-m','0700',REMOTE)
        # Copy only the reviewed payload, never remote path text from a device.
        run(['adb','-s',SERIAL,'push',str(PACKAGE/'payload')+'/.',REMOTE+'/'],timeout=25)
        actual=shell('/system/bin/toybox','sha256sum',*[REMOTE+'/'+n for n in files])['stdout']
        assert {l.split()[1]:l.split()[0] for l in actual.splitlines()}=={REMOTE+'/'+n:i['sha256'] for n,i in files.items()}
        shell('/system/bin/toybox','chmod','0755',REMOTE+'/a6l_haptic_probe')
        snapshot('dmesg-before',['/system/bin/toybox','dmesg'])
        if args.stage=='baseline':
            snapshot('inputs',['/system/bin/toolbox','getevent','-lp'])
            snapshot('interrupts',['/system/bin/toybox','cat','/proc/interrupts'])
            snapshot('modules',['/system/bin/toybox','cat','/proc/modules'])
            snapshot('backlight',['/system/bin/toybox','ls','-l','/sys/class/backlight'])
        elif args.stage=='surface':
            run(['/usr/bin/python3',str(ROOT/'Run-AndroidSurfaceOnV45.py')],timeout=180)
            report['physical_confirmation_required']=True
        elif args.stage=='touch-load':
            load('edt-ft5x06.ko','edt_ft5x06')
            caps=snapshot('inputs',['/system/bin/toolbox','getevent','-lp'])
            from importlib.machinery import SourceFileLoader
            touch=SourceFileLoader('v45_touch',str(ROOT/'Capture-TouchEvents.py')).load_module()
            assert len(touch.front_devices(caps['stdout']))==1
        elif args.stage=='touch-events':
            print('Touch test: corners, swipe and two fingers; 15 seconds.',flush=True)
            run(['/usr/bin/python3',str(ROOT/'Capture-TouchEvents.py'),str(output/'touch'),'--seconds','15'],timeout=40)
        elif args.stage=='keys':
            caps=snapshot('inputs',['/system/bin/toolbox','getevent','-lp'])['stdout']
            assert all(s in caps for s in ['KEY_POWER','KEY_VOLUMEDOWN','KEY_VOLUMEUP'])
            print('Press each side button briefly once, including Power; do not hold it. 15 seconds.',flush=True)
            r=shell('/system/bin/toybox','timeout','15','/system/bin/toolbox','getevent','-t',timeout=25,required=False)
            assert r['exit'] in [0,124,143]
            (output/'events.txt').write_text(r['stdout'])
            codes={int(m[1],16) for m in re.finditer(r'\b0001\s+([0-9a-fA-F]{4})\s+00000001\b',r['stdout'])}
            report['key_down_codes']=sorted(codes);report['missing_codes']=sorted({114,115,116,616}-codes)
            assert not report['missing_codes'],report['missing_codes']
        elif args.stage=='brightness':
            print('Watch two low brightness levels; four seconds, then restoration.',flush=True)
            r=shell('/system/bin/sh',REMOTE+'/backlight_probe.sh','--test',timeout=12)
            assert 'A6L_BACKLIGHT_COMMANDS_PASS' in r['stdout']
        elif args.stage=='vibration':
            load('qcom-spmi-haptics.ko','qcom_spmi_haptics')
            caps=snapshot('inputs',['/system/bin/toolbox','getevent','-lp'])['stdout'];nodes=[]
            for block in re.split(r'(?=add device \d+:)',caps):
                m=re.search(r'add device \d+: (/dev/input/event\d+)',block)
                if m and '"spmi_haptics"' in block:nodes.append(m[1])
            assert len(nodes)==1,nodes
            print('One low-strength 100 ms vibration pulse now.',flush=True)
            r=shell(REMOTE+'/a6l_haptic_probe','--pulse',nodes[0],timeout=8)
            assert 'A6L_HAPTIC_COMMANDS_PASS physical_confirmation_required=1' in r['stdout']
        elif args.stage=='battery':
            load('pmi8998_fg.ko','pmi8998_fg')
            r=shell("for s in /sys/class/power_supply/*; do echo SUPPLY=$s; for n in type capacity voltage_now current_now temp charge_full_design; do if [ -r $s/$n ]; then echo FIELD=$n; /system/bin/toybox cat $s/$n; fi; done; done")
            (output/'telemetry.txt').write_text(r['stdout'])
            fields={m[1]:int(m[2]) for m in re.finditer(r'^FIELD=(\w+)\r?\n(-?\d+)\s*$',r['stdout'],re.M)}
            assert 3000000<=fields.get('voltage_now',0)<=4500000,fields
            assert 0<=fields.get('capacity',-1)<=100,fields
            assert 0<=fields.get('temp',-1)<=600,fields
            report.update(telemetry_collected=True,telemetry_fields=fields,charging_validated=False)
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

