#!/usr/bin/env python3
"""Host test (bug hunt round2 install-tools, 29 Sep 2026): rom-v1 engine/worker guards on a MINI virtual eMMC.
No USB, no phone, no firmware files: the layout module is scaled down (same partition names, synthetic GPT with valid CRCs),
so install -> restore runs in a second. Covers:
  * restore refuses a manifest whose recorded layout differs (userdata=original took its write ranges from it) - no write
  * restore refuses a backup file whose length differs from its restore range BEFORE any write (was: mid-transfer failure
    after earlier partitions were overwritten)
  * check_plan: writes outside one programmable partition (vbmeta, straddling) refused
  * worker Guard/FirehoseDevice: program outside the fixed plan refused with nothing sent; plan fixed once; no plan = no program
usage: python3 tools/tests/test-rom-flash-guards.py      (A6L_ENGINE=<old RomFlashEngineV1.py> to run the engine cases pre-fix)
"""
import hashlib, importlib.util, json, os, random, struct, sys, tempfile, zlib
from pathlib import Path
TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))
import RomFlashLayoutV1 as L
if os.environ.get('A6L_ENGINE'):
    spec = importlib.util.spec_from_file_location('RomFlashEngineV1', os.environ['A6L_ENGINE'])
    E = importlib.util.module_from_spec(spec); spec.loader.exec_module(E)
else:
    import RomFlashEngineV1 as E

# ---- mini layout ---------------------------------------------------------------------------------------------------------
NAMES = ['fsg', 'dtbo', 'boot', 'recovery', 'fsc', 'modemst1', 'modemst2', 'system', 'vendor', 'persist', 'misc', 'devinfo',
         'vbmeta', 'metadata', 'userdata']
SIZES = {'userdata': 4096, 'fsc': 2, 'devinfo': 8, 'misc': 16}
parts, lba = {}, 2048
for n in NAMES:
    parts[n] = (lba, SIZES.get(n, 64)); lba += SIZES.get(n, 64) + 8
TOTAL = lba + 2048
L.PARTITIONS.clear(); L.PARTITIONS.update(parts)
L.DISK_BYTES = TOTAL * 512
L.GPT_TAIL = (TOTAL - 44, 44)
L.USERDATA_HEAD_ZERO, L.USERDATA_HEAD_BACKUP, L.USERDATA_TAIL_BACKUP, L.USERDATA_FOOTER, L.METADATA_HEAD_ZERO = 32, 256, 64, 8, 16
L.PROGRAMMABLE = tuple(dict.fromkeys(('boot', 'dtbo', 'vendor', 'system', 'metadata', 'userdata') + L.ROM_MAY_WRITE))


def gpt():
    ents = bytearray(128 * 128)
    for i, n in enumerate(NAMES):
        st, cnt = parts[n]
        e = struct.pack('<16s16sQQQ', b'T' * 16, bytes([i + 1]) * 16, st, st + cnt - 1, 0) + n.encode('utf-16-le').ljust(72, b'\0')
        ents[i * 128:(i + 1) * 128] = e
    ecrc = zlib.crc32(bytes(ents)) & 0xffffffff
    h = bytearray(struct.pack('<8sIII4sQQQQ16sQIII', b'EFI PART', 0x10000, 92, 0, b'\0' * 4, 1, TOTAL - 1, 34, TOTAL - 34,
                              b'G' * 16, 2, 128, 128, ecrc))
    struct.pack_into('<I', h, 16, zlib.crc32(bytes(h)) & 0xffffffff)
    prim = bytearray(2048 * 512); prim[512:512 + 92] = h; prim[1024:1024 + len(ents)] = ents
    return bytes(prim)


results = {}


def check(name, fn):
    try:
        fn(); results[name] = 'pass'; print('PASS', name, flush=True)
    except Exception as e:   # noqa: BLE001 - any exception is a failed case
        results[name] = 'FAIL %r' % e; print('FAIL', name, repr(e), flush=True)


work = Path(tempfile.mkdtemp(prefix='a6l-flashguard-'))
disk = work / 'emmc.img'
rnd = random.Random(5)
with open(disk, 'wb') as f:
    f.truncate(TOTAL * 512); f.write(gpt())
    for n in NAMES:
        st, cnt = parts[n]; f.seek(st * 512)
        f.write(bytes(cnt * 512) if n == 'misc' else rnd.randbytes(cnt * 512))


def psha(st, cnt):
    with open(disk, 'rb') as f:
        f.seek(st * 512); return hashlib.sha256(f.read(cnt * 512)).hexdigest()


L.VBMETA_STOCK = psha(*parts['vbmeta']); L.DEVINFO_UNLOCKED = psha(*parts['devinfo'])
before = {n: psha(*parts[n]) for n in NAMES}
imgs = {}
for n in ('boot', 'dtbo', 'vendor', 'system'):
    p = work / f'{n}.img'; p.write_bytes(rnd.randbytes(16 * 512)); imgs[n] = (p, 16 * 512, hashlib.sha256(p.read_bytes()).hexdigest())
pins = {n: before[n] for n in ('system', 'vendor', 'dtbo')}
kit = {'primary': gpt(), 'tail': bytes(44 * 512)}
with open(disk, 'r+b') as f:
    f.seek(L.GPT_TAIL[0] * 512); f.write(kit['tail'])


def cap(name):
    c = work / name; c.mkdir(); return c


def programs(dev):
    return [x for x in dev.log if x[0] == 'program']


def t_install_restore_ok():
    dev = E.FileDevice(disk); E.run_install(dev, cap('install'), imgs, pins, kit, {}, lambda: None)
    with open(disk, 'r+b') as f:
        f.seek(parts['modemst1'][0] * 512); f.write(b'rom' * 100)
    dev = E.FileDevice(disk); r = {}; E.run_restore(dev, cap('restore'), work / 'install', kit, r, lambda: None)
    assert r['readback_verified'] and r['rom_changed'] == ['metadata', 'modemst1'], r.get('rom_changed')
    after = {n: psha(*parts[n]) for n in NAMES if n != 'userdata'}
    assert all(after[n] == before[n] for n in after), [n for n in after if after[n] != before[n]]


def tamper_manifest(fn):
    m = json.loads((work / 'install/backup-manifest.json').read_text()); old = json.dumps(m)
    fn(m); (work / 'install/backup-manifest.json').write_text(json.dumps(m))
    return old


def t_manifest_layout_refused():
    # userdata=original wrote userdata-head/tail at the ranges RECORDED in the manifest: a stale/edited layout that points
    # userdata-head at the boot partition must not reach the disk
    old = tamper_manifest(lambda m: m['layout'].__setitem__('userdata-head', [parts['boot'][0], L.USERDATA_HEAD_BACKUP]))
    boot_before = psha(*parts['boot'])
    try:
        dev = E.FileDevice(disk)
        try:
            E.run_restore(dev, cap('restore-layout'), work / 'install', kit, {}, lambda: None, userdata='original')
        except E.StopRun as e:
            assert not programs(dev), 'refused only after writing: %s' % programs(dev)
            assert 'layout' in str(e) or 'not inside' in str(e) or 'length' in str(e), e
            return
        raise AssertionError('restore used the manifest layout: boot now %s (was %s)' % (psha(*parts['boot'])[:12], boot_before[:12]))
    finally:
        (work / 'install/backup-manifest.json').write_text(old)


def t_short_backup_refused_before_write():
    # dtbo.bin shorter than the dtbo partition, manifest hash consistent (e.g. a layout change between install and restore)
    b = work / 'install/dtbo.bin'; keep = b.read_bytes(); b.write_bytes(keep[:-512])
    old = tamper_manifest(lambda m: m['backups'].__setitem__('dtbo', hashlib.sha256(keep[:-512]).hexdigest()))
    try:
        dev = E.FileDevice(disk)
        try:
            E.run_restore(dev, cap('restore-short'), work / 'install', kit, {}, lambda: None)
        except E.StopRun:
            assert not programs(dev), 'failed only after %d program(s) had started: %s' % (len(programs(dev)), programs(dev))
            return
        raise AssertionError('short backup accepted')
    finally:
        b.write_bytes(keep); (work / 'install/backup-manifest.json').write_text(old)


def t_check_plan():
    for bad in [('x', *parts['vbmeta']), ('x', *parts['recovery']), ('x', parts['boot'][0] + 60, 16), ('x', 0, 34),
                ('x', parts['boot'][0], 0)]:
        try:
            L.check_plan([bad])
        except ValueError:
            continue
        raise AssertionError('check_plan accepted %s' % (bad,))
    assert L.check_plan(L.restore_writes(set(L.ROM_MAY_WRITE)))


def t_worker_plan():
    spec = importlib.util.spec_from_file_location('w', TOOLS / 'Write-LaptopRomV1.py'); w = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(w)
    g = w.Guard()
    xml_boot = L.program_xml(*parts['boot'])
    g.programs = {parts['boot']}
    try:
        g.check(xml_boot); raise AssertionError('program accepted with no fixed plan')
    except ValueError:
        pass
    plan = w.plan_for_mode('install', {k: v for k, v in imgs.items()})
    g.set_plan(plan)
    try:
        g.set_plan(plan | {parts['vbmeta']}); raise AssertionError('plan replaced')
    except ValueError:
        pass
    sent = []

    class Cfg: MaxPayloadSizeToTargetInBytes = 1048576

    class FH:
        cfg = Cfg(); cdc = None
        def xmlsend(self, data):
            g.check(data); sent.append(data); raise RuntimeError('stop after the guard (test)')
    dev = w.FirehoseDevice(FH(), g, {}, {}, lambda: None)
    try:
        dev.program(*parts['recovery'], iter([bytes(512)])); raise AssertionError('program outside the plan accepted')
    except E.StopRun:
        pass
    except Exception as e:   # the worker module has its own engine import
        assert type(e).__name__ == 'StopRun', e
    assert not sent, sent
    st, n = sorted(plan)[0]
    try:
        dev.program(st, n, iter([bytes(n * 512)]))
    except RuntimeError:
        pass
    assert len(sent) == 1 and 'start_sector="%d"' % st in sent[0], sent
    r = w.plan_for_mode('restore', {}, 'original')
    assert L.backup_regions()['userdata-head'] in r and parts['vbmeta'] not in r and parts['recovery'] not in r


check('install_restore_ok', t_install_restore_ok)
check('manifest_layout_refused_no_write', t_manifest_layout_refused)
check('short_backup_refused_before_any_write', t_short_backup_refused_before_write)
check('check_plan', t_check_plan)
check('worker_fixed_plan', t_worker_plan)
import shutil; shutil.rmtree(work, ignore_errors=True)
ok = all(v == 'pass' for v in results.values())
print('ROM_FLASH_GUARDS', 'PASS' if ok else 'FAIL', json.dumps(results))
sys.exit(0 if ok else 1)
