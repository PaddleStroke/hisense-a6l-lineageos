#!/usr/bin/env python3
"""Offline end-to-end check of the rom-v1 install/restore engine against a SPARSE VIRTUAL eMMC with the real geometry
(agent flash). No USB, no phone. Run in WSL: python3 Test-RomV1Flash.py <workdir> <rom-v1 stage dir>
The virtual disk gets the real stock GPT (first MiB + tail of the 14 Sep backup) and the real stock bytes of every
partition the tools read/write (from emmc-firmware-prefix.bin); userdata gets random head/tail bytes.
Checks: install (all guards, backups, writes, readbacks) -> simulated ROM runtime writes (modemst1, misc, persist
untouched, userdata formatted) -> restore -> every partition outside userdata byte-identical to the pre-install disk,
userdata head/footer zeroed. Plus negative controls (wrong GPT, wrong payload, non-stock predecessor, injected transfer
failure, tampered backup file, guard XML rejections).
"""
import hashlib, json, os, random, shutil, sys, importlib.util
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import RomFlashLayoutV1 as L
import RomFlashEngineV1 as E

ROOT = Path('/mnt/c/Users/Pierre/Desktop/A6L')
PREFIX = ROOT / 'firmware/raw-backup-20260914/emmc-firmware-prefix.bin'
TAIL = ROOT / 'firmware/raw-backup-20260914/emmc-gpt-tail.bin'
work = Path(sys.argv[1]); stage = Path(sys.argv[2])
work.mkdir(parents=True, exist_ok=True)
results = {}


def check(name, fn):
    try:
        fn(); results[name] = 'pass'; print('PASS', name, flush=True)
    except AssertionError as e:
        results[name] = 'FAIL ' + str(e); print('FAIL', name, e, flush=True)


def build_disk(path):
    with open(path, 'wb') as f:
        f.truncate(L.DISK_BYTES)
    with open(PREFIX, 'rb') as src, open(path, 'r+b') as dst:
        dst.write(src.read(1 << 20))  # GPT primary
        for n in ('boot', 'dtbo', 'recovery', 'misc', 'metadata', 'modemst1', 'modemst2', 'fsg', 'fsc', 'persist', 'vbmeta', 'devinfo', 'vendor', 'system'):
            st, cnt = L.PARTITIONS[n]
            src.seek(st * 512); dst.seek(st * 512)
            left = cnt * 512
            while left:
                b = src.read(min(1 << 24, left)); dst.write(b); left -= len(b)
        dst.seek(L.GPT_TAIL[0] * 512); dst.write(TAIL.read_bytes())
        rnd = random.Random(7)
        ud, udn = L.PARTITIONS['userdata']
        dst.seek(ud * 512); dst.write(rnd.randbytes(64 << 20))
        dst.seek((ud + udn) * 512 - (1 << 20)); dst.write(rnd.randbytes(1 << 20))


def part_sha(disk, start, sectors):
    h = hashlib.sha256()
    with open(disk, 'rb') as f:
        f.seek(start * 512); left = sectors * 512
        while left:
            b = f.read(min(1 << 24, left)); h.update(b); left -= len(b)
    return h.hexdigest()


pins = json.loads((stage / 'rom-v1-pins.json').read_text())
images = {n: (stage / pins['images'][n]['file'], pins['images'][n]['bytes'], pins['images'][n]['sha256']) for n in ('boot', 'dtbo', 'vendor', 'system')}
stock_pins = dict(pins['stock'])
kit = {'primary': PREFIX.open('rb').read(1 << 20), 'tail': TAIL.read_bytes()}
disk = work / 'emmc.img'
build_disk(disk)
before = {n: part_sha(disk, *L.PARTITIONS[n]) for n in L.PARTITIONS if n != 'userdata'}
assert before['vbmeta'] == L.VBMETA_STOCK and before['devinfo'] == L.DEVINFO_UNLOCKED
for n in ('system', 'vendor', 'dtbo'):
    assert before[n] == stock_pins[n], n


def new_capture(name):
    c = work / name
    if c.exists():
        shutil.rmtree(c)
    c.mkdir()
    return c


def run_install(dev, cap, imgs=images, **k):
    report = {}
    E.run_install(dev, cap, imgs, stock_pins, kit, report, lambda: None, **k)
    return report


def t_install():
    dev = E.FileDevice(disk); cap = new_capture('install')
    r = run_install(dev, cap)
    assert r['readback_verified'] and r['power'] == 'off' and r['predecessor'] == {'system': 'stock', 'vendor': 'stock', 'dtbo': 'stock'}
    assert [x for x in dev.log if x[0] == 'power'] == [('power', 'off')]
    progs = [x for x in dev.log if x[0] == 'program']
    assert [(p[1], p[2]) for p in progs] == [(s, n) for _, s, n in L.install_writes({k: v[1] for k, v in images.items()})], progs
    # every program is inside its partition
    for _, s, n in progs:
        assert any(ps <= s and s + n <= ps + pn for ps, pn in L.PARTITIONS.values()), (s, n)
    after = {n: part_sha(disk, *L.PARTITIONS[n]) for n in L.PARTITIONS if n != 'userdata'}
    for n in after:
        if n not in ('boot', 'dtbo', 'vendor', 'system', 'metadata'):
            assert after[n] == before[n], 'untouched partition changed: ' + n
    md = L.PARTITIONS['metadata'][0]
    assert part_sha(disk, md, 2048) == hashlib.sha256(bytes(1 << 20)).hexdigest(), 'metadata head not zeroed'
    assert part_sha(disk, L.PARTITIONS['boot'][0], L.PARTITIONS['boot'][1]) == images['boot'][2]
    ud = L.PARTITIONS['userdata'][0]
    assert part_sha(disk, ud, 2048) == hashlib.sha256(bytes(1 << 20)).hexdigest()
    m = json.loads((cap / 'backup-manifest.json').read_text())
    for n in ('boot', 'dtbo', 'system', 'vendor', 'metadata', 'modemst1', 'persist', 'misc'):
        assert m['backups'][n] == before[n], n


def t_second_install_refused():
    dev = E.FileDevice(disk); cap = new_capture('install-again')
    try:
        run_install(dev, cap)
    except E.StopRun as e:
        assert 'not the verified stock' in str(e), e
        assert not [x for x in dev.log if x[0] == 'program']
        return
    raise AssertionError('reinstall over rom-v1 accepted without --allow-nonstock')


def simulate_rom_runtime():
    rnd = random.Random(9)
    with open(disk, 'r+b') as f:
        for n in ('modemst1', 'modemst2', 'misc', 'metadata'):
            st, cnt = L.PARTITIONS[n]; f.seek(st * 512); f.write(rnd.randbytes(4096))
        ud, udn = L.PARTITIONS['userdata']
        f.seek(ud * 512); f.write(rnd.randbytes(8 << 20))            # ext4 formatted by fs_mgr
        f.seek((ud + udn) * 512 - 16384); f.write(rnd.randbytes(16384))


def t_restore():
    simulate_rom_runtime()
    dev = E.FileDevice(disk); cap = new_capture('restore'); report = {}
    E.run_restore(dev, cap, work / 'install', kit, report, lambda: None)
    assert report['readback_verified'] and report['power'] == 'reset'
    assert set(report['rom_changed']) == {'modemst1', 'modemst2', 'misc', 'metadata'}, report['rom_changed']
    after = {n: part_sha(disk, *L.PARTITIONS[n]) for n in L.PARTITIONS if n != 'userdata'}
    assert after == before, [n for n in after if after[n] != before[n]]
    ud, udn = L.PARTITIONS['userdata']
    assert part_sha(disk, ud, 2048) == hashlib.sha256(bytes(1 << 20)).hexdigest()
    assert part_sha(disk, ud + udn - 32, 32) == hashlib.sha256(bytes(16384)).hexdigest()


def t_bad_gpt():
    bad = dict(kit); bad['primary'] = kit['primary'][:600] + b'X' + kit['primary'][601:]
    dev = E.FileDevice(disk); cap = new_capture('badgpt')
    try:
        E.run_install(dev, cap, images, stock_pins, bad, {}, lambda: None)
    except E.StopRun as e:
        assert 'GPT differs' in str(e); assert all(x[0] == 'read' for x in dev.log); return
    raise AssertionError('wrong GPT accepted')


def t_bad_payload():
    imgs = dict(images); p, n, d = imgs['boot']; imgs['boot'] = (p, n, '0' * 64)
    dev = E.FileDevice(disk); cap = new_capture('badpayload')
    try:
        E.run_install(dev, cap, imgs, stock_pins, kit, {}, lambda: None)
    except E.StopRun as e:
        assert 'payload hash' in str(e) and dev.log == []; return
    raise AssertionError('bad payload accepted')


def t_transfer_failure():
    # restore stock first (disk currently restored) then inject a failure on the 2nd program: no retry, no power
    dev = E.FileDevice(disk, fail_program_at=2); cap = new_capture('install-fail')
    try:
        run_install(dev, cap)
    except E.StopRun as e:
        assert 'injected' in str(e)
        assert len([x for x in dev.log if x[0] == 'program']) == 2 and not [x for x in dev.log if x[0] == 'power']
        return
    raise AssertionError('injected failure not propagated')


def t_tampered_backup():
    b = work / 'install' / 'system.bin'
    with open(b, 'r+b') as f:
        f.seek(12345); c = f.read(1); f.seek(12345); f.write(bytes([c[0] ^ 1]))
    dev = E.FileDevice(disk); cap = new_capture('restore-tampered')
    try:
        E.run_restore(dev, cap, work / 'install', kit, {}, lambda: None)
    except E.StopRun as e:
        assert 'backup file differs' in str(e) and dev.log == []; return
    finally:
        with open(b, 'r+b') as f:
            f.seek(12345); c = f.read(1); f.seek(12345); f.write(bytes([c[0] ^ 1]))
    raise AssertionError('tampered backup accepted')


def t_guard():
    spec = importlib.util.spec_from_file_location('w', HERE / 'Write-LaptopRomV1.py'); w = importlib.util.module_from_spec(spec); spec.loader.exec_module(w)
    g = w.Guard()
    ok = [L.program_xml(*L.PARTITIONS['boot'])]
    g.programs = {tuple(L.PARTITIONS['boot'])}
    assert g.check(ok[0]) == 'program'
    bad = [L.program_xml(L.PARTITIONS['boot'][0] + 1, L.PARTITIONS['boot'][1]), L.program_xml(*L.PARTITIONS['recovery']),
           L.program_xml(*L.PARTITIONS['modemst1']), '<data><erase/></data>', '<data><patch/></data>',
           '<data><power value="reset_to_edl"/></data>', '<data><power value="off"/></data>',
           ok[0].replace('physical_partition_number="0"', 'physical_partition_number="1"'),
           '<data><read SECTOR_SIZE_IN_BYTES="512" num_partition_sectors="8" physical_partition_number="0" start_sector="0"/></data>']
    for x in bad:
        try:
            g.check(x)
        except ValueError:
            continue
        raise AssertionError('guard accepted ' + x)
    g.power = 'off'; assert g.check('<data><power value="off"/></data>') == 'power'
    g.programs = set()
    try:
        g.check(ok[0])
    except ValueError:
        return
    raise AssertionError('program accepted without registration')


check('install_full', t_install)
check('reinstall_refused_without_flag', t_second_install_refused)
check('restore_full_byte_identical', t_restore)
check('bad_gpt_no_write', t_bad_gpt)
check('bad_payload_no_usb', t_bad_payload)
check('transfer_failure_no_retry_no_power', t_transfer_failure)
check('tampered_backup_refused', t_tampered_backup)
check('xml_guard', t_guard)
(work / 'Test-RomV1Flash.json').write_text(json.dumps(results, indent=2) + '\n')
print('ROM_V1_FLASH_TESTS', 'PASS' if all(v == 'pass' for v in results.values()) else 'FAIL', json.dumps(results))
