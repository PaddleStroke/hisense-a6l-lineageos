#!/usr/bin/env python3
"""Offline end-to-end check of the "preserve userdata" update on a SPARSE VIRTUAL eMMC with the REAL geometry, the real
stock GPT and stock partition bytes (agent update-keepdata, 29 Sep 2026). No USB, no phone. WSL:
  python3 Test-RomUpdateFlash.py <workdir> <prev kit dir (installed build)> <new kit dir (with update/ from Prepare-RomUpdateStage)>
Flow: stock disk -> wipe install of the PREV kit (RomFlashEngineV1, as on the phone) -> "ROM ran" (userdata ext4-like bytes
at head, middle and tail; metadata, modemst1, persist written) -> backup-only rehearsal (disk byte-identical) -> update to
the NEW kit (only boot/dtbo/vendor/system heads change; userdata sampled + head/tail, metadata, persist, EFS, misc, recovery,
vbmeta, devinfo, GPT identical) -> rollback (the four partitions back to the prev build, data still identical).
"""
import hashlib, json, random, shutil, sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import RomFlashLayoutV1 as L
import RomFlashEngineV1 as E
import RomUpdateLayoutV1 as U
import RomUpdateEngineV1 as UE

ROOT = Path('/mnt/c/Users/Pierre/Desktop/A6L')
PREFIX = ROOT / 'firmware/raw-backup-20260914/emmc-firmware-prefix.bin'
TAIL = ROOT / 'firmware/raw-backup-20260914/emmc-gpt-tail.bin'
work, prevk, newk = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
work.mkdir(parents=True, exist_ok=False)
results = {}


def check(name, fn):
    try:
        fn(); results[name] = 'pass'; print('PASS', name, flush=True)
    except Exception as e:   # noqa: BLE001
        results[name] = 'FAIL %r' % e; print('FAIL', name, repr(e), flush=True)


def images(kit):
    pins = json.loads((kit / 'images/rom-v1-pins.json').read_text())
    return pins, {n: (kit / 'images' / pins['images'][n]['file'], pins['images'][n]['bytes'], pins['images'][n]['sha256'])
                  for n in ('boot', 'dtbo', 'vendor', 'system')}


prev_pins, PREV = images(prevk)
new_pins, NEW = images(newk)
NEW_C = json.loads((newk / 'update/rom-update-compat.json').read_text())
PREV_C = json.loads((newk / 'update/prev-rom-update-compat.json').read_text())
assert json.loads((newk / 'update/prev-rom-v1-pins.json').read_text())['images'] == prev_pins['images'], 'new kit was prepared against another prev kit'
kit = {'primary': PREFIX.open('rb').read(1 << 20), 'tail': TAIL.read_bytes()}
disk = work / 'emmc.img'
with open(disk, 'wb') as f:
    f.truncate(L.DISK_BYTES)
with open(PREFIX, 'rb') as src, open(disk, 'r+b') as dst:
    dst.write(src.read(1 << 20))
    for n in ('boot', 'dtbo', 'recovery', 'misc', 'metadata', 'modemst1', 'modemst2', 'fsg', 'fsc', 'persist', 'vbmeta', 'devinfo', 'vendor', 'system'):
        st, cnt = L.PARTITIONS[n]; src.seek(st * 512); dst.seek(st * 512); left = cnt * 512
        while left:
            b = src.read(min(1 << 24, left)); dst.write(b); left -= len(b)
    dst.seek(L.GPT_TAIL[0] * 512); dst.write(TAIL.read_bytes())

UD, UDN = L.PARTITIONS['userdata']
SAMPLES = [(UD, 131072), (UD + UDN // 2, 8192), (UD + UDN - 2048, 2048)]   # head 64 MiB, middle 4 MiB, tail 1 MiB


def rsha(st, cnt):
    h = hashlib.sha256()
    with open(disk, 'rb') as f:
        f.seek(st * 512); left = cnt * 512
        while left:
            b = f.read(min(1 << 24, left)); h.update(b); left -= len(b)
    return h.hexdigest()


def snap():
    s = {n: rsha(*L.PARTITIONS[n]) for n in L.PARTITIONS if n != 'userdata'}
    s.update({'ud%d' % i: rsha(*r) for i, r in enumerate(SAMPLES)})
    s['gpt-primary'] = rsha(*L.GPT_PRIMARY); s['gpt-tail'] = rsha(*L.GPT_TAIL)
    return s


def cap(n):
    c = work / n; c.mkdir(); return c


def t_install_prev():
    r = {}
    E.run_install(E.FileDevice(disk), cap('install'), PREV, prev_pins['stock'], kit, r, lambda: None)
    assert r['readback_verified']
    rnd = random.Random(3)
    with open(disk, 'r+b') as f:
        for st, cnt in SAMPLES:
            f.seek(st * 512); f.write(rnd.randbytes(cnt * 512))
        for n in ('metadata', 'modemst1', 'persist'):
            f.seek(L.PARTITIONS[n][0] * 512); f.write(rnd.randbytes(1 << 16))


state = {}


def t_backup_only():
    before = snap(); dev = E.FileDevice(disk); r = {}
    UE.run_update(dev, cap('backup-only'), NEW, PREV, PREV_C, NEW_C, kit, r, lambda: None, mode='backup-only')
    assert r['rehearsal_passed'] and not [x for x in dev.log if x[0] == 'program'] and snap() == before
    assert r['predecessor'] == {n: 'prev' for n in U.UPDATE_WRITABLE}, r['predecessor']


def t_update():
    before = snap(); dev = E.FileDevice(disk); r = {}; c = cap('update')
    UE.run_update(dev, c, NEW, PREV, PREV_C, NEW_C, kit, r, lambda: None)
    (c / 'report.json').write_text(json.dumps(r))
    assert r['readback_verified'] and r['invariants_unchanged'] and r['power'] == 'off'
    after = snap()
    changed = sorted(k for k in after if after[k] != before[k])
    assert set(changed) <= set(U.UPDATE_WRITABLE), changed
    for n in U.UPDATE_WRITABLE:
        assert rsha(L.PARTITIONS[n][0], NEW[n][1] // 512) == NEW[n][2], n
    state['before_update'] = before


def t_rollback():
    before = state['before_update']; r = {}
    UE.run_rollback(E.FileDevice(disk), cap('rollback'), work / 'update', kit, r, lambda: None)
    assert r['readback_verified'] and r['invariants_unchanged'] and snap() == before, r.get('state')


def t_hash_only_update():
    before = snap(); dev = E.FileDevice(disk); r = {}; c = cap('hash-only-update')
    UE.run_update(dev, c, NEW, PREV, PREV_C, NEW_C, kit, r, lambda: None, backup_policy='hash-only')
    assert r['readback_verified'] and r['invariants_unchanged'] and not r['backup_complete']
    after = snap()
    assert {n for n in after if after[n] != before[n]} <= set(U.UPDATE_WRITABLE)
    for n in U.UPDATE_WRITABLE:
        assert rsha(L.PARTITIONS[n][0], NEW[n][1] // 512) == NEW[n][2], n
        assert not (c / (n + '.bin')).exists(), 'unexpected partition snapshot: ' + n
    manifest = json.loads((c / 'backup-manifest.json').read_text())
    assert manifest['kind'] == 'rom-update-hashes' and manifest['backups'] == {}


check('install_prev_wipe_installer', t_install_prev)
check('backup_only_rehearsal', t_backup_only)
check('update_keeps_userdata', t_update)
check('rollback_keeps_userdata', t_rollback)
check('hash_only_update_keeps_userdata', t_hash_only_update)
(work / 'Test-RomUpdateFlash.json').write_text(json.dumps(results, indent=2) + '\n')
print('ROM_UPDATE_FLASH_TESTS', 'PASS' if all(v == 'pass' for v in results.values()) else 'FAIL', json.dumps(results))
