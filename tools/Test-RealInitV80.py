"""Boot the REAL Android init (selinux_setup -> second_stage) inside a PID+mount namespace, from RAM, on the phone kernel in
QEMU. Inputs: the build's labeled EROFS system.img + vendor.img. No phone access. usage: Test-RealInitV80.py <attempt> [minutes]"""
import gzip,hashlib,json,shutil,struct,subprocess,sys,time,glob
from pathlib import Path
N=sys.argv[1];MIN=int(sys.argv[2]) if len(sys.argv)>2 else 30
ROOT=Path(__file__).resolve().parents[1];P=Path('/home/a6l/android/a6l-lineage24/out/target/product/a6l')
OUT=Path(f'/home/a6l/kernel/realinit-v80-r{N}');OUT.mkdir(exist_ok=False)
ARCH=ROOT/f'firmware/extracted/realinit-v80-r{N}';ARCH.mkdir(exist_ok=False)
def trimmed(src,dst):
    with open(src,'rb') as f:
        f.seek(1024);sb=f.read(128);assert sb[:4]==bytes.fromhex('e2e1f5e0'),'not erofs'
        blkbits=sb[12];blocks=struct.unpack_from('<I',sb,36)[0];size=blocks<<blkbits
        f.seek(0)
        with open(dst,'wb') as o:
            left=size
            while left:
                b=f.read(min(left,1<<24));assert b;o.write(b);left-=len(b)
    return size
sizes={n:trimmed(P/f'{n}.img',OUT/f'{n}.erofs') for n in ('system','vendor')}
kernel_archive=Path(sorted(glob.glob(str(ROOT/'firmware/extracted/phone-kernel-v67-candidate-*')))[-1])
import tarfile
with tarfile.open(kernel_archive/'modules.tar.gz') as t:(OUT/'overlay.ko').write_bytes(t.extractfile(next(m for m in t.getmembers() if m.name.endswith('/overlay.ko'))).read())
launcher=OUT/'realinit-launch.sh';launcher.write_bytes((ROOT/'device/hisense/a6l/diagnostic/realinit-launch.sh').read_bytes().replace(b'\r\n',b'\n'))
script=OUT/'test.sh';script.write_text('''#!/system/bin/sh
/system/bin/toybox mknod /dev/ri-console c 204 64
exec > /dev/ri-console 2>&1
echo A6L_RI_TEST_BEGIN
/system/bin/toybox mkdir -p /tmp
P=/ri-in /system/bin/sh /ri-in/realinit-launch.sh &
L=$!
i=0; while [ $i -lt %d ]; do /system/bin/toybox sleep 30; i=$((i+1)); echo "A6L_RI_TICK $i procs=$(/system/bin/toybox ls /proc | /system/bin/toybox grep -c '^[0-9]')"; kill -0 $L 2>/dev/null || { echo A6L_RI_LAUNCHER_EXITED; break; }; done
echo A6L_RI_TEST_DONE
''' % (MIN*2))
rc=OUT/'init.rc';rc.write_text((ROOT/'device/hisense/a6l/diagnostic/android-init.rc').read_text()+'''
on early-init
    start ritest
service ritest /system/bin/sh /ri-in/test.sh
    user root
    group root
    disabled
    oneshot
    seclabel u:r:su:s0
''')
lines=(ROOT/'firmware/extracted/android-ram-v38-20260917-r2/ramdisk.list').read_text().splitlines()
lines=[f'file /system/etc/init/hw/init.rc {rc} 0644 0 0' if x.startswith('file /system/etc/init/hw/init.rc ') else x for x in lines]
lines+=['dir /ri-in 0755 0 0','dir /ri-in/logs 0755 0 0',f'file /ri-in/test.sh {script} 0755 0 0',f'file /ri-in/realinit-launch.sh {launcher} 0755 0 0',
        f'file /ri-in/system.erofs {OUT/"system.erofs"} 0400 0 0',f'file /ri-in/vendor.erofs {OUT/"vendor.erofs"} 0400 0 0',f'file /ri-in/overlay.ko {OUT/"overlay.ko"} 0400 0 0']
recipe=OUT/'ramdisk.list';recipe.write_text('\n'.join(lines)+'\n')
data=subprocess.check_output(['/home/a6l/kernel/out-a6l-android-init/usr/gen_init_cpio','-t','1789344000',str(recipe)])
with gzip.open(OUT/'ramdisk.gz','wb',compresslevel=1) as f:f.write(data)
del data
dtb=sorted(glob.glob(str(ROOT/'firmware/extracted/android-framework-v72-*-r3/virt.dtb')))[-1]
cmd=['qemu-system-aarch64','-machine','virt,gic-version=3','-cpu','cortex-a53','-smp','4','-m','6144','-nodefaults','-nographic','-monitor','none','-serial','stdio','-nic','none','-no-reboot',
     '-dtb',dtb,'-kernel',str(kernel_archive/'Image'),'-initrd',str(OUT/'ramdisk.gz'),
     '-append','console=ttyAMA0,115200 earlycon loglevel=7 printk.devkmsg=on panic=0 androidboot.selinux=permissive androidboot.hardware=qcom androidboot.init_rc=/system/etc/init/hw/init.rc']
print('RI_BOOT images',sizes,flush=True)
with (OUT/'console.log').open('wb') as f:
    p=subprocess.Popen(cmd,stdout=f,stderr=subprocess.STDOUT)
    try:
        deadline=time.monotonic()+MIN*60+300
        while time.monotonic()<deadline:
            d=(OUT/'console.log').read_bytes()
            if b'A6L_RI_TEST_DONE' in d or b'Kernel panic' in d or p.poll() is not None:break
            time.sleep(2)
    finally:
        if p.poll() is None:p.kill();p.wait(timeout=5)
log=(OUT/'console.log').read_text(errors='replace');(ARCH/'console.log').write_text(log)
checks={'launcher_inner':'A6L_RI_INNER' in log,'exec_init':'A6L_RI_EXEC_INIT' in log,'selinux_setup':'init: Loading SELinux policy' in log or 'SELinux: Loaded' in log,
        'second_stage':'init second stage started' in log,'ueventd':"starting service 'ueventd'" in log,'zygote':"starting service 'zygote" in log,'boot_completed':'sys.boot_completed' in log,'no_panic':'Kernel panic' not in log}
(ARCH/'report.json').write_text(json.dumps({'checks':checks,'images':sizes,'scope':'real init in a PID/mount namespace on the phone kernel, QEMU only'},indent=2)+'\n')
print('REALINIT_V80',json.dumps(checks),flush=True)
