#!/usr/bin/env python3
"""Offline diagnostic variant: change only the eMMC DT status from V11."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import zlib
from a6l_fdt import read_fdt

ROOT = Path(__file__).resolve().parents[1]
OLD = ROOT / 'firmware/extracted/recovery-probe-regulator-constraints-20260916'
OUT = ROOT / 'firmware/extracted/recovery-probe-usb-only-20260916'
PACK = Path('/home/a6l/android/a6l-lineage24/system/tools/mkbootimg')
spec = importlib.util.spec_from_file_location('boot', ROOT / 'tools/Verify-BootRoundtrip.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)
def sha(data):
    return hashlib.sha256(data).hexdigest()
def run(*args):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True).stdout
def unpack_args(image, directory):
    values = run(sys.executable, PACK / 'unpack_bootimg.py', '--boot_img', image, '--out', directory, '--format', 'mkbootimg', '-0').split(b'\0')
    if values[-1] == b'':
        values.pop()
    return [a.decode() for a in values]

def main():
    original = (OLD / 'recovery-diagnostic-unsigned.img').read_bytes()
    assert sha(original) == '54ec8ad057bbb4bd52c63ad2c82ee68efd0a4828ff674cfdc51acbb41a12259a'
    assert json.loads((OLD / 'captured-abl-validation.json').read_text())['passed']
    old_base = (OLD / 'base.dtb').read_bytes()
    base = Path('/home/a6l/kernel/out-a6l-probe/arch/arm64/boot/dts/qcom/sdm660-hisense-a6l-usb-only.dtb').read_bytes()
    before, after = read_fdt(old_base), read_fdt(base)
    assert before.keys() == after.keys()
    changes = [(n, k) for n in before for k in set(before[n]) | set(after[n]) if before[n].get(k) != after[n].get(k)]
    target = ('/soc@0/mmc@c0c4000', 'status')
    assert changes == [target], changes
    assert before[target[0]][target[1]] == b'okay\0'
    assert after[target[0]][target[1]] == b'disabled\0'
    old_payload = (OLD / 'Image.gz-dtb').read_bytes()
    dec = zlib.decompressobj(31)
    kernel = dec.decompress(old_payload)
    assert dec.eof and dec.unused_data == old_base
    assert sha(kernel) == '4cc5b6b159bdcef531a4e2916d2e4b7a40598e666f11b61625901d8f41a62ca8'
    ramdisk = (OLD / 'ramdisk.cpio.gz').read_bytes()
    assert sha(ramdisk) == 'adc5b3effef201c73725d61ac7c0afd920a1e50394404dea0e2863f46b0da774'
    payload = old_payload[:-len(old_base)] + base
    OUT.mkdir(exist_ok=False)
    for name, data in [('base.dtb', base), ('Image.gz-dtb', payload), ('ramdisk.cpio.gz', ramdisk)]:
        (OUT / name).write_bytes(data)
    for name in ['overlay.dtbo', 'recovery-dtbo.img']:
        (OUT / name).write_bytes((OLD / name).read_bytes())
    args = unpack_args(OLD / 'recovery-diagnostic-unsigned.img', OUT / 'original-parts')
    for name in ['kernel', 'ramdisk', 'recovery_dtbo']:
        filename = {'kernel': 'Image.gz-dtb', 'ramdisk': 'ramdisk.cpio.gz', 'recovery_dtbo': 'recovery-dtbo.img'}[name]
        args[args.index('--' + name) + 1] = str(OUT / filename)
    body_path = OUT / 'recovery-diagnostic-body.img'
    run(sys.executable, PACK / 'mkbootimg.py', *args, '--output', body_path)
    body = bytearray(body_path.read_bytes())
    body[28:32] = original[28:32]
    body_path.write_bytes(body)
    layout = boot.layout(body)
    assert layout['body_end'] == len(body) < len(original) == 67108864
    for section, data in [('kernel', payload), ('ramdisk', ramdisk), ('recovery_dtbo', (OLD / 'recovery-dtbo.img').read_bytes())]:
        assert layout['sections'][section]['sha256'] == sha(data)
    old_header, new_header = bytearray(original[:4096]), bytearray(body[:4096])
    # Only kernel payload length, payload digest and embedded overlay offset vary.
    for start, end in [(8, 12), (576, 608), (1636, 1644)]:
        old_header[start:end] = new_header[start:end] = bytes(end-start)
    assert old_header == new_header
    roundtrip_args = unpack_args(body_path, OUT / 'roundtrip-parts')
    run(sys.executable, PACK / 'mkbootimg.py', *roundtrip_args, '--output', OUT / 'roundtrip.img')
    roundtrip = bytearray((OUT / 'roundtrip.img').read_bytes())
    roundtrip[28:32] = body[28:32]
    assert roundtrip == body
    candidate = bytes(body) + bytes(len(original) - len(body))
    (OUT / 'recovery-diagnostic-unsigned.img').write_bytes(candidate)
    report = dict(packaging_passed=True, ready_to_flash=False,
                  scope='V11 with eMMC controller disabled; identical kernel, RAM filesystem, USB/regulator wiring, overlay, command line and load addresses',
                  device_tree_changes=changes, candidate_sha256=sha(candidate),
                  kernel_sha256=sha(kernel), ramdisk_sha256=sha(ramdisk), layout=layout,
                  command_line=args[args.index('--cmdline')+1],
                  limits='Isolation test only; does not establish the prior shutdown cause. No physical test yet.')
    (OUT / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({'packaging_passed': True, 'candidate_sha256': sha(candidate), 'device_tree_changes': changes}))

if __name__ == '__main__':
    main()
