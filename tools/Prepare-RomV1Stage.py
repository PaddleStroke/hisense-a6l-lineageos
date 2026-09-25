#!/usr/bin/env python3
"""Assemble the laptop kit ~/A6L-usb-20260915/rom-v1 (agent flash). Offline, WSL. usage: Prepare-RomV1Stage.py <out-dir>
images/: boot.img (64 MiB), dtbo.img (8 MiB), system.erofs + vendor.erofs (trimmed to the EROFS size: only those bytes are
written; the rest of the stock partition stays as it is and is never read by LineageOS), rom-v1-pins.json, SHA256SUMS.
tools: worker, engine, layout, coordinators, launchers, verifier + rom-v1-tools.json (their hashes)."""
import hashlib, json, shutil, struct, sys
from pathlib import Path
ROOT = Path('/mnt/c/Users/Pierre/Desktop/A6L'); P = Path('/home/a6l/android/a6l-lineage24/out/target/product/a6l')
BOOT = Path('/home/a6l/rom-v1/boot')
out = Path(sys.argv[1]); img = out / 'images'; img.mkdir(parents=True, exist_ok=False)


def sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 22), b''):
            h.update(b)
    return h.hexdigest()


def trimmed(src, dst):
    with open(src, 'rb') as f:
        f.seek(1024); sb = f.read(128); assert sb[:4] == bytes.fromhex('e2e1f5e0'), 'not erofs'
        size = struct.unpack_from('<I', sb, 36)[0] << sb[12]
        f.seek(0)
        with open(dst, 'wb') as o:
            left = size
            while left:
                b = f.read(min(left, 1 << 24)); o.write(b); left -= len(b)
    assert size % 4096 == 0
    return size


shutil.copyfile(BOOT / 'boot.img', img / 'boot.img'); shutil.copyfile(BOOT / 'dtbo.img', img / 'dtbo.img')
rep = json.loads((BOOT / 'report.json').read_text()); assert not rep['qemu_variant']
abl = json.loads((BOOT / 'captured-abl-validation.json').read_text()); assert abl['passed']
assert sha(img / 'boot.img') == rep['boot_sha256']
sizes = {'system': trimmed(P / 'system.img', img / 'system.erofs'), 'vendor': trimmed(P / 'vendor.img', img / 'vendor.erofs')}
fv = json.loads((ROOT / 'firmware/raw-backup-20260914/firmware-verification.json').read_text())
stock = {p['name']: p['sha256'] for p in fv['partitions'] if p['name'] in ('system', 'vendor', 'dtbo', 'boot')}
pins = {'rom': 'rom-v1', 'stock': {k: stock[k] for k in ('system', 'vendor', 'dtbo')}, 'stock_boot_14sep': stock['boot'],
        'boot_report': {k: rep[k] for k in ('boot_sha256', 'dtbo_sha256', 'kernel_sha256', 'dtb_sha256', 'ramdisk_sha256', 'init_sha256', 'cmdline')},
        'images': {}}
for n, f in [('boot', 'boot.img'), ('dtbo', 'dtbo.img'), ('system', 'system.erofs'), ('vendor', 'vendor.erofs')]:
    pins['images'][n] = {'file': f, 'bytes': (img / f).stat().st_size, 'sha256': sha(img / f)}
(img / 'rom-v1-pins.json').write_text(json.dumps(pins, indent=2) + '\n')
(img / 'SHA256SUMS').write_text(''.join(f'{sha(img / f)}  {f}\n' for f in sorted(x.name for x in img.iterdir() if x.name != 'SHA256SUMS')))
tools = ['Write-LaptopRomV1.py', 'RomFlashEngineV1.py', 'RomFlashLayoutV1.py', 'Run-LaptopRomInstall-v1.py', 'Run-LaptopRomRestore-v1.py',
         'Launch-RomV1Install.py', 'Launch-RomV1Restore.py', 'Verify-RomV1Stage.py']
for t in tools:
    shutil.copyfile(ROOT / 'tools' / t, out / t)
    (out / t).write_bytes((out / t).read_bytes().replace(b'\r\n', b'\n'))
(out / 'rom-v1-tools.json').write_text(json.dumps({'files': {t: sha(out / t) for t in tools}}, indent=2) + '\n')
print('ROM_V1_STAGE_PASS', json.dumps({n: p['sha256'][:16] for n, p in pins['images'].items()}), json.dumps(sizes))
