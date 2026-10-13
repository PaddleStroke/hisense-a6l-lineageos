"""Diskless QEMU Android Binder transactions and absent-eMMC fail-closed check."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--attempt',type=int,required=True);n=p.parse_args().attempt
out=ROOT/f'firmware/extracted/android-integration-v39-20260917-r{n}'
log=Path(f'/home/a6l/kernel/android-integration-v39-r{n}/console.log')
assert not log.exists()
args=['qemu-system-aarch64','-machine','virt,gic-version=3','-cpu','cortex-a53','-smp','2','-m','2048','-nodefaults','-nographic','-monitor','none','-serial','stdio','-nic','none','-no-reboot',
      '-kernel',str(ROOT/'firmware/extracted/android-init-kernel-20260917/Image'),'-initrd',str(out/'qemu-ramdisk.cpio.gz'),
      '-append','console=ttyAMA0,115200 earlycon=pl011,0x9000000 loglevel=8 panic=0 androidboot.selinux=permissive androidboot.init_rc=/system/etc/init/hw/init.rc']
with log.open('wb') as f:
    proc=subprocess.Popen(args,stdout=f,stderr=subprocess.STDOUT)
    try:
        deadline=time.monotonic()+50
        while time.monotonic()<deadline:
            data=log.read_bytes()
            if b'A6L_QEMU_INTEGRATION_FINISHED' in data or b'Kernel panic' in data or proc.poll() is not None:break
            time.sleep(.25)
    finally:
        if proc.poll() is None:proc.terminate()
        try:proc.wait(timeout=3)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
data=log.read_bytes()
checks={'binder_transaction':b'A6L_BINDER_PASS checkService=1 pingBinder=1 private_namespace=1' in data,
        'binder_exit_zero':b'A6L_QEMU_BINDER_EXIT=0' in data,
        'missing_emmc_rejected':b'A6L_QEMU_NO_EMMC_EXIT=1' in data and b'A6L_INTEGRATION_FAIL /sys/block/mmcblk1/device/name' in data,
        'no_filesystem_mounts':b'A6L_FS_MOUNT' not in data,'no_panic':b'Kernel panic' not in data}
report={'passed':all(checks.values()),'checks':checks,'command':args,'console_sha256':hashlib.sha256(data).hexdigest(),
        'scope':'Actual Android recovery servicemanager plus separate Binder client, same physical V38 kernel; absent eMMC rejects storage path. No physical filesystem proof.'}
(out/'qemu-console.log').write_bytes(data);(out/'qemu-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
if not report['passed']:print(data[-8500:].decode(errors='replace'))
assert report['passed']
