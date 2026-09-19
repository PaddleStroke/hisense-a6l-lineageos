"""Run the QEMU-checked graphics services inside V38 RAM; no firmware writes."""
from datetime import datetime,timezone
import hashlib,json,subprocess,re
from pathlib import Path
ROOT=Path(__file__).resolve().parent
OUT=ROOT/'capture-controls-user-v45/surface';PACKAGE=ROOT/'android-surface-v44';SERIAL='HLTE730T-PROBE'
def main():
    OUT.mkdir(mode=0o700,exist_ok=False)
    report={'started_utc':datetime.now(timezone.utc).isoformat(),'commands':[],'scope':'Private RAM root and Binder context; real AIDL allocator, mapper5, DRM composer; no persistent mounts or firmware writes'}
    printk=None
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
            assert not Path(name).is_absolute() and '..' not in Path(name).parts
            assert name=='a6l_simplefb.ko' or name.startswith(('bin/','root/'))
            assert hashlib.sha256((PACKAGE/'payload'/name).read_bytes()).hexdigest()==info['sha256'],name
        assert shell('/system/bin/toybox','cat','/proc/device-tree/chosen/hisense,a6l-controls')['stdout'].strip('\x00\r\n')=='v45'
        assert shell('getprop','ro.a6l.ramdiag')['stdout'].strip()=='v38'
        assert shell('getprop','ro.adb.secure')['stdout'].strip()=='1'
        assert 'uid=0(root)' in shell('id')['stdout']
        assert shell('uname','-r')['stdout'].strip()=='7.2.3-a6l-probe+'
        before=shell('cat','/proc/mounts')['stdout'];assert 'tmpfs /tmp tmpfs ' in before
        ready_before=shell('getprop','servicemanager.ready')['stdout']
        assert all(l.split()[2] in {'rootfs','tmpfs','devpts','proc','sysfs','selinuxfs','configfs','functionfs','debugfs'} for l in before.splitlines())
        assert shell('/system/bin/toybox','ls','-d','/tmp/a6l-v44',required=False)['exit']!=0
        cards=shell('/system/bin/toybox','ls','/sys/class/drm')['stdout'].split();assert not any(s.startswith('card') for s in cards),cards
        shell('/system/bin/toybox','mkdir','-m','0700','/tmp/a6l-v44')
        run(['adb','-s',SERIAL,'push',str(PACKAGE/'payload')+'/.','/tmp/a6l-v44/'],timeout=60)
        executables=['/tmp/a6l-v44/'+name for name,info in files.items() if info['mode']&0o111]
        shell('/system/bin/toybox','chmod','0755',*executables)
        hashes=shell('sha256sum',*['/tmp/a6l-v44/'+name for name in files],timeout=20)['stdout']
        actual={l.split()[1]:l.split()[0] for l in hashes.splitlines()}
        assert actual=={'/tmp/a6l-v44/'+n:i['sha256'] for n,i in files.items()}
        report['staged_files_verified']=len(files)
        printk=shell('cat','/proc/sys/kernel/printk')['stdout'].strip()
        assert len(printk.split())==4 and all(v.isdigit() for v in printk.split())
        shell('sh','-c',"'echo 1 > /proc/sys/kernel/printk'")
        shell('/system/bin/toybox','insmod','/tmp/a6l-v44/a6l_simplefb.ko',timeout=15)
        p=shell('/tmp/a6l-v44/bin/a6l_graphics_services',timeout=115,required=False)
        (OUT/'supervisor.log').write_text(p['stdout']+p['stderr'])
        run(['adb','-s',SERIAL,'pull','/tmp/a6l-v44/root/logs',str(OUT/'logs')],timeout=15,required=False)
        client=(OUT/'logs/client.log').read_text(errors='replace') if (OUT/'logs/client.log').exists() else ''
        report['graphics_passed']=p['exit']==0 and 'A6L_SURFACE_SERVICES_PASS client=1 services_alive=5 private_properties=1 namespace_cleanup=1' in p['stdout'] and 'A6L_SURFACE_PASS real_surfaceflinger=1 layers=4 transaction=checked' in client
        report['presentation_passed']='A6L_SURFACE_PASS real_surfaceflinger=1 layers=4 transaction=checked' in client
        dump=(OUT/'logs/surfaceflinger.dump').read_text(errors='replace') if (OUT/'logs/surfaceflinger.dump').exists() else ''
        report['software_vulkan']='GLES: ' in dump and 'ANGLE' in dump and 'SwiftShader' in dump
        report['client_composition']='usesClientComposition=true' in dump
        report['no_composer_commit_failures']='Failed to commit frames: 0' in dump and not re.search(r'Failed to (?:test )?commit frames: [1-9]',dump)
        after=shell('cat','/proc/mounts')['stdout'];report['global_mounts_unchanged']=before==after
        report['readiness_property_restored']=ready_before==shell('getprop','servicemanager.ready')['stdout']
        report['adb_alive']=shell('getprop','ro.a6l.ramdiag')['stdout'].strip()=='v38'
        d=run(['adb','-s',SERIAL,'exec-out','dmesg']);(OUT/'dmesg.txt').write_text(d['stdout'])
        report['passed']=report['client_composition'] and report['no_composer_commit_failures'] and report['software_vulkan'] and report['presentation_passed'] and report['graphics_passed'] and report['global_mounts_unchanged'] and report['readiness_property_restored'] and report['adb_alive']
    except Exception as e:report['error']=repr(e);report['passed']=False
    finally:
        if printk is not None:
            try:
                shell('sh','-c',"'echo "+printk+" > /proc/sys/kernel/printk'")
                report['console_loglevel_restored']=shell('cat','/proc/sys/kernel/printk')['stdout'].split()==printk.split()
                report['passed']=report.get('passed',False) and report['console_loglevel_restored']
            except Exception as e:report['cleanup_error']=repr(e);report['passed']=False
        report['finished_utc']=datetime.now(timezone.utc).isoformat();save()
    print(json.dumps({k:v for k,v in report.items() if k!='commands'},indent=2));return 0 if report.get('passed') else 1
if __name__=='__main__':raise SystemExit(main())
