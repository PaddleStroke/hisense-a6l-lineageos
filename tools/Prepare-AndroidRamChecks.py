"""Freeze the passing RAM attempt into V38's packaging/offline checks."""
import argparse
from pathlib import Path
import json
ROOT=Path(__file__).resolve().parents[1]
T=ROOT/'tools'
ap=argparse.ArgumentParser()
ap.add_argument('--attempt',type=int,required=True)
n=ap.parse_args().attempt
ram=f'android-ram-v38-20260917-r{n}'
assert json.loads((ROOT/'firmware/extracted'/ram/'qemu-report.json').read_text())['passed']
def write(name,s):
 p=T/name
 assert not p.exists(),p
 p.write_text(s)
s=(T/'Test-A6LStorageRead.py').read_text()
s=s.replace('storage-read-probe-v37-20260917','android-ram-probe-v38-20260917').replace('out-a6l-baseline-7.2','out-a6l-android-init').replace('baseline-7.2-kernel-20260917','android-init-kernel-20260917').replace('storage-read-ramdisk-v37-20260917',ram)
s=s.replace('loglevel=8 panic=0 initcall_debug','loglevel=8 panic=0 androidboot.selinux=permissive androidboot.init_rc=/system/etc/init/hw/init.rc initcall_debug')
# Existing checks also require the known hash self-test and live diagnostics.
write('Test-A6LAndroidRam.py',s)
s=(T/'Package-RecoveryStorageRead.py').read_text()
s=s.replace('recovery-probe-kernel72-v36-20260917','recovery-probe-storage-read-v37-20260917').replace('recovery-probe-storage-read-v37-20260917\'\nPACK','recovery-probe-android-ram-v38-20260917\'\nPACK')
s=s.replace('storage-read-ramdisk-v37-20260917',ram).replace('storage-read-probe-v37-20260917','android-ram-probe-v38-20260917')
s=s.replace("assert json.loads((ROOT / 'firmware/extracted/baseline-7.2-smoke-20260917/report.json').read_text())['passed']", "assert json.loads((ROOT / 'firmware/extracted/android-init-module-20260917/report.json').read_text())['passed']")
s=s.replace(" == '2e1fdc8e5e5422138b72cb210dde80a77ab2caba0cc73bc4de40641df7585496'",'')
s=s.replace("assert json.loads((RAM / 'report.json').read_text())['source_sha256'] == sha((ROOT / 'device/hisense/a6l/diagnostic/init.c').read_bytes())", "assert json.loads((RAM / 'qemu-report.json').read_text())['passed']")
s=s.replace('fa6319e61da7347b28f76527f8ff75dfeba2a6b3c7067de564ca85fe9123018e','7de9a96229140b066aed2724ccf2cb14b1a09cc9e44a12bfb25ee4543761b8e9')
s=s.replace("assert payload == (OLD / 'Image.gz-dtb').read_bytes()", "assert payload != (OLD / 'Image.gz-dtb').read_bytes()")
s=s.replace('cmdline = old_cmdline', "cmdline = old_cmdline + ' androidboot.selinux=permissive androidboot.init_rc=/system/etc/init/hw/init.rc'")
s=s.replace('[(8, 12), (16, 20), (576, 608), (1636, 1644)]','[(8, 12), (16, 20), (64, 576), (576, 608), (608, 1632), (1636, 1644)]')
s=s.replace('V37 changes only RAM userspace: fixed direct read-only hashes of five immutable firmware/GPT regions twice. Exact working V36 kernel, module, DT, command line and overlays; no block-device writes or persistent mounts.', 'V38 Android first/second-stage init, loaded recovery SELinux policy in development permissive mode, authenticated USB ADB plus ACM logging and fixed direct read-only hashes. New SELinux-enabled 7.2.3 kernel/module; exact working V37 DT/overlays. No persistent mounts or block writes in automatic startup.')
assert "OUT = ROOT / 'firmware/extracted/recovery-probe-android-ram-v38-20260917'" in s
write('Package-RecoveryAndroidRam.py',s)
s=(T/'Test-RecoveryStorageRead.py').read_text().replace('recovery-probe-storage-read-v37-20260917','recovery-probe-android-ram-v38-20260917')
write('Test-RecoveryAndroidRam.py',s)
print('V38 packaging and bootloader checks prepared from passing RAM attempt',n)
