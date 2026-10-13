"""Run the checked V40 LCD test in V38 RAM over authenticated ADB, no flash."""
from datetime import datetime,timezone
import hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parent
OUT=ROOT/'capture-display-user-v40/display'
PACKAGE=ROOT/'android-display-v40'
SERIAL='HLTE730T-PROBE'

def main():
    OUT.mkdir(mode=0o700,exist_ok=False)
    report={'started_utc':datetime.now(timezone.utc).isoformat(),'commands':[],'scope':'Guarded reserved LCD framebuffer, RAM-loaded platform adapter and KMS/PRIME probe; no firmware or filesystem writes'}
    printk=None
    def save():(OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    def run(args,timeout=10,required=True):
        p=subprocess.run(args,capture_output=True,text=True,timeout=timeout)
        item={'argv':args,'exit':p.returncode,'stdout':p.stdout,'stderr':p.stderr};report['commands'].append(item);save()
        if required:assert p.returncode==0,item
        return item
    def shell(*args,**kw):return run(['adb','-s',SERIAL,'shell',*args],**kw)
    try:
        package=json.loads((PACKAGE/'report.json').read_text());assert package['passed']
        assert set(package['files'])=={'a6l_simplefb.ko','a6l_drm_probe'}
        for name,info in package['files'].items():assert hashlib.sha256((PACKAGE/name).read_bytes()).hexdigest()==info['sha256'],name
        assert shell('getprop','ro.a6l.ramdiag')['stdout'].strip()=='v38'
        assert shell('getprop','ro.adb.secure')['stdout'].strip()=='1'
        assert 'uid=0(root)' in shell('id')['stdout']
        assert shell('uname','-r')['stdout'].strip()=='7.2.3-a6l-probe+'
        before=shell('cat','/proc/mounts')['stdout'];assert 'tmpfs /tmp tmpfs ' in before
        assert all(l.split()[2] in {'rootfs','tmpfs','devpts','proc','sysfs','selinuxfs','configfs','functionfs','debugfs'} for l in before.splitlines())
        assert shell('/system/bin/toybox','ls','-d','/tmp/a6l-v40',required=False)['exit']!=0
        cards=shell('/system/bin/toybox','ls','/sys/class/drm')['stdout'].split();assert not any(s.startswith('card') for s in cards),cards
        shell('/system/bin/toybox','mkdir','-m','0700','/tmp/a6l-v40')
        for name in package['files']:
            run(['adb','-s',SERIAL,'push',str(PACKAGE/name),'/tmp/a6l-v40/'+name],timeout=20)
            shell('/system/bin/toybox','chmod','0700','/tmp/a6l-v40/'+name)
            assert shell('sha256sum','/tmp/a6l-v40/'+name)['stdout'].split()[0]==package['files'][name]['sha256']
        report['staged_files_verified']=2
        printk=shell('cat','/proc/sys/kernel/printk')['stdout'].strip()
        assert len(printk.split())==4 and all(v.isdigit() for v in printk.split())
        # Hide console text during the visual check; kernel logs remain readable through ADB.
        shell('sh','-c',"'echo 1 > /proc/sys/kernel/printk'")
        shell('/system/bin/toybox','insmod','/tmp/a6l-v40/a6l_simplefb.ko',timeout=15)
        p=shell('/tmp/a6l-v40/a6l_drm_probe',timeout=48,required=False)
        (OUT/'display.log').write_text(p['stdout']+p['stderr'])
        report['display_passed']=p['exit']==0 and 'A6L_DRM_PASS atomic=1 buffers=2 updates=8 prime_export=1 prime_import=1' in p['stdout'] and 'A6L_DRM_CLEANUP_PASS' in p['stdout']
        after=shell('cat','/proc/mounts')['stdout'];report['global_mounts_unchanged']=before==after
        report['adb_alive']=shell('getprop','ro.a6l.ramdiag')['stdout'].strip()=='v38'
        d=run(['adb','-s',SERIAL,'exec-out','dmesg']);(OUT/'dmesg.txt').write_text(d['stdout'])
        report['passed']=report['display_passed'] and report['global_mounts_unchanged'] and report['adb_alive']
    except Exception as e:report['error']=repr(e);report['passed']=False
    finally:
        if printk is not None:
            try:
                shell('sh','-c',"'echo "+printk+" > /proc/sys/kernel/printk'")
                report['console_loglevel_restored']=shell('cat','/proc/sys/kernel/printk')['stdout'].split()==printk.split()
            except Exception as e:report['cleanup_error']=repr(e);report['passed']=False
        report['finished_utc']=datetime.now(timezone.utc).isoformat();save()
    print(json.dumps({k:v for k,v in report.items() if k!='commands'},indent=2));return 0 if report.get('passed') else 1
if __name__=='__main__':raise SystemExit(main())
