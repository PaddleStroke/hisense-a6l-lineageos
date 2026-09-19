#!/usr/bin/env python3
"""Package the symbol/fixup compatibility correction without rebuilding Linux."""
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zlib
from a6l_fdt import read_fdt

ROOT = Path(__file__).resolve().parents[1]
OLD = ROOT / 'firmware/extracted/recovery-probe-20260914'
OUT = ROOT / 'firmware/extracted/recovery-probe-earlycon-20260915'
EARLY = ROOT / 'firmware/extracted/earlycon-probe-20260915'
DT = Path('/home/a6l/kernel/out-a6l-probe/arch/arm64/boot/dts/qcom')
PACK = Path('/home/a6l/android/a6l-lineage24/system/tools/mkbootimg')
spec = importlib.util.spec_from_file_location('roundtrip', ROOT / 'tools/Verify-BootRoundtrip.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def run(*args):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True).stdout


def main():
    assert json.loads((EARLY / 'report.json').read_text())['passed']
    OUT.mkdir(exist_ok=False)
    original = (OLD / 'recovery-diagnostic-unsigned.img').read_bytes()
    assert sha(original) == 'ff3c54525d12a7f5d479fdc36cfd6ab0e6e103abcb9e95d398bac9ca58a27a7b'
    stock = (OLD / 'restore-stock-recovery.img').read_bytes()
    assert sha(stock) == '9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621'
    base = (DT / 'sdm660-hisense-a6l-recovery.dtb').read_bytes()
    overlay = (DT / 'a6l-recovery-overlay.dtbo').read_bytes()
    before, after = read_fdt((OLD / 'base.dtb').read_bytes()), read_fdt(base)
    assert set(after) - set(before) == {'/__symbols__'} and not set(before) - set(after)
    # -@ may add phandles for labels formerly unused, but existing references
    # and every hardware property must stay byte-for-byte identical.
    added_handles = []
    for node, props in before.items():
        for key in set(props) | set(after[node]):
            if props.get(key) != after[node].get(key):
                assert key == 'phandle' and key not in props, (node, key)
                added_handles.append(node)
    handles = [p['phandle'] for p in after.values() if 'phandle' in p]
    assert len(handles) == len(set(handles))
    assert after['/__symbols__']['a6l_recovery_chosen'] == b'/chosen\0'
    for value in after['/__symbols__'].values():
        assert value.rstrip(b'\0').decode() in after
    ov = read_fdt(overlay)
    oldov = read_fdt((OLD / 'overlay.dtbo').read_bytes())
    assert ov['/'] == {k: oldov['/'][k] for k in ('qcom,board-id', 'qcom,pmic-id')}
    assert set(ov) == {'/', '/fragment@0', '/fragment@0/__overlay__', '/__fixups__'}
    assert ov['/fragment@0'] == {'target': b'\xff' * 4}
    assert ov['/__fixups__'] == {'a6l_recovery_chosen': b'/fragment@0:target:0\0'}
    assert ov['/fragment@0/__overlay__'] == {'hisense,a6l-recovery-probe': b'v1\0'}
    (OUT / 'base.dtb').write_bytes(base)
    (OUT / 'overlay.dtbo').write_bytes(overlay)
    payload = (OLD / 'Image.gz-dtb').read_bytes()
    dec = zlib.decompressobj(31)
    kernel = dec.decompress(payload)
    assert dec.eof and sha(kernel) == 'bce9e6d3b330bb4a77cbd30b5bff71f1c8b5ba94f71417b8e0a495425079fb43'
    assert dec.unused_data == (OLD / 'base.dtb').read_bytes()
    kernel = (EARLY / 'Image.a6l-offset').read_bytes()
    assert sha(kernel) == json.loads((EARLY / 'report.json').read_text())['kernel_sha256']
    compressed = gzip.compress(kernel, mtime=0)
    (OUT / 'Image.gz-dtb').write_bytes(compressed + base)
    ramdisk = (OLD / 'ramdisk.cpio.gz').read_bytes()
    assert sha(ramdisk) == '14141431a5e57a4510159365eddddc5322aceb271272787a5282ee27f2a9d983'
    (OUT / 'ramdisk.cpio.gz').write_bytes(ramdisk)
    table = struct.pack('>8I', 0xd7b7ab1e, 64 + len(overlay), 32, 32, 1, 32, 4096, 0)
    table += struct.pack('>8I', len(overlay), 64, 0, 0, 0, 0, 0, 0) + overlay
    (OUT / 'recovery-dtbo.img').write_bytes(table)
    args = json.loads((ROOT / 'firmware/extracted/boot-roundtrip-20260914-v4/recovery/mkbootimg-arguments.json').read_text())
    cmdline = json.loads((OLD / 'report.json').read_text())['command_line']
    cmdline = cmdline.replace('earlycon=msm_serial_dm,0xc170000', 'earlycon=a6lfb')
    cmdline = cmdline.replace('panic=15', 'panic=0') + ' keep_bootcon initcall_debug'
    for key, value in {'--kernel': OUT / 'Image.gz-dtb', '--ramdisk': OUT / 'ramdisk.cpio.gz',
                       '--recovery_dtbo': OUT / 'recovery-dtbo.img', '--cmdline': cmdline}.items():
        args[args.index(key) + 1] = str(value)
    body_path = OUT / 'recovery-diagnostic-body.img'
    run(sys.executable, PACK / 'mkbootimg.py', *args, '--output', body_path)
    body = bytearray(body_path.read_bytes())
    body[28:32] = stock[28:32]
    body_path.write_bytes(body)
    layout = boot.layout(body)
    assert layout['body_end'] == len(body) < len(stock) == 67108864
    for name, data in [('kernel', compressed + base), ('ramdisk', ramdisk), ('recovery_dtbo', table)]:
        assert layout['sections'][name]['sha256'] == sha(data)
    rebuilt_args = run(sys.executable, PACK / 'unpack_bootimg.py', '--boot_img', body_path,
                       '--out', OUT / 'unpacked', '--format', 'mkbootimg', '-0').split(b'\0')
    if not rebuilt_args[-1]:
        rebuilt_args.pop()
    roundtrip_path = OUT / 'roundtrip.img'
    run(sys.executable, PACK / 'mkbootimg.py', *[a.decode() for a in rebuilt_args], '--output', roundtrip_path)
    roundtrip = bytearray(roundtrip_path.read_bytes())
    roundtrip[28:32] = body[28:32]
    assert roundtrip == body
    # Header differences are restricted to kernel size, embedded overlay size/
    # position and mkbootimg's payload digest. All addresses persist; the diagnostic command line is recorded below.
    old_header, new_header = bytearray(original[:4096]), bytearray(body[:4096])
    for start, stop in [(8, 12), (64, 1632), (1632, 1644)]:
        old_header[start:stop] = new_header[start:stop] = bytes(stop - start)
    assert old_header == new_header
    candidate = bytes(body) + bytes(len(stock) - len(body))
    (OUT / 'recovery-diagnostic-unsigned.img').write_bytes(candidate)
    validator = Path('/home/a6l/tools/a6l-overlay-validator/ufdt_apply_overlay')
    run('fdtoverlay', '-i', OUT / 'base.dtb', '-o', OUT / 'merged-libfdt.dtb', OUT / 'overlay.dtbo')
    run(validator, OUT / 'base.dtb', OUT / 'overlay.dtbo', OUT / 'merged-libufdt.dtb')
    modern = read_fdt((OUT / 'merged-libufdt.dtb').read_bytes())
    assert modern == read_fdt((OUT / 'merged-libfdt.dtb').read_bytes())
    expected = {n: dict(p) for n, p in after.items()}
    expected['/chosen']['hisense,a6l-recovery-probe'] = b'v1\0'
    assert modern == expected
    report = dict(scope='Offline early LCD console candidate; no phone access',
                  packaging_passed=True, ready_to_flash=False, layout=layout,
                  kernel_has_bounded_early_console=True, ramdisk_unchanged=True, command_line=cmdline,
                  added_phandle_nodes=sorted(added_handles),
                  hardware_properties_preserved=True, modern_overlay_passed=True,
                  remaining=['Run captured ABL overlay, selection, fixup and packaging checks',
                             'Prepare guarded recovery-only physical trial and stock restoration',
                             'Physical LCD scanout/stride assumption and kernel execution remain unverified'],
                  artifacts=[dict(file=p.name, bytes=p.stat().st_size, sha256=sha(p.read_bytes()))
                             for p in sorted(OUT.iterdir()) if p.is_file()])
    (OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(candidate_sha256=sha(candidate), body_bytes=len(body),
                          base_bytes=len(base), overlay_bytes=len(overlay), packaging_passed=True)))


if __name__ == '__main__':
    main()
