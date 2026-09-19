"""Exercise actual Android SELinux setup, second-stage init and debug services."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser()
ap.add_argument('--attempt',type=int,required=True)
n=ap.parse_args().attempt
ram=ROOT/f'firmware/extracted/android-ram-v38-20260917-r{n}'
out=Path(f'/home/a6l/kernel/test-android-ram-v38-r{n}')
out.mkdir(exist_ok=False)
log=out/'console.log'
kernel=ROOT/'firmware/extracted/android-init-kernel-20260917/Image'
args=['qemu-system-aarch64','-machine','virt,gic-version=3','-cpu','cortex-a53','-smp','2','-m','2048',
      '-nodefaults','-nographic','-monitor','none','-serial','stdio','-nic','none','-no-reboot',
      '-kernel',str(kernel),'-initrd',str(ram/'ramdisk.cpio.gz'),
      '-append','console=ttyAMA0,115200 earlycon=pl011,0x9000000 loglevel=8 panic=0 androidboot.selinux=permissive androidboot.init_rc=/system/etc/init/hw/init.rc']
proc=None
try:
    with log.open('wb') as f:
        proc=subprocess.Popen(args,stdout=f,stderr=subprocess.STDOUT)
        deadline=time.monotonic()+55
        while time.monotonic()<deadline:
            data=log.read_bytes()
            if (b'A6L_RAM_PROBE_ALIVE seconds=10 ' in data or b'Kernel panic' in data or proc.poll() is not None): break
            time.sleep(.25)
finally:
    if proc and proc.poll() is None:
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill();proc.wait()
data=log.read_bytes()
checks={
 'first_stage':b'init first stage started!' in data,
 'second_stage':b'init second stage started!' in data,
 'policy_loaded':b'Loading SELinux policy' in data and b'init second stage started!' in data,
 'service_started':b'A6L_ANDROID_SERVICE_START' in data,
 'property_service':b'A6L_ANDROID_PROPERTY ramdiag=v38' in data,
 'adbd_running':b'A6L_ANDROID_ADBD state=running' in data,
 'alive':b'A6L_RAM_PROBE_ALIVE seconds=10 ' in data,
 'no_panic':b'Kernel panic' not in data,
}
report={'passed':all(checks.values()),'checks':checks,'command':args,
        'console_sha256':hashlib.sha256(data).hexdigest(),
        'scope':'Diskless QEMU actual Android init+policy+service startup; USB enumeration/storage hardware require physical test'}
(ram/'qemu-console.log').write_bytes(data)
(ram/'qemu-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
if not report['passed']: print(data[-6500:].decode(errors='replace'))
assert report['passed']
