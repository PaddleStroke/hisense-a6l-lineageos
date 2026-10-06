#!/usr/bin/env python3
"""WSL-only: prepare a RAM GPU regression payload; does not contact the phone.
Baseline modules/firmware are extracted from the hash-verified installed r6f
vendor image. Same runtime and executable are used for old/patched Mesa.
"""
import hashlib
import json
import re
import shutil
import subprocess
import tarfile
from pathlib import Path

ROOT = Path('/mnt/c/Users/Pierre/Desktop/A6L')
WORK = Path('/home/a6l/mesa-blitfix-20261002')
PRODUCT = Path('/home/a6l/android/a6l-lineage24/out/target/product/a6l')
TC = Path('/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin')
OUT = WORK / 'copy-probe-payload'
OUT.mkdir(exist_ok=True)

def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()

vendor = Path('/home/a6l/rom-v2/kit-r6f/rom-v2/images/vendor.erofs')
pins = json.loads((ROOT / 'firmware/extracted/rom-r6f-20261001/rom-v1-pins.json').read_text())
assert sha(vendor) == pins['images']['vendor']['sha256'], 'wrong vendor image'
extracted = WORK / 'verified-r6f-vendor'
extract_marker = WORK / 'verified-r6f-vendor.sha256'
if not extract_marker.exists():
    result = subprocess.run([str(PRODUCT.parents[2] / 'host/linux-x86/bin/fsck.erofs'),
                             '--overwrite', '--extract=' + str(extracted), str(vendor)],
                            capture_output=True, text=True)
    (WORK / 'vendor-extraction.log').write_text(result.stdout + result.stderr)
    result.check_returncode()
    extract_marker.write_text(sha(vendor) + '\n')
assert extract_marker.read_text().strip() == sha(vendor)
subprocess.run([str(TC / 'aarch64-linux-android34-clang'), '-O2', '-Wall',
                '-Wextra', '-Werror', str(ROOT / 'tools/gpu-copy-probe.c'),
                '-o', str(OUT / 'gpu-copy-probe'),
                '-L' + str(WORK / 'build-arm64/src/mesa/glapi/es2api'),
                '-Wl,--allow-shlib-undefined', '-lEGL', '-lGLESv2', '-lm'], check=True)

modules = ['drm_exec', 'drm_gpuvm', 'gpu-sched', 'mdt_loader', 'ocmem',
           'ubwc_config', 'llcc-qcom', 'qcom_aoss', 'cec',
           'drm_display_helper', 'drm_dp_aux_bus', 'msm']
(OUT / 'modules').mkdir(exist_ok=True)
for mod in modules:
    src = extracted / 'lib/modules' / (mod + '.ko')
    assert src.is_file(), src
    shutil.copyfile(src, OUT / 'modules' / src.name)
    assert sha(src) == sha(PRODUCT / 'vendor/lib/modules' / src.name), 'modules changed'
(OUT / 'modules/order.txt').write_text(''.join(m + '.ko\n' for m in modules))
firmware = ['qcom/a530_pm4.fw', 'qcom/a530_pfp.fw', 'qcom/a530v3_gpmu.fw2']
firmware += ['qcom/hisense/a6l/a512_zap.' + ext for ext in ('mdt', 'b00', 'b01', 'b02')]
for name in firmware:
    dst = OUT / 'firmware' / name
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(extracted / 'firmware' / name, dst)

relpaths = {'libEGL.so':'src/egl/libEGL.so',
            'libGLESv2.so':'src/mesa/glapi/es2api/libGLESv2.so',
            'libgallium_dri.so':'src/gallium/targets/dri/libgallium_dri.so'}
for tag, build in [('old', Path('/home/a6l/mesa-a6l/build')),
                   ('new', WORK / 'build-arm64')]:
    (OUT / tag).mkdir(exist_ok=True)
    for name, rel in relpaths.items():
        shutil.copyfile(build / rel, OUT / tag / name)

def needed(p):
    text = subprocess.check_output([str(TC / 'llvm-readelf'), '-d', str(p)], text=True)
    return re.findall(r'\(NEEDED\).*?\[(.*?)\]', text)

(OUT / 'runtime').mkdir(exist_ok=True)
done = set()
pending = needed(OUT / 'gpu-copy-probe')
for p in (OUT / 'new').iterdir():
    pending += needed(p)
while pending:
    name = pending.pop()
    if name in done or name in relpaths:
        continue
    done.add(name)
    source = PRODUCT / 'system/lib64' / name
    if not source.exists():
        source = PRODUCT / 'apex/com.android.runtime/lib64/bionic' / name
        if not source.exists():
            source = PRODUCT / 'recovery/root/system/lib64' / name
            if not source.exists():
                matches = list((PRODUCT / 'apex').glob('*/lib64/' + name))
                assert len(matches) == 1, (name, matches)
                source = matches[0]
    shutil.copyfile(source, OUT / 'runtime' / name)
    pending += needed(source)
shutil.copyfile(PRODUCT / 'apex/com.android.runtime/bin/linker64', OUT / 'runtime/linker64')

files = sorted(p for p in OUT.rglob('*') if p.is_file()
               and p.name not in ('manifest.json', 'SHA256SUMS'))
manifest = {str(p.relative_to(OUT)): {'sha256': sha(p), 'bytes': p.stat().st_size}
            for p in files}
(OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
(OUT / 'SHA256SUMS').write_text(''.join(f'{sha(p)}  {p.relative_to(OUT)}\n'
                                      for p in files + [OUT / 'manifest.json']))
archive = WORK / 'gpu-copy-probe-20261002.tar.gz'
with tarfile.open(archive, 'w:gz') as t:
    t.add(OUT, arcname='gpu-copy-probe-20261002')
evidence = ROOT / 'firmware/extracted/gpu-upstream-20261002'
shutil.copyfile(OUT / 'manifest.json', evidence / 'copy-probe-manifest.json')
shutil.copyfile(OUT / 'SHA256SUMS', evidence / 'copy-probe-SHA256SUMS')
print('GPU_COPY_PROBE_PAYLOAD_READY', archive, archive.stat().st_size, sha(archive))
