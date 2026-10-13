"""Package V19 with delayed storage module and exact V18 device tree."""
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OLD = ROOT / 'firmware/extracted/recovery-probe-storage-trace-v20-20260916'
RAM = ROOT / 'firmware/extracted/storage-core-ramdisk-v21-20260916'
TEST = ROOT / 'firmware/extracted/storage-core-probe-v21-20260916'
OUT = ROOT / 'firmware/extracted/recovery-probe-storage-core-v21-20260916'
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
    assert json.loads((ROOT / 'firmware/extracted/storage-core-smoke-v21-20260916/report.json').read_text())['passed']
    kernel = (TEST / 'Image.a6l-offset').read_bytes()
    assert sha(kernel) == test['kernel_sha256'] == '5717b756307399dae8767922963ef592d6661edf2ddb8a457487f3fe06f37643'
    assert struct.unpack_from('<Q', kernel, 8)[0] == 0x80000
    assert b'A6L_USB_CONNECT_HELD' in kernel and b'A6L_USB_EVENT_TRACE armed' in kernel and b'A6L_USB_IRQ_BEGIN' in kernel
    ramdisk = (RAM / 'ramdisk.cpio.gz').read_bytes()
    assert sha(ramdisk) == test['ramdisk_sha256']
    assert json.loads((RAM / 'report.json').read_text())['source_sha256'] == sha((ROOT / 'device/hisense/a6l/diagnostic/init.c').read_bytes())
    original = (OLD / 'recovery-diagnostic-unsigned.img').read_bytes()
    assert sha(original) == 'c1ba3da846fa31cc9a40a8ba6c2aba434d8711d0f67b795469bf65c6180d4392'
    assert json.loads((OLD / 'captured-abl-validation.json').read_text())['passed']
    OUT.mkdir(exist_ok=False)
    base = (OLD / 'base.dtb').read_bytes()
    payload = gzip.compress(kernel, mtime=0) + base
    for name in ['base.dtb', 'overlay.dtbo', 'recovery-dtbo.img']:
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
              'scope': 'V20 DT, command line, RAM init and power settings preserved; finer sleepable SDHCI setup checkpoints share existing64-pause limit; no persistent mounts',
              'remaining': ['Captured bootloader checks', 'Physical trial']}
    (OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
