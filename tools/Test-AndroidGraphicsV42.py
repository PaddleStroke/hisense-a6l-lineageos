"""Build a RAM chroot containing unmodified Android graphics services; diskless QEMU."""
import argparse,gzip,hashlib,json,re,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];A=Path('/home/a6l/android/a6l-lineage24');P=A/'out/target/product/a6l'
ap=argparse.ArgumentParser();ap.add_argument('--attempt',type=int,required=True);n=ap.parse_args().attempt
OUT=Path(f'/home/a6l/kernel/graphics-v42-r{n}');OUT.mkdir(exist_ok=False)
ARCH=ROOT/f'firmware/extracted/android-graphics-v42-20260917-r{n}';ARCH.mkdir(exist_ok=False)
files={};payload=OUT/'payload';sha=lambda x:hashlib.sha256(x).hexdigest()
def put(name,src,mode=0o644):
    data=src.read_bytes();p=payload/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
    files[name]={'sha256':sha(data),'bytes':len(data),'mode':mode,'source':str(src)}
def content(name,data):
    p=payload/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(data)
    files[name]={'sha256':sha(p.read_bytes()),'bytes':p.stat().st_size,'mode':0o644,'source':'fixture configuration'}
put('bin/a6l_graphics_services',P/'system/bin/a6l_graphics_services',0o755)
put('a6l_simplefb.ko',ROOT/'firmware/extracted/android-display-v40-20260917-r1/a6l_simplefb.ko',0o400)
put('root/system/bin/linker64',P/'recovery/root/system/bin/linker64',0o755)
seeds=[('system','bin/servicemanager'),('vendor','bin/a6l_graphics_client'),
       ('vendor','bin/hw/android.hardware.graphics.allocator-service.minigbm'),
       ('vendor','bin/hw/android.hardware.composer.hwc3-service.drm'),('vendor','lib64/hw/mapper.minigbm.so')]
for group,path in seeds:
    name=f'root/{group}/{path}';put(name,P/group/path,0o755 if path.startswith('bin/') else 0o644)
    queue=[name];seen=set()
    while queue:
        current=queue.pop()
        if current in seen:continue
        seen.add(current)
        text=subprocess.check_output(['readelf','-l','-d',str(payload/current)],text=True)
        for interp in re.findall(r'Requesting program interpreter: (.*?)\]',text):
            assert interp in ['/system/bin/linker64','/system/bin/bootstrap/linker64'],interp
            dest='root'+interp
            if dest not in files:
                put(dest,P/interp.lstrip('/'),0o755)
        for lib in re.findall(r'\(NEEDED\).*\[(.*?)\]',text):
            name=f'root/{group}/lib64/{lib}'
            if name in files:continue
            candidates=[P/group/'lib64'/lib,P/'system/lib64'/lib,P/'system_ext/lib64'/lib,
                        P/'apex/com.android.runtime/lib64/bionic'/lib,P/'system/lib64/bootstrap'/lib]
            found=next((f for f in candidates if f.is_file()),None);assert found,(group,lib)
            put(name,found);queue.append(name)
put('root/system/etc/selinux/plat_service_contexts',P/'system/etc/selinux/plat_service_contexts')
manifest='''<manifest version="1.0" type="device">
  <hal format="aidl"><name>android.hardware.graphics.allocator</name><version>3</version><fqname>IAllocator/default</fqname></hal>
  <hal format="aidl"><name>android.hardware.graphics.composer3</name><version>5</version><fqname>IComposer/default</fqname></hal>
  <hal format="native"><name>mapper</name><version>5.0</version><interface><instance>minigbm</instance></interface></hal>
</manifest>
'''
content('root/vendor/etc/vintf/manifest.xml',manifest)
content('root/system/etc/vintf/manifest.xml','<manifest version="1.0" type="framework"/>\n')
for name,info in files.items():
    p=ARCH/'payload'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes((payload/name).read_bytes())
(ARCH/'manifest.json').write_text(json.dumps({'files':files,'scope':'Private RAM root, private Binder, real Android AIDL allocator/stable mapper/DRM composer; no storage device nodes or persistent mounts'},indent=2)+'\n')
for name in ['graphics_services.c','graphics_client.cpp']:
    src=ROOT/'device/hisense/a6l/diagnostic'/name;assert src.read_bytes()==(A/'device/hisense/a6l/diagnostic'/name).read_bytes();(ARCH/name).write_bytes(src.read_bytes())
for name in ['drv.c','dumb_driver.c','cros_gralloc/cros_gralloc_driver.cc']:
    src=A/'external/minigbm'/name
    dest=ARCH/'minigbm-source'/name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(src.read_bytes())
(ARCH/'minigbm.patch').write_bytes(subprocess.check_output(['git','diff'],cwd=A/'external/minigbm'))
for log in Path('/home/a6l/logs').glob('build-android-graphics-v42*.log'):(ARCH/log.name).write_bytes(log.read_bytes())
script=OUT/'qemu.sh';script.write_text('''#!/system/bin/sh
/system/bin/toybox mknod /dev/v42-console c 204 64
exec > /dev/v42-console 2>&1
echo A6L_QEMU_GRAPHICS_START
/system/bin/toybox mkdir -p /tmp/a6l-v42
/system/bin/toybox cp -R /v42/. /tmp/a6l-v42/
/system/bin/toybox insmod /tmp/a6l-v42/a6l_simplefb.ko
echo A6L_QEMU_GRAPHICS_MODULE_EXIT=$?
ready_before=$(/system/bin/getprop servicemanager.ready)
/tmp/a6l-v42/bin/a6l_graphics_services
echo A6L_QEMU_GRAPHICS_EXIT=$?
ready_after=$(/system/bin/getprop servicemanager.ready)
if [ "$ready_before" = "$ready_after" ]; then echo A6L_QEMU_READY_RESTORED=1; fi
for f in /tmp/a6l-v42/root/logs/*; do
 echo LOGFILE:$f
 /system/bin/toybox cat "$f"
done
/system/bin/toybox dmesg
echo A6L_QEMU_GRAPHICS_DONE
''')
rc=OUT/'init.rc';rc.write_text((ROOT/'device/hisense/a6l/diagnostic/android-init.rc').read_text()+'''
on early-init
    start graphicstest
service graphicstest /system/bin/sh /v42/qemu.sh
    user root
    group root
    disabled
    oneshot
    seclabel u:r:su:s0
''')
v38=ROOT/'firmware/extracted/android-ram-v38-20260917-r2'
lines=(v38/'ramdisk.list').read_text().splitlines()
lines=[f'file /system/etc/init/hw/init.rc {rc} 0644 0 0' if l.startswith('file /system/etc/init/hw/init.rc ') else l for l in lines]
dirs={'/v42'}
for name in files:dirs.update('/v42/'+str(p) for p in Path(name).parents if str(p)!='.')
lines += [f'dir {p} 0755 0 0' for p in sorted(dirs)]
lines += [f'file /v42/{name} {payload/name} {i["mode"]:04o} 0 0' for name,i in files.items()]
lines += [f'file /v42/qemu.sh {script} 0755 0 0']
recipe=OUT/'ramdisk.list';recipe.write_text('\n'.join(lines)+'\n')
data=subprocess.check_output(['/home/a6l/kernel/out-a6l-android-init/usr/gen_init_cpio','-t','1789344000',str(recipe)])
(OUT/'ramdisk.gz').write_bytes(gzip.compress(data,mtime=0))
dtb=ROOT/'firmware/extracted/android-display-v40-20260917-r1/positive/virt.dtb'
args=['qemu-system-aarch64','-machine','virt,gic-version=3','-cpu','cortex-a53','-smp','2','-m','2048','-nodefaults','-nographic','-monitor','none','-serial','stdio','-nic','none','-no-reboot','-dtb',str(dtb),'-kernel',str(ROOT/'firmware/extracted/android-init-kernel-20260917/Image'),'-initrd',str(OUT/'ramdisk.gz'),'-append','console=ttyAMA0,115200 loglevel=1 panic=0 androidboot.selinux=permissive androidboot.init_rc=/system/etc/init/hw/init.rc']
with (OUT/'console.log').open('wb') as f:
    proc=subprocess.Popen(args,stdout=f,stderr=subprocess.STDOUT)
    try:
        deadline=time.monotonic()+95
        while time.monotonic()<deadline:
            data=(OUT/'console.log').read_bytes()
            if b'A6L_QEMU_GRAPHICS_DONE' in data or b'Kernel panic' in data or proc.poll() is not None:break
            time.sleep(.3)
    finally:
        if proc.poll() is None:proc.kill();proc.wait(timeout=5)
data=(OUT/'console.log').read_bytes();checks={
 'module':b'A6L_QEMU_GRAPHICS_MODULE_EXIT=0' in data,'exit_zero':b'A6L_QEMU_GRAPHICS_EXIT=0' in data,
 'services':b'A6L_GRAPHICS_SERVICES_PASS client=1 services_alive=3 namespace_cleanup=1' in data,
 'allocator':b'A6L_GRAPHICS_ALLOCATOR_PASS binder=1 mapper5=1 metadata=1 pixels=2527200' in data,
 'composer':b'A6L_GRAPHICS_CLIENT_PASS allocator_aidl=1 mapper5=1 composer5=1 native_display=1' in data,
 'ready_property_restored':b'A6L_QEMU_READY_RESTORED=1' in data,
 'no_panic':b'Kernel panic' not in data}
report={'passed':all(checks.values()),'checks':checks,'command':args,'console_sha256':sha(data),'payload_files':len(files),'payload_bytes':sum(x['bytes'] for x in files.values())}
(ARCH/'qemu-report.json').write_text(json.dumps(report,indent=2)+'\n');(ARCH/'qemu-console.log').write_bytes(data)
print(json.dumps(report,indent=2));print(data[-18000:].decode(errors='replace'));assert report['passed']
