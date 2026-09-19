"""Run the checked vendor allocator in V38 RAM. No flash or persistent mounts."""
from datetime import datetime,timezone
import hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parent
OUT=ROOT/'capture-allocator-user-v41/allocator'
PACKAGE=ROOT/'android-allocator-v41'
SERIAL='HLTE730T-PROBE'

def main():
    OUT.mkdir(mode=0o700,exist_ok=False)
    report={'started_utc':datetime.now(timezone.utc).isoformat(),'commands':[],'scope':'Real vendor minigbm allocator with guarded simpleDRM adapter; RAM-only allocation/metadata/native-handle import and pixel checks'}
    def save():(OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    def run(args,timeout=10,required=True):
        p=subprocess.run(args,capture_output=True,text=True,timeout=timeout)
        item={'argv':args,'exit':p.returncode,'stdout':p.stdout,'stderr':p.stderr};report['commands'].append(item);save()
        if required:assert p.returncode==0,item
        return item
    def shell(*args,**kw):return run(['adb','-s',SERIAL,'shell',*args],**kw)
    try:
        assert json.loads((PACKAGE/'qemu-report.json').read_text())['passed']
        files=json.loads((PACKAGE/'manifest.json').read_text())['files']
        for name,info in files.items():
            assert name=='a6l_simplefb.ko' or name.startswith(('bin/','lib64/'))
            assert '..' not in Path(name).parts
            assert hashlib.sha256((PACKAGE/'payload'/name).read_bytes()).hexdigest()==info['sha256'],name
        assert shell('getprop','ro.a6l.ramdiag')['stdout'].strip()=='v38'
        assert shell('getprop','ro.adb.secure')['stdout'].strip()=='1'
        assert 'uid=0(root)' in shell('id')['stdout']
        assert shell('uname','-r')['stdout'].strip()=='7.2.3-a6l-probe+'
        before=shell('cat','/proc/mounts')['stdout'];assert 'tmpfs /tmp tmpfs ' in before
        assert all(l.split()[2] in {'rootfs','tmpfs','devpts','proc','sysfs','selinuxfs','configfs','functionfs','debugfs'} for l in before.splitlines())
        assert shell('/system/bin/toybox','ls','-d','/tmp/a6l-v41',required=False)['exit']!=0
        cards=shell('/system/bin/toybox','ls','/sys/class/drm')['stdout'].split();assert not any(s.startswith('card') for s in cards),cards
        shell('/system/bin/toybox','mkdir','-m','0700','/tmp/a6l-v41')
        run(['adb','-s',SERIAL,'push',str(PACKAGE/'payload')+'/.','/tmp/a6l-v41/'],timeout=40)
        shell('/system/bin/toybox','chmod','0700','/tmp/a6l-v41/bin/a6l_gralloc_probe')
        hashes=shell('sha256sum',*['/tmp/a6l-v41/'+name for name in files],timeout=15)['stdout']
        actual={l.split()[1]:l.split()[0] for l in hashes.splitlines()}
        assert actual=={'/tmp/a6l-v41/'+n:i['sha256'] for n,i in files.items()}
        report['staged_files_verified']=len(files)
        shell('/system/bin/toybox','insmod','/tmp/a6l-v41/a6l_simplefb.ko',timeout=15)
        p=shell('/system/bin/toybox','env','LD_LIBRARY_PATH=/tmp/a6l-v41/lib64','/tmp/a6l-v41/bin/a6l_gralloc_probe',timeout=48,required=False)
        (OUT/'allocator.log').write_text(p['stdout']+p['stderr'])
        report['allocator_passed']=p['exit']==0 and 'A6L_GRALLOC_PASS formats=2 metadata=1 cross_process_import=1 pixel_coherence=1' in p['stdout'] and p['stdout'].count('A6L_GRALLOC_CHILD_PASS')==2
        after=shell('cat','/proc/mounts')['stdout'];report['global_mounts_unchanged']=before==after
        report['adb_alive']=shell('getprop','ro.a6l.ramdiag')['stdout'].strip()=='v38'
        d=run(['adb','-s',SERIAL,'exec-out','dmesg']);(OUT/'dmesg.txt').write_text(d['stdout'])
        report['passed']=report['allocator_passed'] and report['global_mounts_unchanged'] and report['adb_alive']
    except Exception as e:report['error']=repr(e);report['passed']=False
    finally:
        report['finished_utc']=datetime.now(timezone.utc).isoformat();save()
    print(json.dumps({k:v for k,v in report.items() if k!='commands'},indent=2));return 0 if report.get('passed') else 1
if __name__=='__main__':raise SystemExit(main())
