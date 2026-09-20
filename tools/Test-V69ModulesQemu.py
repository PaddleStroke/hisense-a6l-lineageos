"""insmod every V69 bundle module, in bundle order, on the V67 phone kernel in diskless QEMU (ABI/dependency check only)."""
import gzip,json,subprocess,time,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];B=ROOT/('firmware/extracted/'+(sys.argv[2] if len(sys.argv)>2 else 'v69-attended-bundle-20260921'))
OUT=Path(f'/home/a6l/kernel/modules-qemu-r{sys.argv[1]}');OUT.mkdir(exist_ok=False)
areas=[d.name for d in sorted(B.iterdir()) if (d/'modules/order.txt').exists()]
script=OUT/'test.sh';script.write_text('#!/system/bin/sh\n/system/bin/toybox mknod /dev/mc c 204 64\nexec > /dev/mc 2>&1\n'+''.join(
 f'for m in $(/system/bin/toybox cat /mods/{a}/order.txt); do n=$(echo ${{m%.ko}} | /system/bin/toybox tr - _); /system/bin/toybox grep -q "^$n " /proc/modules && continue; /system/bin/toybox insmod /mods/{a}/$m && echo A6L_MOD_OK {a} $m || echo A6L_MOD_FAIL {a} $m; done\n' for a in areas)+'echo A6L_MOD_DONE\n')
rc=OUT/'init.rc';rc.write_text((ROOT/'device/hisense/a6l/diagnostic/android-init.rc').read_text()+'\non early-init\n    start modtest\nservice modtest /system/bin/sh /mods/test.sh\n    user root\n    group root\n    disabled\n    oneshot\n    seclabel u:r:su:s0\n')
lines=(ROOT/'firmware/extracted/android-ram-v38-20260917-r2/ramdisk.list').read_text().splitlines()
lines=[f'file /system/etc/init/hw/init.rc {rc} 0644 0 0' if x.startswith('file /system/etc/init/hw/init.rc ') else x for x in lines]
lines+=['dir /mods 0755 0 0',f'file /mods/test.sh {script} 0755 0 0']
for a in areas:
    lines.append(f'dir /mods/{a} 0755 0 0')
    for f in sorted((B/a/'modules').iterdir()):lines.append(f'file /mods/{a}/{f.name} {f} 0644 0 0')
(OUT/'ramdisk.list').write_text('\n'.join(lines)+'\n')
(OUT/'ramdisk.gz').write_bytes(gzip.compress(subprocess.check_output(['/home/a6l/kernel/out-a6l-android-init/usr/gen_init_cpio','-t','1789344000',str(OUT/'ramdisk.list')]),mtime=0))
cmd=['qemu-system-aarch64','-machine','virt,gic-version=3','-cpu','cortex-a53','-smp','2','-m','1024','-nodefaults','-nographic','-monitor','none','-serial','stdio','-nic','none','-no-reboot','-kernel',str(ROOT/'firmware/extracted/phone-kernel-v67-candidate-20260919/Image'),'-initrd',str(OUT/'ramdisk.gz'),'-append','console=ttyAMA0,115200 loglevel=4 panic=0 androidboot.selinux=permissive']
with (OUT/'console.log').open('wb') as f:
    p=subprocess.Popen(cmd,stdout=f,stderr=subprocess.STDOUT);d=time.monotonic()+90
    while time.monotonic()<d and b'A6L_MOD_DONE' not in (OUT/'console.log').read_bytes() and p.poll() is None:time.sleep(.5)
    p.kill()
log=(OUT/'console.log').read_text(errors='replace')
ok=[l for l in log.splitlines() if 'A6L_MOD_OK' in l];bad=[l for l in log.splitlines() if 'A6L_MOD_FAIL' in l or 'Unknown symbol' in l or 'disagrees' in l]
report={'passed':'A6L_MOD_DONE' in log and not bad and 'Kernel panic' not in log,'loaded':len(ok),'failures':bad[:20]}
(B/'qemu-module-report.json').write_text(json.dumps(report,indent=1)+'\n');print(json.dumps(report,indent=1))
