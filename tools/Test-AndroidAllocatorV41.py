"""Package the actual Android vendor allocator plus a diskless QEMU test."""
import argparse,gzip,hashlib,json,re,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
A=Path('/home/a6l/android/a6l-lineage24');PRODUCT=A/'out/target/product/a6l'
BUILD=Path('/home/a6l/kernel/out-a6l-android-init')
ap=argparse.ArgumentParser();ap.add_argument('--attempt',type=int,required=True);n=ap.parse_args().attempt
OUT=Path(f'/home/a6l/kernel/allocator-v41-r{n}');OUT.mkdir(exist_ok=False)
ARCH=ROOT/f'firmware/extracted/android-allocator-v41-20260917-r{n}';ARCH.mkdir(exist_ok=False)
def sha(b):return hashlib.sha256(b).hexdigest()
def run(*a):return subprocess.check_output([str(x) for x in a],stderr=subprocess.STDOUT)
files={}
def put(name,source,mode):
    data=source.read_bytes();dest=OUT/'payload'/name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
    files[name]={'sha256':sha(data),'bytes':len(data),'mode':mode,'source':str(source)}
put('bin/a6l_gralloc_probe',PRODUCT/'vendor/bin/a6l_gralloc_probe',0o755)
queue=list(files);done=set()
while queue:
    name=queue.pop()
    if name in done:continue
    done.add(name);elf=run('readelf','-l','-d',OUT/'payload'/name).decode()
    for lib in re.findall(r'\(NEEDED\).*\[(.*?)\]',elf):
        dest='lib64/'+lib
        if dest not in files:
            candidates=[PRODUCT/'vendor'/dest,PRODUCT/'system'/dest,PRODUCT/'recovery/root/system'/dest,
                        PRODUCT/'apex/com.android.runtime/lib64/bionic'/lib,
                        PRODUCT/'system/lib64/bootstrap'/lib]
            found=next((p for p in candidates if p.is_file()),None);assert found,lib
            put(dest,found,0o644);queue.append(dest)
    for interp in re.findall(r'Requesting program interpreter: (.*?)\]',elf):assert interp=='/system/bin/linker64'
v38=ROOT/'firmware/extracted/android-ram-v38-20260917-r2'
old=json.loads((v38/'report.json').read_text())
assert sha((PRODUCT/'recovery/root/system/bin/linker64').read_bytes())==old['files']['/system/bin/linker64']['sha256']
put('a6l_simplefb.ko',ROOT/'firmware/extracted/android-display-v40-20260917-r1/a6l_simplefb.ko',0o400)
for name,info in files.items():
    p=ARCH/'payload'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes((OUT/'payload'/name).read_bytes())
(ARCH/'manifest.json').write_text(json.dumps({'files':files,'scope':'V41 real vendor allocator, metadata and cross-process native handles; no persistent mounts or writes'},indent=2)+'\n')
src=ROOT/'device/hisense/a6l/diagnostic/gralloc_probe.cpp';assert src.read_bytes()==(A/'device/hisense/a6l/diagnostic/gralloc_probe.cpp').read_bytes();(ARCH/src.name).write_bytes(src.read_bytes())
backend=json.loads((ROOT/'research/android-allocator-v41-20260917/report.json').read_text())
for name,info in backend['files'].items():assert sha((A/'external/minigbm'/name).read_bytes())==info['after_sha256']
(ARCH/'backend-report.json').write_text(json.dumps(backend,indent=2)+'\n')
for log in sorted(Path('/home/a6l/logs').glob('build-android-allocator-v41*.log')):
    (ARCH/log.name).write_bytes(log.read_bytes())
hal_paths=[
    'vendor/bin/hw/android.hardware.graphics.allocator@4.0-service.minigbm',
    'vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl.minigbm.so',
    'vendor/bin/hw/android.hardware.composer.hwc3-service.drm',
]
(ARCH/'built-graphics-services.json').write_text(json.dumps({
    'scope':'Built for later integration; services are not started by V41',
    'files':{name:{'sha256':sha((PRODUCT/name).read_bytes()),'bytes':(PRODUCT/name).stat().st_size} for name in hal_paths},
},indent=2)+'\n')
script=OUT/'qemu.sh';script.write_text('''#!/system/bin/sh
/system/bin/toybox mknod /dev/v41-console c 204 64
exec > /dev/v41-console 2>&1
echo A6L_QEMU_ALLOCATOR_START
/system/bin/toybox insmod /v41/a6l_simplefb.ko
code=$?
echo A6L_QEMU_ALLOCATOR_MODULE_EXIT=$code
if [ "$code" = 0 ]; then
    LD_LIBRARY_PATH=/v41/lib64 /v41/bin/a6l_gralloc_probe
    echo A6L_QEMU_ALLOCATOR_EXIT=$?
fi
echo A6L_QEMU_ALLOCATOR_DONE
''')
rc=OUT/'init.rc';rc.write_text((ROOT/'device/hisense/a6l/diagnostic/android-init.rc').read_text()+'''
on early-init
    start allocatortest
service allocatortest /system/bin/sh /v41/qemu.sh
    user root
    group root
    disabled
    oneshot
    seclabel u:r:su:s0
''')
lines=(v38/'ramdisk.list').read_text().splitlines()
lines=[f'file /system/etc/init/hw/init.rc {rc} 0644 0 0' if l.startswith('file /system/etc/init/hw/init.rc ') else l for l in lines]
dirs={'/v41'}
for name in files:dirs.update('/v41/'+str(p) for p in Path(name).parents if str(p)!='.')
lines += [f'dir {p} 0755 0 0' for p in sorted(dirs)]
lines += [f'file /v41/{name} {OUT/"payload"/name} {v["mode"]:04o} 0 0' for name,v in files.items()]
lines += [f'file /v41/qemu.sh {script} 0755 0 0']
recipe=OUT/'ramdisk.list';recipe.write_text('\n'.join(lines)+'\n')
(OUT/'ramdisk.gz').write_bytes(gzip.compress(run(BUILD/'usr/gen_init_cpio','-t','1789344000',recipe),mtime=0))
dtb=ROOT/'firmware/extracted/android-display-v40-20260917-r1/positive/virt.dtb'
args=['qemu-system-aarch64','-machine','virt,gic-version=3','-cpu','cortex-a53','-smp','2','-m','2048','-nodefaults','-nographic','-monitor','none','-serial','stdio','-nic','none','-no-reboot','-dtb',str(dtb),'-kernel',str(ROOT/'firmware/extracted/android-init-kernel-20260917/Image'),'-initrd',str(OUT/'ramdisk.gz'),'-append','console=ttyAMA0,115200 loglevel=1 panic=0 androidboot.selinux=permissive androidboot.init_rc=/system/etc/init/hw/init.rc']
with (OUT/'console.log').open('wb') as f:
    proc=subprocess.Popen(args,stdout=f,stderr=subprocess.STDOUT)
    try:
        deadline=time.monotonic()+65
        while time.monotonic()<deadline:
            data=(OUT/'console.log').read_bytes()
            if b'A6L_QEMU_ALLOCATOR_DONE' in data or b'Kernel panic' in data or proc.poll() is not None:break
            time.sleep(.2)
    finally:
        if proc.poll() is None:proc.kill();proc.wait(timeout=5)
data=(OUT/'console.log').read_bytes()
checks={'module_loaded':b'A6L_QEMU_ALLOCATOR_MODULE_EXIT=0' in data,'probe_exit_zero':b'A6L_QEMU_ALLOCATOR_EXIT=0' in data,'formats':all(b'A6L_GRALLOC_FORMAT_PASS name='+x in data for x in [b'RGBA8888',b'RGBX8888']),'fresh_processes':data.count(b'A6L_GRALLOC_CHILD_PASS')==2,'all_checks':b'A6L_GRALLOC_PASS formats=2 metadata=1 cross_process_import=1 pixel_coherence=1' in data,'no_panic':b'Kernel panic' not in data}
report={'passed':all(checks.values()),'checks':checks,'command':args,'console_sha256':sha(data),'payload_files':len(files),'payload_bytes':sum(x['bytes'] for x in files.values()),'scope':'Actual vendor minigbm allocator using simpleDRM; allocation, metadata, native handle transport and full pixel verification by fresh process for RGBA/RGBX. No compositor/GUI proof.'}
(ARCH/'qemu-report.json').write_text(json.dumps(report,indent=2)+'\n');(ARCH/'qemu-console.log').write_bytes(data)
print(json.dumps(report,indent=2));print(data[-6000:].decode(errors='replace'));assert report['passed']
