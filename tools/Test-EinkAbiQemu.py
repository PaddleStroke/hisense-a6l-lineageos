"""Load original e-ink TCON against modern Android libraries in diskless QEMU."""
import gzip,hashlib,json,re,shutil,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OUT=Path('/home/a6l/kernel/eink-abi-qemu-20260917');OUT.mkdir(exist_ok=False)
ARCH=ROOT/'firmware/extracted/eink-abi-20260917';ARCH.mkdir(exist_ok=False)
LIBS=ARCH/'lib64';LIBS.mkdir()
SURFACE=ROOT/'firmware/extracted/android-surface-v44-20260917-r7'
assert json.loads((SURFACE/'qemu-report.json').read_text())['passed']
manifest=json.loads((SURFACE/'manifest.json').read_text())['files']
files={}
def copy(src,dest):
    shutil.copyfile(src,dest);files[dest.relative_to(ARCH).as_posix()]={'source':str(src),'sha256':hashlib.sha256(dest.read_bytes()).hexdigest(),'bytes':dest.stat().st_size}
copy(Path('/home/a6l/android/a6l-lineage24/out/target/product/a6l/system/bin/a6l_eink_abi_probe'),ARCH/'a6l_eink_abi_probe')
copy(ROOT/'firmware/extracted/vendor/lib64/libtcon_eink.so',LIBS/'libtcon_eink.so')
queue=[ARCH/'a6l_eink_abi_probe',LIBS/'libtcon_eink.so'];seen=set()
while queue:
    p=queue.pop()
    if p in seen:continue
    seen.add(p)
    needed=re.findall(r'Shared library: \[(.*?)\]',subprocess.check_output(['readelf','-d',str(p)],text=True))
    for name in needed:
        target=LIBS/name
        if target.exists():continue
        key='root/system/lib64/'+name;src=SURFACE/'payload'/key
        assert hashlib.sha256(src.read_bytes()).hexdigest()==manifest[key]['sha256']
        copy(src,target);queue.append(target)
(ARCH/'manifest.json').write_text(json.dumps({'files':files,'phone_access':False,'init_function_called':False},indent=2)+'\n')
script=OUT/'test.sh';script.write_text('''#!/system/bin/sh
/system/bin/toybox mknod /dev/eink-console c 204 64
exec > /dev/eink-console 2>&1
export LD_LIBRARY_PATH=/eink/lib64
/eink/a6l_eink_abi_probe
echo A6L_EINK_ABI_EXIT=$?
echo A6L_EINK_ABI_DONE
''')
rc=OUT/'init.rc';rc.write_text((ROOT/'device/hisense/a6l/diagnostic/android-init.rc').read_text()+'''
on early-init
    start einkabitest
service einkabitest /system/bin/sh /eink/test.sh
    user root
    group root
    disabled
    oneshot
    seclabel u:r:su:s0
''')
lines=(ROOT/'firmware/extracted/android-ram-v38-20260917-r2/ramdisk.list').read_text().splitlines()
lines=[f'file /system/etc/init/hw/init.rc {rc} 0644 0 0' if x.startswith('file /system/etc/init/hw/init.rc ') else x for x in lines]
lines+=['dir /eink 0755 0 0','dir /eink/lib64 0755 0 0',f'file /eink/test.sh {script} 0755 0 0']
lines += [f'file /eink/{n} {ARCH/n} 0755 0 0' for n in files]
recipe=OUT/'ramdisk.list';recipe.write_text('\n'.join(lines)+'\n')
builder=Path('/home/a6l/kernel/out-a6l-android-init/usr/gen_init_cpio')
(OUT/'ramdisk.gz').write_bytes(gzip.compress(subprocess.check_output([str(builder),'-t','1789344000',str(recipe)]),mtime=0))
kernel=ROOT/'firmware/extracted/android-init-kernel-20260917/Image'
assert hashlib.sha256(kernel.read_bytes()).hexdigest()=='0dc31cdc9c3a6ecec53ae919737480783d104ff8451018d1448573efd140d2d7'
cmd=['qemu-system-aarch64','-machine','virt,gic-version=3','-cpu','cortex-a53','-smp','2','-m','1024','-nodefaults','-nographic','-monitor','none','-serial','stdio','-nic','none','-no-reboot','-kernel',str(kernel),'-initrd',str(OUT/'ramdisk.gz'),'-append','console=ttyAMA0,115200 loglevel=1 panic=0 androidboot.selinux=permissive androidboot.init_rc=/system/etc/init/hw/init.rc']
with (OUT/'console.log').open('wb') as f:
    p=subprocess.Popen(cmd,stdout=f,stderr=subprocess.STDOUT)
    try:
        deadline=time.monotonic()+45
        while time.monotonic()<deadline:
            data=(OUT/'console.log').read_bytes()
            if b'A6L_EINK_ABI_DONE' in data or b'Kernel panic' in data or p.poll() is not None:break
            time.sleep(.1)
    finally:
        if p.poll() is None:p.kill();p.wait(timeout=5)
data=(OUT/'console.log').read_text(errors='replace');(ARCH/'qemu.log').write_text(data)
checks={'success':'A6L_EINK_ABI_PASS version=2.2 guards=1 init_called=0' in data,'exit_zero':'A6L_EINK_ABI_EXIT=0' in data.splitlines(),'done':'A6L_EINK_ABI_DONE' in data,'no_panic':'Kernel panic' not in data}
report={'passed':all(checks.values()),'checks':checks,'scope':'Actual stock TCON dlopen RTLD_NOW, four exports, disassembled version ABI and guarded output buffers with modern Android libraries. No panel initialization, waveform conversion, DRM or physical hardware.'}
(ARCH/'qemu-report.json').write_text(json.dumps(report,indent=2)+'\n');print('EINK_ABI_QEMU',report,flush=True);assert report['passed']
