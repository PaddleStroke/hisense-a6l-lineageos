"""Diskless ARM tests of the real built Android first-stage init.

Recovery marker skips disk mounts. The V37 RAM logger replaces second-stage
init solely as an exec sentinel: this does NOT test Android policy or services.
No phone access, virtual disks, network interfaces or host-directory shares.
"""
import gzip
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
ANDROID = Path('/home/a6l/android/a6l-lineage24')
OLD = Path('/home/a6l/kernel/out-a6l-baseline-7.2')
NEW = Path('/home/a6l/kernel/out-a6l-android-init')
OUT = Path('/home/a6l/kernel/test-android-first-stage-20260917')
ARCHIVE = ROOT / 'firmware/extracted/android-first-stage-20260917'

def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    global OUT, ARCHIVE
    parser = argparse.ArgumentParser()
    parser.add_argument('--attempt', type=int, default=1)
    attempt = parser.parse_args().attempt
    assert attempt > 0
    if attempt != 1:
        OUT = OUT.with_name(OUT.name + f'-r{attempt}')
        ARCHIVE = ARCHIVE.with_name(ARCHIVE.name + f'-r{attempt}')
    assert 'A6L_ANDROID_INIT_KERNEL_BUILD_SUCCESS' in (NEW / 'build.log').read_text()
    OUT.mkdir(exist_ok=False)
    ARCHIVE.mkdir(exist_ok=False)
    first = ANDROID / 'out/soong/.intermediates/system/core/init/init_first_stage/android_ramdisk_arm64_armv8-a/init'
    sentinel = ROOT / 'firmware/extracted/storage-read-ramdisk-v37-20260917/init'
    assert sha(sentinel) == 'b9cb35bd6f48eb6b6a71bfa3b139c5303b68df327e6329405b480b557df06326'
    shutil.copy2(first, OUT / 'first-stage-init')
    shutil.copy2(sentinel, OUT / 'exec-sentinel')
    (OUT / 'recovery-marker').write_bytes(b'')
    listing = ''.join(f'dir /{p} 0755 0 0\n' for p in ['dev','proc','sys','mnt','system','system/bin',
                                                    'debug_ramdisk','second_stage_resources'])
    listing += f'file /init {OUT / "first-stage-init"} 0755 0 0\n'
    listing += f'file /system/bin/init {OUT / "exec-sentinel"} 0755 0 0\n'
    listing += f'file /system/bin/recovery {OUT / "recovery-marker"} 0400 0 0\n'
    (OUT / 'ramdisk.list').write_text(listing)
    cpio = subprocess.check_output([str(OLD / 'usr/gen_init_cpio'), '-t', '1789344000', str(OUT / 'ramdisk.list')])
    (OUT / 'ramdisk.cpio.gz').write_bytes(gzip.compress(cpio, mtime=0))
    results = []
    for name, build in [('v37-negative-no-selinux', OLD), ('android-init-selinux', NEW)]:
        command = ['qemu-system-aarch64','-machine','virt,gic-version=3','-cpu','cortex-a53',
                   '-smp','2','-m','2048','-nodefaults','-nographic','-monitor','none',
                   '-serial','stdio','-nic','none','-no-reboot',
                   '-kernel',str(build / 'arch/arm64/boot/Image'),
                   '-initrd',str(OUT / 'ramdisk.cpio.gz'),
                   '-append','console=ttyAMA0,115200 earlycon=pl011,0x9000000 loglevel=8 panic=0']
        log = OUT / (name + '.log')
        proc = None
        try:
            with log.open('wb') as stream:
                proc = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 60
                while time.monotonic() < deadline:
                    data = log.read_bytes()
                    if b'A6L_RAM_PROBE_START' in data or b'Kernel panic' in data or proc.poll() is not None:
                        break
                    time.sleep(.25)
        finally:
            if proc and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
        data = log.read_bytes()
        if build == OLD:
            passed = (b'selinuxfs' in data and b'Init encountered errors starting first stage' in data
                      and b'A6L_RAM_PROBE_START' not in data)
        else:
            passed = (b'init first stage started!' in data and b'First stage mount skipped (recovery mode)' in data
                      and b'A6L_RAM_PROBE_START' in data and b'Kernel panic' not in data
                      and b'Init encountered errors' not in data)
        results.append(dict(name=name, passed=passed, command=command, kernel_sha256=sha(build / 'arch/arm64/boot/Image'),
                            log_sha256=sha(log)))
        (ARCHIVE / log.name).write_bytes(log.read_bytes())
    result = dict(passed=all(r['passed'] for r in results), tests=results,
                  first_stage_sha256=sha(first), sentinel_sha256=sha(sentinel),
                  scope='Actual first-stage init and exec handoff only; second-stage is RAM diagnostic sentinel, no SELinux policy or Android service validation')
    (ARCHIVE / 'report.json').write_text(json.dumps(result, indent=2)+'\n')
    (ARCHIVE / 'ramdisk.list').write_bytes((OUT / 'ramdisk.list').read_bytes())
    print(json.dumps(result, indent=2))
    assert result['passed']

if __name__ == '__main__':
    main()
