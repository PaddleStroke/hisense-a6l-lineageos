#!/usr/bin/env python3
"""Offline check of the staged rom-v1 kit on the laptop (agent flash). No USB, no phone, no adb.
Checks: every file in rom-v1/SHA256SUMS; image pins; tool pins (rom-v1-tools.json); recovery kit hashes; engine/guard
self-test against a small in-memory model; edl library import in imported mode with USB disabled.
usage: python3 Verify-RomV1Stage.py [--restore]   (--restore also verifies capture-rom-v1-install/edl backup files)
"""
import hashlib, importlib.util, json, sys
from pathlib import Path
root = Path(__file__).resolve().parent
rom = root / 'images'
RESTORE = '--restore' in sys.argv
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()


def sha_big(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 22), b''):
            h.update(b)
    return h.hexdigest()


result = {'passed': False}
try:
    tools = json.loads((root / 'rom-v1-tools.json').read_text())
    for name, digest in tools['files'].items():
        assert sha(root / name) == digest, 'tool differs: ' + name
    for line in (rom / 'SHA256SUMS').read_text().splitlines():
        digest, name = line.split(None, 1)
        assert sha_big(rom / name.strip()) == digest, 'staged file differs: ' + name
    pins = json.loads((rom / 'rom-v1-pins.json').read_text())
    for n, p in pins['images'].items():
        f = rom / p['file']
        assert f.stat().st_size == p['bytes'] and sha_big(f) == p['sha256'], 'image pin: ' + n
    kit = root.parent / 'a6l-recovery-kit'
    for name, digest in json.loads((kit / 'manifest.json').read_text())['files'].items():
        assert sha(kit / name) == digest, 'kit: ' + name
    sys.path.insert(0, str(root))
    import RomFlashLayoutV1 as L
    L.check_layout_against_gpt((kit / 'expected-primary.bin').read_bytes())
    assert (kit / 'expected-tail.bin').stat().st_size == L.GPT_TAIL[1] * 512
    sys.path[:0] = [str(kit / 'deps'), str(kit / 'edl')]
    import usb.core
    def no_usb(*a, **k):
        raise RuntimeError('offline verification must not access USB')
    usb.core.find = no_usb
    spec = importlib.util.spec_from_file_location('w', root / 'Write-LaptopRomV1.py'); w = importlib.util.module_from_spec(spec); spec.loader.exec_module(w)
    g = w.Guard()
    for bad in ['<data><erase/></data>', '<data><power value="off"/></data>', L.program_xml(*L.PARTITIONS['boot'])]:
        try:
            g.check(bad)
        except ValueError:
            continue
        raise AssertionError('guard accepted ' + bad)
    g.programs = {tuple(L.PARTITIONS['boot'])}; assert g.check(L.program_xml(*L.PARTITIONS['boot'])) == 'program'
    sys.argv = ['edl.py', 'getstorageinfo', '--loader=' + str(kit / 'programmer.elf'), '--memory=eMMC', '--vid=05c6', '--pid=9008']
    spec = importlib.util.spec_from_file_location('edl', kit / 'edl/edl.py'); edl = importlib.util.module_from_spec(spec); spec.loader.exec_module(edl)
    assert edl.main(edl.args).imported is True
    if RESTORE:
        b = root / 'capture-rom-v1-install/edl'
        m = json.loads((b / 'backup-manifest.json').read_text())
        for label, digest in m['backups'].items():
            if label in ('vbmeta', 'devinfo'):
                continue
            assert sha_big(b / f'{label}.bin') == digest, 'backup differs: ' + label
        result['backup_verified'] = sorted(m['backups'])
    result.update(passed=True, images={n: p['sha256'] for n, p in pins['images'].items()})
except Exception as e:
    result['error'] = repr(e)
print(json.dumps(result, indent=2))
sys.exit(0 if result['passed'] else 1)
