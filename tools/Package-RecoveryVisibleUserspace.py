#!/usr/bin/env python3
"""Replace only the tested V9 diagnostic ramdisk; no phone access."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OLD = ROOT / 'firmware/extracted/recovery-probe-keep-boot-domains-20260915'
RAM = ROOT / 'firmware/extracted/visible-userspace-ramdisk-20260915'
TEST = ROOT / 'firmware/extracted/visible-userspace-probe-20260915'
OUT = ROOT / 'firmware/extracted/recovery-probe-visible-userspace-20260915'
PACK = Path('/home/a6l/android/a6l-lineage24/system/tools/mkbootimg')
spec = importlib.util.spec_from_file_location('boot', ROOT / 'tools/Verify-BootRoundtrip.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def run(*args):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True).stdout


def main():
    trial = json.loads((TEST / 'report.json').read_text())
    assert trial['passed']
    ramdisk = (RAM / 'ramdisk.cpio.gz').read_bytes()
    assert sha(ramdisk) == trial['ramdisk_sha256']
    assert json.loads((RAM / 'report.json').read_text())['source_sha256'] == sha(
        (ROOT / 'device/hisense/a6l/diagnostic/init.c').read_bytes())
    original = (OLD / 'recovery-diagnostic-unsigned.img').read_bytes()
    assert sha(original) == 'd252fa62e3803c877844d6644200d4b91626b93424352291604c1ca50a02a5a1'
    old_layout = boot.layout(original)
    OUT.mkdir(exist_ok=False)
    raw = run(sys.executable, PACK / 'unpack_bootimg.py', '--boot_img',
              OLD / 'recovery-diagnostic-unsigned.img', '--out', OUT / 'original-parts',
              '--format', 'mkbootimg', '-0').split(b'\0')
    if not raw[-1]:
        raw.pop()
    args = [a.decode() for a in raw]
    (OUT / 'ramdisk.cpio.gz').write_bytes(ramdisk)
    args[args.index('--ramdisk') + 1] = str(OUT / 'ramdisk.cpio.gz')
    body_path = OUT / 'recovery-diagnostic-body.img'
    run(sys.executable, PACK / 'mkbootimg.py', *args, '--output', body_path)
    body = bytearray(body_path.read_bytes())
    body[28:32] = original[28:32]
    body_path.write_bytes(body)
    layout = boot.layout(body)
    assert layout['body_end'] == len(body) < len(original) == 67108864
    assert layout['sections']['ramdisk']['sha256'] == sha(ramdisk)
    for name in ('kernel', 'recovery_dtbo'):
        before, after = old_layout['sections'][name], layout['sections'][name]
        assert original[before['offset']:before['offset'] + before['bytes']] == body[
            after['offset']:after['offset'] + after['bytes']]
    old_header, new_header = bytearray(original[:4096]), bytearray(body[:4096])
    # Only ramdisk size, payload digest, and following DTBO file offset vary.
    for start, end in ((16, 20), (576, 608), (1636, 1644)):
        old_header[start:end] = new_header[start:end] = bytes(end-start)
    assert old_header == new_header
    for name in ('base.dtb', 'overlay.dtbo', 'Image.gz-dtb', 'recovery-dtbo.img'):
        (OUT / name).write_bytes((OLD / name).read_bytes())
    candidate = bytes(body) + bytes(len(original)-len(body))
    (OUT / 'recovery-diagnostic-unsigned.img').write_bytes(candidate)
    raw = run(sys.executable, PACK / 'unpack_bootimg.py', '--boot_img', body_path,
              '--out', OUT / 'roundtrip-parts', '--format', 'mkbootimg', '-0').split(b'\0')
    if not raw[-1]:
        raw.pop()
    run(sys.executable, PACK / 'mkbootimg.py', *[a.decode() for a in raw],
        '--output', OUT / 'roundtrip.img')
    roundtrip = bytearray((OUT / 'roundtrip.img').read_bytes())
    roundtrip[28:32] = body[28:32]
    assert roundtrip == body
    report = dict(packaging_passed=True, ready_to_flash=False,
                  scope='Only RAM diagnostic replacement; kernel, DT, command line and load addresses unchanged from V9',
                  candidate_sha256=sha(candidate), ramdisk_sha256=sha(ramdisk),
                  kernel_sha256=trial['kernel_sha256'], layout=layout,
                  command_line=args[args.index('--cmdline') + 1],
                  remaining=['Captured ABL checks', 'Guarded physical installation and trial'])
    (OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
