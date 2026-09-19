#!/usr/bin/env python3
"""Package the tested regulator constraint and logging corrections; no phone access."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import struct
import zlib
from a6l_fdt import read_fdt

ROOT = Path(__file__).resolve().parents[1]
OLD = ROOT / 'firmware/extracted/recovery-probe-visible-userspace-20260915'
RAM = ROOT / 'firmware/extracted/regulator-ramdisk-20260916'
TEST = ROOT / 'firmware/extracted/regulator-probe-20260916'
OUT = ROOT / 'firmware/extracted/recovery-probe-regulator-constraints-20260916'
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
    assert sha(original) == '19027fa8401d368783e181376494a60761bd6805fb64688af970ccba6c3368c2'
    old_layout = boot.layout(original)
    assert json.loads((ROOT / 'firmware/extracted/regulator-constraints-20260916/report.json').read_text())['passed']
    old_base = (OLD / 'base.dtb').read_bytes()
    base = Path('/home/a6l/kernel/out-a6l-probe/arch/arm64/boot/dts/qcom/sdm660-hisense-a6l-recovery.dtb').read_bytes()
    before, after = read_fdt(old_base), read_fdt(base)
    assert before.keys() == after.keys() and len(old_base) == len(base)
    changes = [(n, k) for n in before for k in set(before[n]) | set(after[n]) if before[n].get(k) != after[n].get(k)]
    target = ('/remoteproc/glink-edge/rpm-requests/regulators-0/l4', 'regulator-min-microvolt')
    assert changes == [target], changes
    assert before[target[0]][target[1]] == struct.pack('>I', 2950000)
    assert after[target[0]][target[1]] == struct.pack('>I', 2944000)
    old_payload = (OLD / 'Image.gz-dtb').read_bytes()
    dec = zlib.decompressobj(31)
    kernel = dec.decompress(old_payload)
    assert dec.eof and dec.unused_data == old_base
    assert sha(kernel) == trial['kernel_sha256'] == '4cc5b6b159bdcef531a4e2916d2e4b7a40598e666f11b61625901d8f41a62ca8'
    payload = old_payload[:-len(old_base)] + base
    assert len(payload) == len(old_payload)
    OUT.mkdir(exist_ok=False)
    (OUT / 'base.dtb').write_bytes(base)
    (OUT / 'Image.gz-dtb').write_bytes(payload)
    raw = run(sys.executable, PACK / 'unpack_bootimg.py', '--boot_img',
              OLD / 'recovery-diagnostic-unsigned.img', '--out', OUT / 'original-parts',
              '--format', 'mkbootimg', '-0').split(b'\0')
    if not raw[-1]:
        raw.pop()
    args = [a.decode() for a in raw]
    (OUT / 'ramdisk.cpio.gz').write_bytes(ramdisk)
    args[args.index('--ramdisk') + 1] = str(OUT / 'ramdisk.cpio.gz')
    args[args.index('--kernel') + 1] = str(OUT / 'Image.gz-dtb')
    body_path = OUT / 'recovery-diagnostic-body.img'
    run(sys.executable, PACK / 'mkbootimg.py', *args, '--output', body_path)
    body = bytearray(body_path.read_bytes())
    body[28:32] = original[28:32]
    body_path.write_bytes(body)
    layout = boot.layout(body)
    assert layout['body_end'] == len(body) < len(original) == 67108864
    assert layout['sections']['ramdisk']['sha256'] == sha(ramdisk)
    assert layout['sections']['kernel']['sha256'] == sha(payload)
    for name in ('recovery_dtbo',):
        before, after = old_layout['sections'][name], layout['sections'][name]
        assert original[before['offset']:before['offset'] + before['bytes']] == body[
            after['offset']:after['offset'] + after['bytes']]
    old_header, new_header = bytearray(original[:4096]), bytearray(body[:4096])
    # Only ramdisk size, payload digest, and following DTBO file offset vary.
    for start, end in ((16, 20), (576, 608), (1636, 1644)):
        old_header[start:end] = new_header[start:end] = bytes(end-start)
    assert old_header == new_header
    for name in ('overlay.dtbo', 'recovery-dtbo.img'):
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
                  scope='One DT minimum-voltage correction and RAM logging newline fix; kernel, overlay, command line and load addresses unchanged from V10',
                  device_tree_changes=changes,
                  candidate_sha256=sha(candidate), ramdisk_sha256=sha(ramdisk),
                  kernel_sha256=trial['kernel_sha256'], layout=layout,
                  command_line=args[args.index('--cmdline') + 1],
                  remaining=['Captured ABL checks', 'Guarded physical installation and trial'])
    (OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
