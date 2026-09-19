"""Run a pinned V39 payload in V38's tmpfs over authenticated ADB. No flash."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parent
CAPTURE=ROOT/'capture-integration-user-v39'
PACKAGE=ROOT/'android-integration-v39'
SERIAL='HLTE730T-PROBE'
OUT=CAPTURE/'integration'

def main():
    assert CAPTURE.is_dir() and not OUT.exists()
    OUT.mkdir(mode=0o700)
    report={'started_utc':datetime.now(timezone.utc).isoformat(),'commands':[],
            'scope':'Reuse V38 recovery: private Binder test and fixed system/vendor ro,noload mounts; no flash or persistent writes'}
    def save(): (OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    def run(args,timeout=10,required=True):
        p=subprocess.run(args,capture_output=True,text=True,timeout=timeout)
        item={'argv':args,'exit':p.returncode,'stdout':p.stdout,'stderr':p.stderr}
        report['commands'].append(item);save()
        if required: assert p.returncode==0,item
        return item
    def shell(*args,**kw):return run(['adb','-s',SERIAL,'shell',*args],**kw)
    try:
        manifest=json.loads((PACKAGE/'manifest.json').read_text())
        assert json.loads((PACKAGE/'qemu-report.json').read_text())['passed']
        for name,info in manifest['files'].items():
            assert name.startswith(('bin/','lib64/')) and '..' not in Path(name).parts
            assert hashlib.sha256((PACKAGE/'payload'/name).read_bytes()).hexdigest()==info['sha256'],name
        assert shell('getprop','ro.a6l.ramdiag')['stdout'].strip()=='v38'
        assert 'uid=0(root)' in shell('id')['stdout']
        assert shell('readlink','/proc/1/exe')['stdout'].strip()=='/system/bin/init'
        assert shell('getprop','ro.adb.secure')['stdout'].strip()=='1'
        old=shell('cat','/proc/mounts')['stdout']
        assert all(line.split()[2] in {'rootfs','tmpfs','devpts','proc','sysfs','selinuxfs','configfs','functionfs','debugfs'} for line in old.splitlines())
        assert 'tmpfs /tmp tmpfs ' in old
        assert shell('/system/bin/toybox','ls','-ld','/tmp/a6l-v39',required=False)['exit']!=0,'Do not overwrite a previous trial'
        shell('/system/bin/toybox','mkdir','-m','0700','/tmp/a6l-v39')
        run(['adb','-s',SERIAL,'push',str(PACKAGE/'payload')+'/.','/tmp/a6l-v39/'],timeout=40)
        for name in manifest['files']:
            if name.startswith('bin/'):
                shell('/system/bin/toybox','chmod','0700','/tmp/a6l-v39/'+name)
        hashes=shell('/system/bin/toybox','sha256sum',*['/tmp/a6l-v39/'+n for n in manifest['files']],timeout=15)['stdout']
        actual={line.split()[1]:line.split()[0] for line in hashes.splitlines()}
        assert actual=={'/tmp/a6l-v39/'+n:v['sha256'] for n,v in manifest['files'].items()}
        report['staged_files_verified']=len(actual)
        # Capture both independent outcomes even when one fails.
        for mode in ['--binder','--filesystems']:
            p=shell('/tmp/a6l-v39/bin/a6l_integration_probe',mode,timeout=42,required=False)
            (OUT/(mode[2:]+'.log')).write_text(p['stdout']+p['stderr'])
            report[mode[2:]+'_passed']=(p['exit']==0 and
                ('A6L_BINDER_PASS' if mode=='--binder' else 'A6L_FILESYSTEMS_PASS') in p['stdout'])
        after=shell('cat','/proc/mounts')['stdout']
        report['global_mounts_unchanged']=after==old
        report['passed']=bool(report['binder_passed'] and report['filesystems_passed'] and report['global_mounts_unchanged'])
        d=run(['adb','-s',SERIAL,'exec-out','dmesg'],timeout=10)
        (OUT/'dmesg.txt').write_text(d['stdout'])
    except Exception as error:
        report['error']=repr(error);report['passed']=False
    finally:
        report['finished_utc']=datetime.now(timezone.utc).isoformat();save()
    print(json.dumps({k:v for k,v in report.items() if k!='commands'},indent=2))
    return 0 if report.get('passed') else 1

if __name__=='__main__':raise SystemExit(main())
