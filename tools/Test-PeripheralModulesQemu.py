"""Load/unload the real peripheral module dependency graph in validated V38 QEMU.

This verifies kernel ABI and dependency completeness, not A6L hardware operation.
No physical device, radios, speaker, or firmware is accessed.
"""
import argparse,gzip,hashlib,json,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser();ap.add_argument('--profile',choices=['peripheral','power-sensors','controls-radio','controls-v45','haptics-brake'],default='peripheral');args=ap.parse_args()
date='20260918' if args.profile=='haptics-brake' else '20260917'
OUT=Path(f'/home/a6l/kernel/{args.profile}-module-qemu-{date}');OUT.mkdir(exist_ok=False)
ARCH=ROOT/f'firmware/extracted/{args.profile}-prep-{date}'
BUILD=Path('/home/a6l/kernel/out-a6l-android-init')
kernel=ROOT/'firmware/extracted/android-init-kernel-20260917/Image'
assert hashlib.sha256(kernel.read_bytes()).hexdigest()=='0dc31cdc9c3a6ecec53ae919737480783d104ff8451018d1448573efd140d2d7'
modules=json.loads((ARCH/'module-manifest.json').read_text())['load_order']
commands=['#!/system/bin/sh','/system/bin/toybox mknod /dev/peripheral-console c 204 64','exec > /dev/peripheral-console 2>&1','echo A6L_PERIPHERAL_ABI_START']
for m in modules:
    assert hashlib.sha256((ARCH/'modules'/m['file']).read_bytes()).hexdigest()==m['sha256']
    commands += [f'/system/bin/toybox insmod /peripheral/{m["file"]}',f'echo A6L_LOAD_{m["name"]}=$?']
commands+=['/system/bin/toybox cat /proc/modules']
for m in reversed(modules):
    commands += [f'/system/bin/toybox rmmod {m["name"]}',f'echo A6L_UNLOAD_{m["name"]}=$?']
commands+=['echo A6L_MODULES_AFTER','/system/bin/toybox cat /proc/modules','echo A6L_PERIPHERAL_ABI_DONE']
script=OUT/'test.sh';script.write_text('\n'.join(commands)+'\n')
rc=OUT/'init.rc';rc.write_text((ROOT/'device/hisense/a6l/diagnostic/android-init.rc').read_text()+'''
on early-init
    start peripheralabitest
service peripheralabitest /system/bin/sh /peripheral/test.sh
    user root
    group root
    disabled
    oneshot
    seclabel u:r:su:s0
''')
lines=(ROOT/'firmware/extracted/android-ram-v38-20260917-r2/ramdisk.list').read_text().splitlines()
lines=[f'file /system/etc/init/hw/init.rc {rc} 0644 0 0' if x.startswith('file /system/etc/init/hw/init.rc ') else x for x in lines]
lines+=['dir /peripheral 0755 0 0',f'file /peripheral/test.sh {script} 0755 0 0']
lines += [f'file /peripheral/{m["file"]} {ARCH}/modules/{m["file"]} 0400 0 0' for m in modules]
recipe=OUT/'ramdisk.list';recipe.write_text('\n'.join(lines)+'\n')
(OUT/'ramdisk.gz').write_bytes(gzip.compress(subprocess.check_output([str(BUILD/'usr/gen_init_cpio'),'-t','1789344000',str(recipe)]),mtime=0))
cmd=['qemu-system-aarch64','-machine','virt,gic-version=3','-cpu','cortex-a53','-smp','2','-m','1024','-nodefaults','-nographic','-monitor','none','-serial','stdio','-nic','none','-no-reboot','-kernel',str(kernel),'-initrd',str(OUT/'ramdisk.gz'),'-append','console=ttyAMA0,115200 loglevel=1 panic=0 androidboot.selinux=permissive androidboot.init_rc=/system/etc/init/hw/init.rc']
with (OUT/'console.log').open('wb') as f:
    p=subprocess.Popen(cmd,stdout=f,stderr=subprocess.STDOUT)
    try:
        deadline=time.monotonic()+60
        while time.monotonic()<deadline:
            data=(OUT/'console.log').read_bytes()
            if b'A6L_PERIPHERAL_ABI_DONE' in data or b'Kernel panic' in data or p.poll() is not None:break
            time.sleep(.1)
    finally:
        if p.poll() is None:p.kill();p.wait(timeout=5)
data=(OUT/'console.log').read_text(errors='replace');(ARCH/'qemu-module.log').write_text(data)
checks={}
for m in modules:
    for stage in ['LOAD','UNLOAD']:
        checks[f'{stage}_{m["name"]}']=f'A6L_{stage}_{m["name"]}=0' in data.splitlines()
checks['no_panic']='Kernel panic' not in data
checks['finished']='A6L_PERIPHERAL_ABI_DONE' in data
tail=data.split('A6L_MODULES_AFTER')[-1].split('A6L_PERIPHERAL_ABI_DONE')[0]
checks['modules_removed']=all(not any(line.startswith(m['name']+' ') for line in tail.splitlines()) for m in modules)
report={'passed':all(checks.values()),'checks':checks,'scope':'Real module ABI/dependency load and unload; no physical hardware emulated','command':cmd}
(ARCH/'qemu-module-report.json').write_text(json.dumps(report,indent=2)+'\n')
print('checks',len(checks),'passed',sum(checks.values()),'failed',[k for k,v in checks.items() if not v],flush=True)
assert report['passed']
