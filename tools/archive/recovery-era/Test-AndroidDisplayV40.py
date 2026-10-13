"""Package and test the RAM-only LCD adapter/probe on the exact V38 kernel."""
import argparse,gzip,hashlib,json,os,socket,struct,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
A=Path('/home/a6l/android/a6l-lineage24')
K=ROOT/'firmware/extracted/android-init-kernel-20260917'
BUILD=Path('/home/a6l/kernel/out-a6l-android-init')
ap=argparse.ArgumentParser();ap.add_argument('--attempt',type=int,required=True);n=ap.parse_args().attempt
OUT=Path(f'/home/a6l/kernel/display-v40-r{n}');OUT.mkdir(exist_ok=False)
ARCH=ROOT/f'firmware/extracted/android-display-v40-20260917-r{n}';ARCH.mkdir(exist_ok=False)
def run(*a):return subprocess.check_output([str(x) for x in a],stderr=subprocess.STDOUT)
def sha(b):return hashlib.sha256(b).hexdigest()
files={}
for name,source in [('a6l_simplefb.ko',Path('/home/a6l/kernel/a6l-simplefb-v40/a6l_simplefb.ko')),('a6l_drm_probe',A/'out/target/product/a6l/system/bin/a6l_drm_probe')]:
    data=source.read_bytes();(OUT/name).write_bytes(data);(ARCH/name).write_bytes(data)
    files[name]={'sha256':sha(data),'bytes':len(data)}
assert b'NEEDED' not in run('readelf','-d',OUT/'a6l_drm_probe')
assert sha((K/'Image').read_bytes())=='0dc31cdc9c3a6ecec53ae919737480783d104ff8451018d1448573efd140d2d7'
assert (BUILD/'arch/arm64/boot/Image').read_bytes()==(K/'Image').read_bytes()
assert b'7.2.3-a6l-probe+' in run('modinfo',OUT/'a6l_simplefb.ko')
for rel,copy in [('diagnostic/drm_probe.c',A/'device/hisense/a6l/diagnostic/drm_probe.c'),('kernel/a6l_simplefb.c',Path('/home/a6l/kernel/a6l-simplefb-v40/a6l_simplefb.c'))]:
    data=(ROOT/'device/hisense/a6l'/rel).read_bytes();assert data==copy.read_bytes();(ARCH/Path(rel).name).write_bytes(data)
(ARCH/'build.log').write_bytes(Path('/home/a6l/logs/build-android-display-v40.log').read_bytes())
script=OUT/'qemu.sh';script.write_text('''#!/system/bin/sh
/system/bin/toybox mknod /dev/v40-console c 204 64
exec > /dev/v40-console 2>&1
/system/bin/toybox mkdir /tmp/a6l-v40
echo A6L_QEMU_DISPLAY_START
/system/bin/toybox insmod /v40/a6l_simplefb.ko
code=$?
echo A6L_QEMU_DISPLAY_MODULE_EXIT=$code
if [ "$code" = 0 ]; then
    /v40/a6l_drm_probe
    echo A6L_QEMU_DISPLAY_PROBE_EXIT=$?
fi
echo A6L_QEMU_DISPLAY_DONE
''')
rc=OUT/'init.rc';rc.write_text((ROOT/'device/hisense/a6l/diagnostic/android-init.rc').read_text()+'''
on early-init
    start displaytest
service displaytest /system/bin/sh /v40/qemu.sh
    user root
    group root
    disabled
    oneshot
    seclabel u:r:su:s0
''')
lines=(ROOT/'firmware/extracted/android-ram-v38-20260917-r2/ramdisk.list').read_text().splitlines()
lines=[f'file /system/etc/init/hw/init.rc {rc} 0644 0 0' if l.startswith('file /system/etc/init/hw/init.rc ') else l for l in lines]
lines+=['dir /v40 0755 0 0']+[f'file /v40/{s} {OUT/s} 0755 0 0' for s in ['qemu.sh',*files]]
recipe=OUT/'ramdisk.list';recipe.write_text('\n'.join(lines)+'\n')
ram=gzip.compress(run(BUILD/'usr/gen_init_cpio','-t','1789344000',recipe),mtime=0)
(OUT/'ramdisk.gz').write_bytes(ram)
run('qemu-system-aarch64','-machine',f'virt,gic-version=3,dumpdtb={OUT}/virt.dtb','-cpu','cortex-a53','-smp','2','-m','2048','-nodefaults','-nographic')
report={'passed':False,'files':files,'kernel_sha256':sha((K/'Image').read_bytes()),'cases':{},'scope':'Diskless real simpleDRM buffer, PRIME and atomic update tests, guarded board/reservation negatives; not physical LCD proof'}
try:
    for name,board,size in [('positive','hisense,hlte730t',0x23ff000),('wrong-board','linux,dummy-virt',0x23ff000),('short-reservation','hisense,hlte730t',0x1000)]:
        d=OUT/name;d.mkdir()
        guard='guard@9d401000 { reg = <0 0x9d401000 0 0x23fe000>; no-map; };' if size==0x1000 else ''
        (d/'fixture.dts').write_text(f'''/dts-v1/; /plugin/;
/ {{ fragment@0 {{ target-path="/"; __overlay__ {{ compatible="{board}", "linux,dummy-virt";
reserved-memory {{ #address-cells=<2>; #size-cells=<2>; ranges;
framebuffer@9d400000 {{ reg=<0 0x9d400000 0 {size}>; no-map; }}; {guard}
}}; }}; }}; }};
''')
        run('dtc','-@','-I','dts','-O','dtb','-o',d/'fixture.dtbo',d/'fixture.dts')
        run('fdtoverlay','-i',OUT/'virt.dtb','-o',d/'virt.dtb',d/'fixture.dtbo')
        sock=Path(f'/tmp/v40-{os.getpid()}-{name}.sock')
        args=['qemu-system-aarch64','-machine','virt,gic-version=3','-cpu','cortex-a53','-smp','2','-m','2048','-nodefaults','-nographic','-monitor','none','-serial','stdio','-nic','none','-no-reboot','-qmp',f'unix:{sock},server=on,wait=off','-dtb',str(d/'virt.dtb'),'-kernel',str(K/'Image'),'-initrd',str(OUT/'ramdisk.gz'),'-append','console=ttyAMA0,115200 loglevel=1 panic=0 androidboot.selinux=permissive androidboot.init_rc=/system/etc/init/hw/init.rc']
        sampled=False;proc=None;case={'command':args,'passed':False}
        try:
            with (d/'console.log').open('wb') as f:
                proc=subprocess.Popen(args,stdout=f,stderr=subprocess.STDOUT)
                deadline=time.monotonic()+5
                while not sock.exists() and time.monotonic()<deadline:time.sleep(.05)
                with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as conn:
                    conn.settimeout(5);conn.connect(str(sock));stream=conn.makefile('rwb',buffering=0);stream.readline()
                    def qmp(command,arguments=None):
                        req={'execute':command}
                        if arguments:req['arguments']=arguments
                        stream.write(json.dumps(req).encode()+b'\n')
                        while True:
                            msg=json.loads(stream.readline());assert 'error' not in msg,msg
                            if 'return' in msg:return msg['return']
                    qmp('qmp_capabilities');deadline=time.monotonic()+65
                    while time.monotonic()<deadline:
                        data=(d/'console.log').read_bytes()
                        if not sampled and (b'A6L_DRM_VISIBLE' in data or b'A6L_QEMU_DISPLAY_DONE' in data or b'A6L_DRM_FAIL' in data):
                            qmp('stop');qmp('pmemsave',{'val':0x9d400000,'size':1080*2340*4+4096,'filename':str(d/'framebuffer.bin')});qmp('cont');sampled=True
                        if b'A6L_QEMU_DISPLAY_DONE' in data or b'Kernel panic' in data or proc.poll() is not None:break
                        time.sleep(.1)
            data=(d/'console.log').read_bytes();frame=(d/'framebuffer.bin').read_bytes() if sampled else b''
            checks={'completed':b'A6L_QEMU_DISPLAY_DONE' in data,'no_panic':b'Kernel panic' not in data,'sampled':sampled}
            if name=='positive':
                checks.update(module_loaded=b'A6L_QEMU_DISPLAY_MODULE_EXIT=0' in data,probe_passed=b'A6L_QEMU_DISPLAY_PROBE_EXIT=0' in data and b'A6L_DRM_PASS' in data,cleanup=b'A6L_DRM_CLEANUP_PASS' in data)
                if sampled:
                    actual=[struct.unpack_from('<I',frame,(100*1080+x)*4)[0] for x in [10,226,442,658,874]]
                    checks['color_pixels']=actual==[0xffff0000,0xff00ff00,0xff0000ff,0xffffffff,0xff000000]
                    checks['gradient_pixel']=struct.unpack_from('<I',frame,(1800*1080+540)*4)[0]==(0xff000000|((540*255//1080)<<16)|((1800*255//2340)<<8)|0x40)
                    checks['outside_frame_untouched']=not any(frame[1080*2340*4:]);case['sampled_colors']=actual
            else:
                checks['module_refused']=b'A6L_QEMU_DISPLAY_MODULE_EXIT=1' in data
                checks['probe_not_run']=b'A6L_DRM_DRIVER' not in data
                checks['frame_untouched']=sampled and not any(frame)
            case.update(checks=checks,passed=all(checks.values()),console_sha256=sha(data))
            print(json.dumps({'case':name,**case}),flush=True)
        finally:
            if proc and proc.poll() is None:proc.kill();proc.wait(timeout=5)
            sock.unlink(missing_ok=True);report['cases'][name]=case
            (d/'report.json').write_text(json.dumps(case,indent=2)+'\n')
    report['passed']=all(c['passed'] for c in report['cases'].values())
finally:
    (ARCH/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    for source in OUT.rglob('*'):
        if source.is_file():
            dest=ARCH/source.relative_to(OUT);dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(source.read_bytes())
print(json.dumps({'passed':report['passed'],'output':str(ARCH)}));assert report['passed']
