"""V37: read-only storage hash validation on the exact working V36 kernel."""
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
from a6l_fdt import read_fdt

ROOT = Path(__file__).resolve().parents[1]
OLD = ROOT / 'firmware/extracted/recovery-probe-kernel72-v36-20260917'
KERNEL = ROOT / 'firmware/extracted/baseline-7.2-kernel-20260917'
RAM = ROOT / 'firmware/extracted/storage-read-ramdisk-v37-20260917'
TEST = ROOT / 'firmware/extracted/storage-read-probe-v37-20260917'
OUT = ROOT / 'firmware/extracted/recovery-probe-storage-read-v37-20260917'
PACK = Path('/home/a6l/android/a6l-lineage24/system/tools/mkbootimg')
spec = importlib.util.spec_from_file_location('boot', ROOT / 'tools/Verify-BootRoundtrip.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)

def sha(data):
    return hashlib.sha256(data).hexdigest()

def run(*args):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True).stdout

def unpack_args(image, target):
    raw = run(sys.executable, PACK / 'unpack_bootimg.py', '--boot_img', image,
              '--out', target, '--format', 'mkbootimg', '-0').split(b'\0')
    assert raw.pop() == b''
    return [v.decode() for v in raw]

def main():
    test = json.loads((TEST / 'report.json').read_text())
    assert test['passed']
    assert json.loads((ROOT / 'firmware/extracted/baseline-7.2-smoke-20260917/report.json').read_text())['passed']
    kernel = (TEST / 'Image.a6l-offset').read_bytes()
    assert sha(kernel) == test['kernel_sha256'] == '2e1fdc8e5e5422138b72cb210dde80a77ab2caba0cc73bc4de40641df7585496'
    assert struct.unpack_from('<Q', kernel, 8)[0] == 0x80000
    assert b'A6L_USB_CONNECT_HELD' in kernel and b'A6L_CMD0_CALLBACK' not in kernel
    assert b'Linux version 7.2.3-a6l-probe' in kernel
    ramdisk = (RAM / 'ramdisk.cpio.gz').read_bytes()
    assert sha(ramdisk) == test['ramdisk_sha256']
    assert json.loads((RAM / 'report.json').read_text())['source_sha256'] == sha((ROOT / 'device/hisense/a6l/diagnostic/init.c').read_bytes())
    original = (OLD / 'recovery-diagnostic-unsigned.img').read_bytes()
    assert sha(original) == 'fa6319e61da7347b28f76527f8ff75dfeba2a6b3c7067de564ca85fe9123018e'
    assert json.loads((OLD / 'captured-abl-validation.json').read_text())['passed']
    OUT.mkdir(exist_ok=False)
    before = (KERNEL / 'base.dtb').read_bytes()
    audit = json.loads((KERNEL / 'device-tree-audit.json').read_text())
    assert audit['passed'] and sha(before) == audit['base_sha256']
    expected = read_fdt(before)
    (OUT / 'base.dtb').write_bytes(before)
    nodes = ['/remoteproc/glink-edge/rpm-requests/regulators-0/l4',
             '/remoteproc/glink-edge/rpm-requests/regulators-1/l8']
    for node in nodes:
        assert 'regulator-allow-set-load' not in expected[node]
        run('fdtput', '-t', 's', OUT / 'base.dtb', node, 'regulator-allow-set-load')
        expected[node]['regulator-allow-set-load'] = b''
    base = (OUT / 'base.dtb').read_bytes()
    assert read_fdt(base) == expected, 'Unexpected change beyond two eMMC load permissions'
    assert base == (OLD / 'base.dtb').read_bytes(), 'V37 must preserve the exact working V36 DT'
    payload = gzip.compress(kernel, mtime=0) + base
    assert payload == (OLD / 'Image.gz-dtb').read_bytes()
    for name in ['overlay.dtbo', 'recovery-dtbo.img']:
        (OUT / name).write_bytes((OLD / name).read_bytes())
    (OUT / 'Image.gz-dtb').write_bytes(payload)
    (OUT / 'ramdisk.cpio.gz').write_bytes(ramdisk)
    args = unpack_args(OLD / 'recovery-diagnostic-unsigned.img', OUT / 'original-parts')
    old_cmdline = args[args.index('--cmdline') + 1]
    assert 'a6l_manual_usb=1' in old_cmdline and 'a6l_usb_trace=1' in old_cmdline
    assert 'a6l_storage_trace=1' in old_cmdline
    cmdline = old_cmdline
    args[args.index('--cmdline') + 1] = cmdline
    for name, filename in [('kernel', 'Image.gz-dtb'), ('ramdisk', 'ramdisk.cpio.gz'), ('recovery_dtbo', 'recovery-dtbo.img')]:
        args[args.index('--' + name) + 1] = str(OUT / filename)
    body_path = OUT / 'recovery-diagnostic-body.img'
    run(sys.executable, PACK / 'mkbootimg.py', *args, '--output', body_path)
    body = bytearray(body_path.read_bytes())
    body[28:32] = original[28:32]
    body_path.write_bytes(body)
    layout = boot.layout(body)
    assert layout['body_end'] == len(body) < len(original) == 67108864
    for name, value in [('kernel', payload), ('ramdisk', ramdisk), ('recovery_dtbo', (OLD / 'recovery-dtbo.img').read_bytes())]:
        assert layout['sections'][name]['sha256'] == sha(value)
    before, after = bytearray(original[:4096]), bytearray(body[:4096])
    # Only payload sizes/digest, relocated DTBO offset and the explicit boot flag.
    for start, end in [(8, 12), (16, 20), (576, 608), (1636, 1644)]:
        before[start:end] = after[start:end] = bytes(end-start)
    assert before == after
    rebuilt_args = unpack_args(body_path, OUT / 'roundtrip-parts')
    assert rebuilt_args[rebuilt_args.index('--cmdline') + 1] == cmdline
    run(sys.executable, PACK / 'mkbootimg.py', *rebuilt_args, '--output', OUT / 'roundtrip.img')
    rebuilt = bytearray((OUT / 'roundtrip.img').read_bytes())
    rebuilt[28:32] = body[28:32]
    assert rebuilt == body
    candidate = bytes(body) + bytes(len(original)-len(body))
    (OUT / 'recovery-diagnostic-unsigned.img').write_bytes(candidate)
    report = {'packaging_passed': True, 'ready_to_flash': False,
              'candidate_sha256': sha(candidate), 'kernel_sha256': sha(kernel),
              'ramdisk_sha256': sha(ramdisk), 'layout': layout, 'command_line': cmdline,
              'scope': 'V37 changes only RAM userspace: fixed direct read-only hashes of five immutable firmware/GPT regions twice. Exact working V36 kernel, module, DT, command line and overlays; no block-device writes or persistent mounts.',
              'base_sha256': sha(base), 'source_revision': 'e47d622cb6d2440a9eacdc8bb2df32c037bec7b8',
              'remaining': ['Captured bootloader checks', 'Physical trial']}
    (OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
