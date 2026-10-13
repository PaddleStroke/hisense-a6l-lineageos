"""Build an explicit RAM-only Android recovery userspace; no host/device mounts."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
A = Path('/home/a6l/android/a6l-lineage24')
PRODUCT = A / 'out/target/product/a6l'
KERNEL = ROOT / 'firmware/extracted/android-init-kernel-20260917'

def sha(b): return hashlib.sha256(b).hexdigest()

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--attempt',type=int,default=1)
    n=ap.parse_args().attempt
    log=Path('/home/a6l/logs/build-android-ram-userspace.log').read_bytes()
    assert b'#### build completed successfully' in log[-16384:]
    out=ROOT / f'firmware/extracted/android-ram-v38-20260917-r{n}'
    out.mkdir(exist_ok=False)
    tree=Path(f'/home/a6l/kernel/android-ram-v38-r{n}')
    tree.mkdir(exist_ok=False)
    files={}
    def put(dest,data,mode=0o644):
        assert dest.startswith('/') and '..' not in Path(dest).parts and dest not in files
        p=tree / dest.lstrip('/')
        p.parent.mkdir(parents=True,exist_ok=True)
        p.write_bytes(data)
        files[dest]=dict(sha256=sha(data),bytes=len(data),mode=mode)
    recovery=PRODUCT/'recovery/root'
    for name in ['init','adbd','sh','toybox','toolbox']:
        p=recovery/'system/bin'/name
        put('/system/bin/'+name,p.read_bytes(),0o755)
    # Close ELF dependency graphs only with matching recovery variants.
    queue=list(files)
    done=set()
    while queue:
        dest=queue.pop()
        if dest in done: continue
        done.add(dest)
        p=tree/dest.lstrip('/')
        text=subprocess.check_output(['readelf','-l','-d',str(p)],text=True)
        targets=['/system/lib64/'+x for x in re.findall(r'\(NEEDED\).*\[(.*?)\]',text)]
        targets += re.findall(r'Requesting program interpreter: (.*?)\]',text)
        for target in targets:
            if target in files: continue
            original=recovery/target.lstrip('/')
            assert original.is_file(), f'Missing matching recovery dependency: {target}'
            put(target,original.read_bytes(),0o755 if '/bin/' in target else 0o644)
            queue.append(target)
    first=A/'out/soong/.intermediates/system/core/init/init_first_stage/android_ramdisk_arm64_armv8-a/init'
    put('/init',first.read_bytes(),0o755)
    put('/system/bin/recovery',b'') # Marker only, never executed.
    put('/system/bin/a6l_android_probe',(PRODUCT/'system/bin/a6l_android_probe').read_bytes(),0o755)
    put('/sdhci-msm.ko',(KERNEL/'sdhci-msm.ko').read_bytes(),0o400)
    put('/sepolicy',(recovery/'sepolicy').read_bytes(),0o644)
    put('/system/etc/init/hw/init.rc',(ROOT/'device/hisense/a6l/diagnostic/android-init.rc').read_bytes())
    for name in ['plat_file_contexts','plat_property_contexts','plat_service_contexts','plat_seapp_contexts']:
        put('/system/etc/selinux/'+name,(PRODUCT/'system/etc/selinux'/name).read_bytes())
    for name in ['cgroups.json','task_profiles.json']:
        p=PRODUCT/'system/etc'/name
        if p.exists(): put('/system/etc/'+name,p.read_bytes())
    key=(ROOT/'firmware/extracted/laptop-adb-public-key.txt').read_bytes()
    assert sha(key)=='09b76c923da9454ae04127bb4e0fa9695777da379217943d44bf99df631051c7'
    put('/adb_keys',key,0o640)
    put('/prop.default',b'ro.a6l.ramdiag=v38\nro.debuggable=1\nro.secure=1\nro.adb.secure=1\nro.adb.secure.recovery=1\nservice.adb.root=1\nro.product.cpu.abilist=arm64-v8a\nro.product.cpu.abilist64=arm64-v8a\nro.build.version.sdk=37\nro.product.first_api_level=37\nro.treble.enabled=false\n')
    put('/etc/recovery.fstab',b'# Deliberately no persistent mounts in the RAM diagnostic.\n')
    dirs={'/dev','/proc','/sys','/mnt','/debug_ramdisk','/second_stage_resources','/tmp','/data','/apex','/linkerconfig','/acct','/system_ext','/product','/vendor','/odm'}
    for name in files:
        dirs.update(str(p) for p in Path(name).parents if str(p)!='/')
    listing=''.join(f'dir {d} 0755 0 0\n' for d in sorted(dirs,key=lambda d:(d.count('/'),d)))
    listing+=''.join(f'file {d} {tree/d.lstrip("/")} {v["mode"]:04o} 0 0\n' for d,v in sorted(files.items()))
    listing+='slink /bin /system/bin 0755 0 0\n'
    # Only non-destructive convenience applets, not format/flash tools.
    for name in ['cat','ls','id','uname','ps','dmesg','sha256sum','readlink','stat','sleep']:
        listing+=f'slink /system/bin/{name} /system/bin/toybox 0755 0 0\n'
    listing+='slink /system/bin/getprop /system/bin/toolbox 0755 0 0\n'
    (out/'ramdisk.list').write_text(listing)
    cpio=subprocess.check_output(['/home/a6l/kernel/out-a6l-android-init/usr/gen_init_cpio','-t','1789344000',str(out/'ramdisk.list')])
    compressed=gzip.compress(cpio,mtime=0)
    (out/'ramdisk.cpio.gz').write_bytes(compressed)
    (out/'ramdisk-listing.txt').write_bytes(subprocess.check_output(['cpio','-itv'],input=cpio))
    report=dict(packaging_passed=True,files=files,ramdisk_sha256=sha(compressed),ramdisk_bytes=len(compressed),
                scope='Actual Android init/recovery policy/adbd; RAM filesystem only; serial helper and fixed direct read verifier; no persistent mounts')
    report['sources']={}
    for name in ['android_service.c','storage_read.c','storage_read_ranges.h']:
        source=(ROOT/'device/hisense/a6l/diagnostic'/name).read_bytes()
        assert source==(A/'device/hisense/a6l/diagnostic'/name).read_bytes()
        report['sources'][name]=sha(source)
        (out/name).write_bytes(source)
    (out/'build.log').write_bytes(log)
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'out':str(out),'bytes':len(compressed),'sha256':sha(compressed),'files':len(files)}))

if __name__=='__main__': main()
