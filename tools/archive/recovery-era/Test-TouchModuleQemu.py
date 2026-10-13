"""Check the real FT8719 module ABI in the exact V38 kernel; no emulated touch claim."""
import gzip,hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OUT=Path('/home/a6l/kernel/touch-module-20260917');OUT.mkdir(exist_ok=False)
ARCH=ROOT/'firmware/extracted/touchscreen-prep-20260917'
BUILD=Path('/home/a6l/kernel/out-a6l-android-init')
kernel=ROOT/'firmware/extracted/android-init-kernel-20260917/Image'
assert hashlib.sha256(kernel.read_bytes()).hexdigest()=='0dc31cdc9c3a6ecec53ae919737480783d104ff8451018d1448573efd140d2d7'
script=OUT/'test.sh';script.write_text('''#!/system/bin/sh
/system/bin/toybox mknod /dev/touch-console c 204 64
exec > /dev/touch-console 2>&1
echo A6L_TOUCH_ABI_START
/system/bin/toybox insmod /touch/edt-ft5x06.ko
echo A6L_TOUCH_LOAD_EXIT=$?
/system/bin/toybox cat /proc/modules
/system/bin/toybox ls /sys/bus/i2c/drivers/edt_ft5x06
/system/bin/toybox rmmod edt_ft5x06
echo A6L_TOUCH_UNLOAD_EXIT=$?
if [ ! -d /sys/bus/i2c/drivers/edt_ft5x06 ]; then echo A6L_TOUCH_DRIVER_REMOVED=1; fi
echo A6L_TOUCH_ABI_DONE
''')
rc=OUT/'init.rc';rc.write_text((ROOT/'device/hisense/a6l/diagnostic/android-init.rc').read_text()+'''
on early-init
    start touchabitest
service touchabitest /system/bin/sh /touch/test.sh
    user root
    group root
    disabled
    oneshot
    seclabel u:r:su:s0
''')
lines=(ROOT/'firmware/extracted/android-ram-v38-20260917-r2/ramdisk.list').read_text().splitlines()
lines=[f'file /system/etc/init/hw/init.rc {rc} 0644 0 0' if x.startswith('file /system/etc/init/hw/init.rc ') else x for x in lines]
lines+=['dir /touch 0755 0 0',f'file /touch/test.sh {script} 0755 0 0',f'file /touch/edt-ft5x06.ko {ARCH}/edt-ft5x06.ko 0400 0 0']
recipe=OUT/'ramdisk.list';recipe.write_text('\n'.join(lines)+'\n')
(OUT/'ramdisk.gz').write_bytes(gzip.compress(subprocess.check_output([str(BUILD/'usr/gen_init_cpio'),'-t','1789344000',str(recipe)]),mtime=0))
cmd=['qemu-system-aarch64','-machine','virt,gic-version=3','-cpu','cortex-a53','-smp','2','-m','1024','-nodefaults','-nographic','-monitor','none','-serial','stdio','-nic','none','-no-reboot','-kernel',str(kernel),'-initrd',str(OUT/'ramdisk.gz'),'-append','console=ttyAMA0,115200 loglevel=1 panic=0 androidboot.selinux=permissive androidboot.init_rc=/system/etc/init/hw/init.rc']
import time
with (OUT/'console.log').open('wb') as f:
    p=subprocess.Popen(cmd,stdout=f,stderr=subprocess.STDOUT)
    try:
        deadline=time.monotonic()+45
        while time.monotonic()<deadline:
            data=(OUT/'console.log').read_bytes()
            if b'A6L_TOUCH_ABI_DONE' in data or b'Kernel panic' in data or p.poll() is not None:break
            time.sleep(.1)
    finally:
        if p.poll() is None:p.kill();p.wait(timeout=5)
data=(OUT/'console.log').read_bytes();(ARCH/'qemu-module.log').write_bytes(data)
checks={k:mark in data for k,mark in {'loaded':b'A6L_TOUCH_LOAD_EXIT=0','listed':b'edt_ft5x06 ','unloaded':b'A6L_TOUCH_UNLOAD_EXIT=0','removed':b'A6L_TOUCH_DRIVER_REMOVED=1','finished':b'A6L_TOUCH_ABI_DONE'}.items()}
checks['no_panic']=b'Kernel panic' not in data
report={'passed':all(checks.values()),'checks':checks,'scope':'Real upstream module load/unload only; no I2C controller or physical touch events emulated','command':cmd}
(ARCH/'qemu-module-report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2));assert report['passed']
