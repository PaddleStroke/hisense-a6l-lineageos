"""V73: run the a6l_epdd e-paper service in --dry mode in diskless QEMU (stock libtcon_eink.so + real waveform) and dump every update.
usage: Test-EpddQemu.py <attempt> <waveform.bin> <a6l_epdd> <script> <files...>   No phone access."""
import gzip,hashlib,json,re,shutil,subprocess,time
from pathlib import Path
import sys,datetime
N=sys.argv[1];WF=Path(sys.argv[2]);EPDD=Path(sys.argv[3]);SCRIPT=Path(sys.argv[4]);EXTRAF=[Path(x) for x in sys.argv[5:]];DUMP=['--dump'];TEMP='25';STAMP=datetime.date.today().strftime('%Y%m%d')
ROOT=Path(__file__).resolve().parents[1]
OUT=Path(f'/home/a6l/kernel/epdd-qemu-{STAMP}-r{N}');OUT.mkdir(exist_ok=False)
ARCH=ROOT/f'firmware/extracted/epdd-qemu-{STAMP}-r{N}';ARCH.mkdir(exist_ok=False)
LIBS=ARCH/'lib64';LIBS.mkdir()
SURFACE=ROOT/'firmware/extracted/android-surface-v44-20260917-r7'
assert json.loads((SURFACE/'qemu-report.json').read_text())['passed']
manifest=json.loads((SURFACE/'manifest.json').read_text())['files']
files={}
def copy(src,dest):
    shutil.copyfile(src,dest);files[dest.relative_to(ARCH).as_posix()]={'source':str(src),'sha256':hashlib.sha256(dest.read_bytes()).hexdigest(),'bytes':dest.stat().st_size}
copy(EPDD,ARCH/'a6l_epdd')
copy(ROOT/'firmware/extracted/vendor/lib64/libtcon_eink.so',LIBS/'libtcon_eink.so')
queue=[ARCH/'a6l_epdd',LIBS/'libtcon_eink.so'];seen=set()
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
copy(WF,ARCH/'waveform.bin');copy(SCRIPT,ARCH/'script.txt')
for x in EXTRAF:copy(x,ARCH/x.name)
(ARCH/'manifest.json').write_text(json.dumps({'files':files,'phone_access':False,'init_function_called':True,'waveform':'file' if WF else 'zeros'},indent=2)+'\n')
script=OUT/'test.sh';script.write_text('''#!/system/bin/sh
/system/bin/toybox mknod /dev/eink-console c 204 64
exec > /dev/eink-console 2>&1
export LD_LIBRARY_PATH=/eink/lib64
/eink/a6l_epdd --waveform /eink/waveform.bin --lib /eink/lib64/libtcon_eink.so --dry - --script /eink/script.txt --temp 25
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
cmd=['qemu-system-aarch64','-machine','virt,gic-version=3','-cpu','cortex-a53','-smp','2','-m','2048','-nodefaults','-nographic','-monitor','none','-serial','stdio','-nic','none','-no-reboot','-kernel',str(kernel),'-initrd',str(OUT/'ramdisk.gz'),'-append','console=ttyAMA0,115200 loglevel=1 panic=0 androidboot.selinux=permissive androidboot.init_rc=/system/etc/init/hw/init.rc']
with (OUT/'console.log').open('wb') as f:
    p=subprocess.Popen(cmd,stdout=f,stderr=subprocess.STDOUT)
    try:
        deadline=time.monotonic()+(7000 if DUMP else 600)
        while time.monotonic()<deadline:
            data=(OUT/'console.log').read_bytes()
            if b'A6L_EINK_ABI_DONE' in data or b'Kernel panic' in data or p.poll() is not None:break
            time.sleep(.1)
    finally:
        if p.poll() is None:p.kill();p.wait(timeout=5)
data=(OUT/'console.log').read_text(errors='replace');(ARCH/'qemu.log').write_text(data)
checks={'done':'A6L_EINK_ABI_DONE' in data,'no_panic':'Kernel panic' not in data,'epdd_exit0':'A6L_EINK_ABI_EXIT=0' in data,'no_fail':'A6L_EPDD FAIL' not in data,'nothing_refused':'refused' not in data}
lines=[l for l in data.splitlines() if ('A6L_EPDD' in l or l.startswith('A6L_EINK_')) and 'A6L_EINK_RLE' not in l]
if DUMP:
    import struct
    for upd in [str(i) for i in range(1,20)]:
        frames=[]
        if not any(l.startswith(f'A6L_EINK_RLE {upd} ') for l in data.splitlines()):continue
        for l in data.splitlines():
            if not l.startswith(f'A6L_EINK_RLE {upd} '):continue
            runs=[tuple(int(x,16) for x in r.split(':')) for r in l.split()[3:]];assert sum(c for c,_ in runs)==384*725,(upd,len(frames));frames.append(runs)
        blob=b'A6LEPD1\n'+struct.pack('<III',384,725,len(frames))+b''.join(struct.pack('<I',len(r))+b''.join(struct.pack('<II',c,v) for c,v in r) for r in frames)
        (ARCH/f'update{upd}-t{TEMP}.a6lepd').write_bytes(blob);lines.append(f'DUMPED update{upd} frames={len(frames)} bytes={len(blob)} values={sorted({hex(v) for r in frames for _,v in r})[:40]}')
report={'passed':all(checks.values()),'checks':checks,'probe_output':lines,'scope':'a6l_epdd --dry in QEMU with the real waveform. No panel, DRM or phone.'}
(ARCH/'qemu-report.json').write_text(json.dumps(report,indent=2)+'\n');print('EINK_SWTCON_QEMU',json.dumps(report,indent=1),flush=True)
