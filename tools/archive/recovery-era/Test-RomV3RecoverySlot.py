#!/usr/bin/env python3
"""rom-v3 kit x recovery slot content (agent usbrec, 26 Sep 2026). Offline, no USB, no phone.
Runs the unchanged Test-RomV1Flash.py virtual-eMMC simulation (install, reinstall refusal, byte-identical restore, bad GPT /
payload / transfer failure / tampered backup / XML guard) with the RECOVERY partition of the virtual disk holding a given
diagnostic recovery image instead of the stock bytes, and checks that the ROM install and restore never change it.
usage (from tools/): python3 Test-RomV3RecoverySlot.py <workdir> <rom kit images dir> <recovery image> <expected sha256>"""
import hashlib, json, sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
work, stage, image, expected = sys.argv[1], sys.argv[2], Path(sys.argv[3]), sys.argv[4]
data = image.read_bytes()
assert len(data) == 67108864 and hashlib.sha256(data).hexdigest() == expected, 'recovery image differs'
src = (HERE / 'Test-RomV1Flash.py').read_text()
hook = 'build_disk(disk)\nbefore = '
assert src.count(hook) == 1
src = src.replace(hook, 'build_disk(disk)\n__put_recovery(disk)\nbefore = ')

def put_recovery(disk):
    import RomFlashLayoutV1 as L
    st, cnt = L.PARTITIONS['recovery']
    assert (st, cnt) == (917504, 131072) and cnt * 512 == len(data)
    with open(disk, 'r+b') as f:
        f.seek(st * 512); f.write(data)

ns = {'__name__': '__rom_flash_test__', '__file__': str(HERE / 'Test-RomV1Flash.py'), '__put_recovery': put_recovery}
sys.argv = ['Test-RomV1Flash.py', work, stage]
exec(compile(src, 'Test-RomV1Flash.py', 'exec'), ns)
L = ns['L']
ok = all(v == 'pass' for v in ns['results'].values())
slot = ns['before']['recovery'] == expected and ns['part_sha'](ns['disk'], *L.PARTITIONS['recovery']) == expected
label = L.RECOVERY_KNOWN.get(expected, 'unknown')
out = {'recovery_image_sha256': expected, 'engine_label': label, 'flash_tests': ns['results'],
       'recovery_slot_unchanged_after_install_and_restore': slot}
(Path(work) / 'Test-RomV3RecoverySlot.json').write_text(json.dumps(out, indent=2) + '\n')
print('ROM_V3_RECOVERY_SLOT', 'PASS' if ok and slot else 'FAIL', json.dumps(out))
