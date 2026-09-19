"""Run the actual AArch64 utility's bounds and refusal paths; no PMIC emulated."""
import argparse,gzip,hashlib,json,shutil,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser();ap.add_argument('--attempt',type=int,default=1);args=ap.parse_args()
OUT=Path(f'/home/a6l/kernel/control-probes-qemu-20260917-r{args.attempt}');OUT.mkdir(exist_ok=False)
ARCH=ROOT/'firmware/extracted/controls-radio-prep-20260917'
BUILD=Path('/home/a6l/kernel/out-a6l-android-init')
kernel=ROOT/'firmware/extracted/android-init-kernel-20260917/Image'
assert hashlib.sha256(kernel.read_bytes()).hexdigest()=='0dc31cdc9c3a6ecec53ae919737480783d104ff8451018d1448573efd140d2d7'
exe=Path('/home/a6l/android/a6l-lineage24/out/target/product/a6l/system/bin/a6l_haptic_probe')
shutil.copyfile(exe,ARCH/exe.name)
backlight=ROOT/'device/hisense/a6l/diagnostic/backlight_probe.sh'
shutil.copyfile(backlight,ARCH/backlight.name)
subprocess.run(['sh','-n',str(backlight)],check=True)
script=OUT/'test.sh';script.write_text('''#!/system/bin/sh
/system/bin/toybox mknod /dev/control-console c 204 64
exec > /dev/control-console 2>&1
/controls/a6l_haptic_probe --self-test
echo A6L_BOUNDS_EXIT=$?
/controls/a6l_haptic_probe
echo A6L_NOARGS_EXIT=$?
/controls/a6l_haptic_probe --pulse /dev/null
echo A6L_WRONGPATH_EXIT=$?
/controls/a6l_haptic_probe --pulse /dev/input/event999
echo A6L_ABSENT_EXIT=$?
/system/bin/sh /controls/backlight_probe.sh --test
echo A6L_NOBACKLIGHT_EXIT=$?
echo A6L_CONTROL_PROBES_DONE
''')
rc=OUT/'init.rc';rc.write_text((ROOT/'device/hisense/a6l/diagnostic/android-init.rc').read_text()+'''
on early-init
    start controlprobetest
service controlprobetest /system/bin/sh /controls/test.sh
    user root
    group root
    disabled
    oneshot
    seclabel u:r:su:s0
''')
lines=(ROOT/'firmware/extracted/android-ram-v38-20260917-r2/ramdisk.list').read_text().splitlines()
lines=[f'file /system/etc/init/hw/init.rc {rc} 0644 0 0' if x.startswith('file /system/etc/init/hw/init.rc ') else x for x in lines]
lines+=['dir /controls 0755 0 0',f'file /controls/test.sh {script} 0755 0 0',f'file /controls/a6l_haptic_probe {exe} 0755 0 0',f'file /controls/backlight_probe.sh {backlight} 0755 0 0']
recipe=OUT/'ramdisk.list';recipe.write_text('\n'.join(lines)+'\n')
(OUT/'ramdisk.gz').write_bytes(gzip.compress(subprocess.check_output([str(BUILD/'usr/gen_init_cpio'),'-t','1789344000',str(recipe)]),mtime=0))
cmd=['qemu-system-aarch64','-machine','virt,gic-version=3','-cpu','cortex-a53','-smp','2','-m','1024','-nodefaults','-nographic','-monitor','none','-serial','stdio','-nic','none','-no-reboot','-kernel',str(kernel),'-initrd',str(OUT/'ramdisk.gz'),'-append','console=ttyAMA0,115200 loglevel=1 panic=0 androidboot.selinux=permissive androidboot.init_rc=/system/etc/init/hw/init.rc']
with (OUT/'console.log').open('wb') as f:
    p=subprocess.Popen(cmd,stdout=f,stderr=subprocess.STDOUT)
    try:
        deadline=time.monotonic()+60
        while time.monotonic()<deadline:
            data=(OUT/'console.log').read_bytes()
            if b'A6L_CONTROL_PROBES_DONE' in data or b'Kernel panic' in data or p.poll() is not None:break
            time.sleep(.1)
    finally:
        if p.poll() is None:p.kill();p.wait(timeout=5)
data=(OUT/'console.log').read_text(errors='replace');(ARCH/'qemu-controls.log').write_text(data)
checks={name:f'A6L_{name}_EXIT={code}' in data.splitlines() for name,code in [('BOUNDS',0),('NOARGS',2),('WRONGPATH',2),('ABSENT',1),('NOBACKLIGHT',1)]}
checks.update(no_panic='Kernel panic' not in data,finished='A6L_CONTROL_PROBES_DONE' in data,voltage_bound='rounded_mv=1276' in data)
checks['backlight_refused_missing_device']='A6L_BACKLIGHT_REFUSED no-compatible-device' in data
report={'passed':all(checks.values()),'checks':checks,'utility_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'scope':'AArch64 bounds and absent-device refusal paths; no haptic or backlight hardware emulated'}
(ARCH/'qemu-controls-report.json').write_text(json.dumps(report,indent=2)+'\n')
print('CONTROL_PROBES_QEMU',report,flush=True)
assert report['passed']
