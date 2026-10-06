#!/usr/bin/env python3
"""Host test (agent update-keepdata, 29 Sep 2026): "preserve userdata" update / backup-only rehearsal / rollback on a MINI
virtual eMMC (same partition names, synthetic GPT with valid CRCs; RomFlashLayoutV1 scaled down), over the file device AND
over the adb-recovery transport driven through a fake `adb` (real subprocess path, GNU dd on backing files).
No USB, no phone, no firmware files. usage: python3 tools/tests/test-rom-update-keepdata.py   -> ROM_UPDATE_TESTS PASS
"""
import hashlib, importlib.util, json, os, random, shutil, struct, subprocess, sys, tempfile, zlib
from pathlib import Path
TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))
import RomFlashLayoutV1 as L
import RomFlashEngineV1 as E
import RomUpdateLayoutV1 as U
import RomUpdateEngineV1 as UE
import RomUpdateAdbV1 as A

REAL = {k: tuple(v) for k, v in L.PARTITIONS.items()}
# ---- real-geometry static checks first (before the layout is scaled down) ----------------------------------------------
results = {}


def check(name, fn):
    try:
        fn(); results[name] = 'pass'; print('PASS', name, flush=True)
    except Exception as e:   # noqa: BLE001 - any exception is a failed case
        results[name] = 'FAIL %r' % e; print('FAIL', name, repr(e), flush=True)


def t_real_plan():
    sizes = {'boot': 64 << 20, 'dtbo': 8 << 20, 'system': 1741045760, 'vendor': 174190592}   # kit-r5 sizes
    w = U.update_writes(sizes)
    assert [x[0] for x in w] == ['boot', 'dtbo', 'vendor', 'system'] and L.check_plan(w)
    for label, st, n in w:
        assert st == REAL[label][0] and n <= REAL[label][1]
    assert U.rollback_writes() == [(n, *REAL[n]) for n in ('boot', 'dtbo', 'vendor', 'system')]
    inv = U.invariant_regions()
    for n in U.NEVER_WRITE:
        if n != 'userdata':
            assert inv[n] == REAL[n], n
    for label, st, n in w:   # no update write overlaps an invariant region
        for r, (s2, n2) in inv.items():
            assert not (st < s2 + n2 and s2 < st + n), (label, r)


check('real_geometry_plan', t_real_plan)

# ---- mini layout -----------------------------------------------------------------------------------------------------------
NAMES = ['fsg', 'dtbo', 'boot', 'recovery', 'fsc', 'modemst1', 'modemst2', 'system', 'vendor', 'persist', 'misc', 'devinfo',
         'vbmeta', 'metadata', 'userdata']
SIZES = {'userdata': 4096, 'fsc': 2, 'devinfo': 8, 'misc': 16, 'system': 256, 'vendor': 128}
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


work = Path(tempfile.mkdtemp(prefix='a6l-updtest-'))
rnd = random.Random(11)
KIT = {'primary': gpt(), 'tail': rnd.randbytes(44 * 512)}
STOCK = work / 'stock.img'
with open(STOCK, 'wb') as f:
    f.truncate(TOTAL * 512); f.write(KIT['primary'])
    for n in NAMES:
        st, cnt = parts[n]; f.seek(st * 512)
        f.write(bytes(cnt * 512) if n == 'misc' else rnd.randbytes(cnt * 512))
    f.seek(L.GPT_TAIL[0] * 512); f.write(KIT['tail'])


def rsha(disk, st, cnt):
    with open(disk, 'rb') as f:
        f.seek(st * 512); return hashlib.sha256(f.read(cnt * 512)).hexdigest()


L.VBMETA_STOCK = rsha(STOCK, *parts['vbmeta']); L.DEVINFO_UNLOCKED = rsha(STOCK, *parts['devinfo'])
STOCK_PINS = {n: rsha(STOCK, *parts[n]) for n in ('system', 'vendor', 'dtbo')}


def make_images(tag, sizes):
    out = {}
    for n in ('boot', 'dtbo', 'vendor', 'system'):
        p = work / f'{tag}-{n}.img'; p.write_bytes(rnd.randbytes(sizes[n] * 512))
        out[n] = (p, sizes[n] * 512, hashlib.sha256(p.read_bytes()).hexdigest())
    return out


PREV = make_images('prev', {'boot': 64, 'dtbo': 64, 'vendor': 96, 'system': 200})     # boot/dtbo = full partition like the kit
NEW = make_images('new', {'boot': 64, 'dtbo': 64, 'vendor': 80, 'system': 230})
COMPAT = {'system_fingerprint': 'Android/generic_system/generic:17/CP2A/eng.root:userdebug/test-keys', 'sdk': 37,
          'security_patch': '2026-09-01', 'vendor_security_patch': '2026-09-01', 'platform_cert_sha256': 'c8a2' * 16,
          'data_fs': 'ext4', 'data_encryption': 'none', 'build_type': 'userdebug', 'build_date_utc': '1'}
PREV_C = dict(COMPAT, build_date_utc='1790684811')
NEW_C = dict(COMPAT, build_date_utc='1790700000')


def installed_disk(name):
    """Stock mini disk -> rom install (the wipe installer) with PREV -> 'ROM ran': /data full of user files, metadata,
    modem EFS and persist changed. Returns the disk path."""
    disk = work / f'{name}.img'
    shutil.copyfile(STOCK, disk)
    cap = work / f'{name}-install'; cap.mkdir()
    E.run_install(E.FileDevice(disk), cap, PREV, STOCK_PINS, KIT, {}, lambda: None)
    r = random.Random(name)
    with open(disk, 'r+b') as f:
        for n in ('userdata', 'metadata', 'modemst1', 'persist'):
            st, cnt = parts[n]; f.seek(st * 512); f.write(r.randbytes(cnt * 512))
    return disk


def snap(disk):
    s = {n: rsha(disk, *parts[n]) for n in NAMES}
    s['gpt-primary'] = rsha(disk, *L.GPT_PRIMARY); s['gpt-tail'] = rsha(disk, *L.GPT_TAIL)
    return s


def cap(name):
    c = work / name; c.mkdir(); return c


def progs(dev):
    return [x for x in dev.log if x[0] == 'program']


def upd(dev, c, **k):
    rep = {}
    kw = dict(images=NEW, prev_images=PREV, prev_compat=PREV_C, new_compat=NEW_C)
    kw.update({x: k.pop(x) for x in list(k) if x in kw})
    UE.run_update(dev, c, kw['images'], kw['prev_images'], kw['prev_compat'], kw['new_compat'], KIT, rep, lambda: None, **k)
    return rep


def head_sha(disk, name, nbytes):
    return rsha(disk, parts[name][0], nbytes // 512)


PROTECTED = [n for n in NAMES if n not in U.UPDATE_WRITABLE] + ['gpt-primary', 'gpt-tail']


SEQ = [0]


def t_update_keeps_data():
    SEQ[0] += 1
    disk = installed_disk('upd%d' % SEQ[0]); before = snap(disk)
    dev = E.FileDevice(disk); c = cap('upd%d-run' % SEQ[0]); r = upd(dev, c)
    assert r['readback_verified'] and r['invariants_unchanged'] and r['power'] == 'off', r
    assert r['predecessor'] == {n: 'prev' for n in U.UPDATE_WRITABLE}, r['predecessor']
    assert [(p[1], p[2]) for p in progs(dev)] == [(s, n) for _, s, n in U.update_writes({k: v[1] for k, v in NEW.items()})]
    after = snap(disk)
    for n in PROTECTED:
        assert after[n] == before[n], 'protected region changed: ' + n
    for n in U.UPDATE_WRITABLE:
        assert head_sha(disk, n, NEW[n][1]) == NEW[n][2], n
    m = json.loads((c / 'backup-manifest.json').read_text())
    for n in U.UPDATE_WRITABLE:
        assert m['backups'][n] == before[n], n
    (c / 'report.json').write_text(json.dumps(r))
    return disk, c, before


def t_update_writes_only_changed():
    """r6b: a partition whose new image equals the installed one is not written (r6 -> r6b: dtbo unchanged)."""
    disk = installed_disk('uoc'); before = snap(disk)
    new = dict(NEW, dtbo=PREV['dtbo'])
    dev = E.FileDevice(disk); r = upd(dev, cap('uoc-run'), images=new)
    assert r['readback_verified'] and r['invariants_unchanged'] and r['power'] == 'off', r
    assert r['unchanged_skipped'] == ['dtbo'] and [p[0] for p in r['plan']] == ['boot', 'vendor', 'system'], r
    wr = U.update_writes({k: v[1] for k, v in new.items()})
    assert [(p[1], p[2]) for p in progs(dev)] == [(s, n) for l, s, n in wr if l != 'dtbo'], progs(dev)
    after = snap(disk)
    assert after['dtbo'] == before['dtbo']
    for n in ('boot', 'vendor', 'system'):
        assert head_sha(disk, n, new[n][1]) == new[n][2], n
    # same kit again without --reflash: recognised as already installed (dtbo, identical in both kits, is neutral)
    try:
        upd(E.FileDevice(disk), cap('uoc-again'), images=new)
        raise AssertionError('second update accepted')
    except E.StopRun as e:
        assert 'already installed' in str(e), str(e)
    # --reflash writes all four again
    dev2 = E.FileDevice(disk); r2 = upd(dev2, cap('uoc-reflash'), images=new, reflash=True)
    assert r2['unchanged_skipped'] == [] and len(progs(dev2)) == 4, r2


def t_backup_only():
    disk = installed_disk('bo'); before = snap(disk)
    dev = E.FileDevice(disk); r = upd(dev, cap('bo-run'), mode='backup-only', userdata_full=True)
    assert r['rehearsal_passed'] and r['power'] == 'reset' and r['plan'] == [] and len(r['plan_if_update']) == 4, r
    assert not progs(dev) and [x for x in dev.log if x[0] == 'power'] == [('power', 'reset')]
    assert snap(disk) == before
    assert L.sha256_file(work / 'bo-run' / 'userdata-full.bin') == before['userdata']


def t_rollback():
    disk, c, before = t_update_keeps_data()
    with open(disk, 'r+b') as f:   # the new build ran: user data changes after the update
        st, _ = parts['userdata']; f.seek(st * 512 + 4096); f.write(b'new-build-user-data' * 10)
    mid = snap(disk)
    dev = E.FileDevice(disk); r = {}
    UE.run_rollback(dev, cap('rb-run'), c, KIT, r, lambda: None)
    assert r['readback_verified'] and r['invariants_unchanged'] and r['power'] == 'reset', r
    assert r['state'] == {n: 'update' for n in U.UPDATE_WRITABLE}, r['state']
    after = snap(disk)
    for n in U.UPDATE_WRITABLE:
        assert after[n] == before[n], n
    for n in PROTECTED:
        assert after[n] == mid[n], n


def t_rollback_after_failed_update():
    disk = installed_disk('fail'); before = snap(disk)
    dev = E.FileDevice(disk, fail_program_at=3); c = cap('fail-run'); r = {}
    try:
        UE.run_update(dev, c, NEW, PREV, PREV_C, NEW_C, KIT, r, lambda: None)
        raise AssertionError('injected failure not propagated')
    except E.StopRun as e:
        assert 'injected' in str(e)
    assert not [x for x in dev.log if x[0] == 'power']
    (c / 'report.json').write_text(json.dumps(dict(r, mode='update')))
    r2 = {}
    UE.run_rollback(E.FileDevice(disk), cap('fail-rb'), c, KIT, r2, lambda: None)
    assert r2['state'] == {'boot': 'update', 'dtbo': 'update', 'vendor': 'saved', 'system': 'saved'}, r2['state']
    assert snap(disk) == before


def refused(name, expect, disk=None, **k):
    disk = disk or installed_disk('ref-' + name)
    before = snap(disk)
    dev = E.FileDevice(disk)
    try:
        upd(dev, cap('ref-' + name + '-run'), **k)
    except E.StopRun as e:
        assert expect in str(e), (name, str(e))
        assert not progs(dev), (name, progs(dev))
        assert snap(disk) == before, name
        return dev
    raise AssertionError('accepted: ' + name)


def t_refusals():
    stock = work / 'ref-stock.img'; shutil.copyfile(STOCK, stock)
    refused('stock', 'not the expected previous kit build', disk=stock)
    refused('cert', 'signing certificate', new_compat=dict(NEW_C, platform_cert_sha256='ff' * 32))
    refused('fbe', 'encryption differs', new_compat=dict(NEW_C, data_encryption='fileencryption=aes-256-xts'))
    refused('patch', 'security patch goes down', new_compat=dict(NEW_C, security_patch='2026-08-01'))
    refused('vpatch', 'vendor security patch', new_compat=dict(NEW_C, vendor_security_patch='2025-01-01'))
    refused('sdk', 'SDK level', new_compat=dict(NEW_C, sdk=36))
    refused('missing', 'compat field missing', new_compat={k: v for k, v in NEW_C.items() if k != 'data_fs'})
    bad = dict(NEW); p, n, _ = bad['boot']; bad['boot'] = (p, n, '0' * 64)
    d = refused('payload', 'payload hash', images=bad)
    assert d.log == [], d.log                     # nothing at all sent to the device
    d = installed_disk('ref-bcb')
    with open(d, 'r+b') as f:
        f.seek(parts['misc'][0] * 512); f.write(b'boot-recovery'.ljust(32, b'\0'))
    refused('bcb', 'unknown boot message', disk=d)
    d = installed_disk('ref-vbmeta')
    with open(d, 'r+b') as f:
        f.seek(parts['vbmeta'][0] * 512); f.write(b'AVB0')
    refused('vbmeta', 'vbmeta differs', disk=d)
    d = installed_disk('ref-gpt')
    with open(d, 'r+b') as f:
        f.seek(L.GPT_TAIL[0] * 512); f.write(b'X')
    refused('gpt', 'GPT differs', disk=d)
    # already on the new build: refused without --reflash, accepted with it
    d, _, _ = t_update_keeps_data()
    refused('same', 'already installed', disk=d)
    r = upd(E.FileDevice(d), cap('reflash-run'), reflash=True)
    assert r['readback_verified'] and r['predecessor'] == {n: 'new' for n in U.UPDATE_WRITABLE}


def t_plan_allowlist():
    for n in U.NEVER_WRITE:
        for w in [(n, *parts[n]), ('boot', *parts[n]), ('boot', parts[n][0], 1)]:
            try:
                U.check_update_plan([w])
            except ValueError:
                continue
            raise AssertionError('update plan accepted %s' % (w,))
    bad = [('boot', parts['boot'][0] + 1, 8), ('boot', parts['boot'][0], parts['boot'][1] + 1), ('dtbo', *parts['boot']),
           ('boot', 0, 34), ('boot', parts['boot'][0], 0), ('system', parts['system'][0], parts['system'][1] + 20)]
    for w in bad:
        try:
            U.check_update_plan([w])
        except ValueError:
            continue
        raise AssertionError('update plan accepted %s' % (w,))
    try:
        U.check_update_plan([('boot', *parts['boot']), ('boot', *parts['boot'])]); raise AssertionError('duplicate accepted')
    except ValueError:
        pass
    # rollback refuses a capture whose layout differs / a non-update capture
    disk, c, _ = t_update_keeps_data()
    m = json.loads((c / 'backup-manifest.json').read_text()); keep = json.dumps(m)
    m['layout']['boot'] = list(parts['userdata']); (c / 'backup-manifest.json').write_text(json.dumps(m))
    dev = E.FileDevice(disk)
    try:
        UE.run_rollback(dev, cap('rb-layout'), c, KIT, {}, lambda: None); raise AssertionError('edited layout accepted')
    except E.StopRun:
        assert dev.log == []
    finally:
        (c / 'backup-manifest.json').write_text(keep)


def t_worker_guard():
    spec = importlib.util.spec_from_file_location('wu', TOOLS / 'Write-LaptopRomUpdateV1.py'); w = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(w)
    for n in U.NEVER_WRITE:
        try:
            w.UpdateGuard().set_plan({parts[n]}); raise AssertionError('guard plan accepted ' + n)
        except ValueError:
            pass
    g = w.UpdateGuard()
    plan = {(s, c) for _, s, c in U.update_writes({k: v[1] for k, v in NEW.items()})}
    g.set_plan(plan)
    st, c = sorted(plan)[0]
    g.programs = {(st, c)}
    assert g.check(L.program_xml(st, c)) == 'program'
    # even if the base plan/transfer contained metadata, the update guard refuses the XML
    g2 = w.UpdateGuard(); g2.plan = g2.update_plan = frozenset({parts['metadata']}); g2.programs = {parts['metadata']}
    try:
        g2.check(L.program_xml(*parts['metadata'])); raise AssertionError('metadata program accepted')
    except ValueError:
        pass


OLD_V1_WORKER = """
# minimal stand-in for the kit-r5 Write-LaptopRomV1.py (pre bug hunt round 2): no set_plan, self-registering programs
import xml.etree.ElementTree as ET
LOADER_HASH = IDENTITY = None
class Guard:
    def __init__(self):
        self.reads = set(); self.programs = set(); self.power = None
    def check(self, data):
        n = ET.fromstring(data)[0]
        if n.tag == 'program' and (int(n.get('start_sector')), int(n.get('num_partition_sectors'))) not in self.programs:
            raise ValueError('Program outside the registered plan')
        return n.tag
class FirehoseDevice:
    pass
"""


def t_worker_guard_old_base():
    """kit-r5 carries the pre-bug-hunt Write-LaptopRomV1 (Guard without set_plan, programs self-registered)."""
    d = work / 'oldkit'; d.mkdir()
    for t in ('RomFlashLayoutV1.py', 'RomFlashEngineV1.py', 'RomUpdateLayoutV1.py', 'RomUpdateEngineV1.py', 'Write-LaptopRomUpdateV1.py'):
        shutil.copyfile(TOOLS / t, d / t)
    (d / 'Write-LaptopRomV1.py').write_text(OLD_V1_WORKER)
    spec = importlib.util.spec_from_file_location('wu_old', d / 'Write-LaptopRomUpdateV1.py'); w = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(w)
    assert not hasattr(w.W.Guard, 'set_plan')
    g = w.UpdateGuard()
    g.programs = {parts['userdata']}          # what the old FirehoseDevice.program does just before sending
    try:
        g.check(L.program_xml(*parts['userdata'])); raise AssertionError('old base: unplanned program accepted')
    except ValueError:
        pass
    plan = {(s, c) for _, s, c in U.update_writes({k: v[1] for k, v in NEW.items()})}
    g.set_plan(plan)
    st, c = sorted(plan)[0]; g.programs = {(st, c)}
    assert g.check(L.program_xml(st, c)) == 'program'
    g.programs = {parts['metadata']}
    try:
        g.check(L.program_xml(*parts['metadata'])); raise AssertionError('old base: metadata program accepted')
    except ValueError:
        pass
    g.programs = {parts['boot']}              # a planned partition start but not the planned length
    try:
        g.check(L.program_xml(parts['boot'][0], 8)); raise AssertionError('old base: unplanned boot range accepted')
    except ValueError:
        pass


FAKE_ADB = r'''#!/usr/bin/env python3
import os, subprocess, sys
root = os.environ['FAKE_ROOT']
a = sys.argv[1:]
assert a[0] == '-s' and a[1] == os.environ.get('FAKE_SERIAL', 'HLTE730T-PROBE'), a
kind, cmd = a[2], a[3]
with open(root + '/adb.log', 'a') as f:
    f.write(kind + ' ' + ' '.join(a[3:]) + '\n')
os.makedirs(root + '/tmp', exist_ok=True)
if kind == 'push':          # r6c: adb push <local> <remote>; FAKE_PUSH_CORRUPT=1 flips one byte on the "phone" side
    assert a[4].startswith('/tmp/'), a
    data = bytearray(open(a[3], 'rb').read())
    if os.environ.get('FAKE_PUSH_CORRUPT') == '1' and data:
        data[-1] ^= 0xff
    open(root + a[4], 'wb').write(data)
    sys.exit(0)
assert kind in ('shell', 'exec-out'), 'transport must not use ' + kind
cmd = cmd.replace('/system/bin/toybox ', '').replace('/tmp/', root + '/tmp/')
for p in ('/sys/class/block', '/dev/block', '/proc/device-tree', '/proc/modules'):
    cmd = cmd.replace(p, root + p)
cmd = cmd.replace('/proc/sys/vm/drop_caches', root + '/drop_caches')
pre = ('mknod() { ln -sf "$FAKE_ROOT/backing/$3_$4" "$1"; }; blockdev() { :; }; '
       'insmod() { echo "sdhci_msm 1 0" >> "$FAKE_ROOT/proc/modules"; }; reboot() { echo reset >> "$FAKE_ROOT/power.log"; }; '
       'poweroff() { echo off >> "$FAKE_ROOT/power.log"; }; ')
sys.exit(subprocess.run(['bash', '-c', pre + cmd]).returncode)
'''


def fake_recovery(disk, geometry_bug=None):
    root = work / ('fake-' + disk.stem); (root / 'backing').mkdir(parents=True); (root / 'dev/block').mkdir(parents=True)
    (root / 'proc/device-tree/chosen').mkdir(parents=True); (root / 'proc/modules').write_text('')
    (root / 'proc/device-tree/chosen/hisense,a6l-image').write_bytes(b'v74\0')
    sysb = root / 'sys/class/block'
    d = sysb / 'mmcblk1'; d.mkdir(parents=True)
    (d / 'dev').write_text('179:0\n'); (d / 'size').write_text('%d\n' % TOTAL); (d / 'uevent').write_text('DEVTYPE=disk\nDEVNAME=mmcblk1\n')
    shutil.copyfile(disk, root / 'backing/179_0')
    for i, n in enumerate(NAMES, 1):
        st, cnt = parts[n]
        if n == geometry_bug:
            st += 8
        p = sysb / f'mmcblk1p{i}'; p.mkdir()
        (p / 'dev').write_text(f'179:{i}\n'); (p / 'size').write_text(f'{cnt}\n'); (p / 'start').write_text(f'{st}\n')
        (p / 'uevent').write_text(f'DEVTYPE=partition\nPARTN={i}\nPARTNAME={n}\n')
        with open(disk, 'rb') as f:
            f.seek(parts[n][0] * 512); (root / f'backing/179_{i}').write_bytes(f.read(cnt * 512))
    adb = root / 'adb'; adb.write_text(FAKE_ADB); adb.chmod(0o755)
    os.environ['FAKE_ROOT'] = str(root)
    return root, adb


def fake_snap(root):
    return {n: hashlib.sha256((root / f'backing/179_{i}').read_bytes()).hexdigest() for i, n in enumerate(NAMES, 1)}


def t_adb_transport(backup_policy='snapshot'):
    tag = 'adb' if backup_policy == 'snapshot' else 'adbhash'
    disk = installed_disk(tag)
    root, adb = fake_recovery(disk)
    before = fake_snap(root)
    dev = A.AdbRecoveryDevice('HLTE730T-PROBE', adb=str(adb))
    assert dev.preflight() == 'mmcblk1'
    assert '179_0' in os.readlink(root / 'dev/block/a6lupd/disk')
    r = upd(dev, cap(tag+'-bo'), mode='backup-only')
    assert r['rehearsal_passed'] and fake_snap(root) == before and not progs(dev)
    c = cap(tag+'-upd')
    r = upd(dev, c, backup_policy=backup_policy)
    assert r['readback_verified'] and r['invariants_unchanged'], r
    if backup_policy == 'hash-only':
        assert not r['backup_complete']
        assert all(not (c/(n+'.bin')).exists() for n in U.UPDATE_WRITABLE)
        assert any(row[0] == 'hash' for row in dev.log)
    after = fake_snap(root)
    for n in NAMES:
        if n in U.UPDATE_WRITABLE:
            assert hashlib.sha256((root / f'backing/179_{NAMES.index(n) + 1}').read_bytes()[:NEW[n][1]]).hexdigest() == NEW[n][2], n
        else:
            assert after[n] == before[n], 'adb transport changed ' + n
    assert (root / 'power.log').read_text().split() == ['reset', 'off']
    log = (root / 'adb.log').read_text()
    assert 'of=/dev/block/a6lupd/disk' not in log and 'of=/dev/block/a6lupd/userdata' not in log
    assert 'exec-in' not in log, 'r6c: programs never stream through adb exec-in (it truncated the tail on the phone)'
    assert log.count('push ') >= 3 and 'conv=notrunc' in log, 'programs = push + on-phone dd'
    n0 = log.count('push ')
    for name in ('userdata', 'metadata', 'persist', 'misc', 'recovery'):
        try:
            dev.program(parts[name][0], parts[name][1], iter([bytes(parts[name][1] * 512)])); raise AssertionError('adb program ' + name)
        except A.AdbStop:
            pass
    assert (root / 'adb.log').read_text().count('push ') == n0, 'a refused program reached adb'


def t_adb_chunked_program():
    """r6c: multi-chunk program (tiny CHUNK) lands byte-exact; a chunk corrupted in transit is refused before its dd."""
    disk = installed_disk('adbchunk')
    root, adb = fake_recovery(disk)
    dev = A.AdbRecoveryDevice('HLTE730T-PROBE', adb=str(adb))
    dev.preflight()
    old_chunk = A.CHUNK
    try:
        A.CHUNK = 1 << 20
        vi = NAMES.index('vendor') + 1
        n = parts["vendor"][1] * 512                     # 64 KiB in the mini layout: one 1 MiB chunk
        payload = bytes(random.Random(7).getrandbits(8) for _ in range(n))
        # stream in odd-sized pieces: the device re-chunks
        pieces = [payload[i:i + 3001] for i in range(0, n, 3001)]
        dev.program(parts['vendor'][0], parts['vendor'][1], iter(pieces))
        assert (root / f'backing/179_{vi}').read_bytes()[:n] == payload
        assert not (root / 'tmp/a6l-upd-chunk').exists(), 'chunk left on the phone'
        # several chunks: shrink CHUNK below the payload (must stay MiB aligned -> emulate with MIB = 4 KiB)
        old_mib = A.MIB
        A.MIB, A.CHUNK = 4096, 16384
        try:
            payload2 = bytes(random.Random(8).getrandbits(8) for _ in range(n))
            dev.program(parts['vendor'][0], parts['vendor'][1], iter([payload2[i:i + 5000] for i in range(0, n, 5000)]))
            assert (root / f'backing/179_{vi}').read_bytes()[:n] == payload2
            assert sum(1 for l in (root / 'adb.log').read_text().splitlines() if l.startswith('push ')) >= 1 + n // 16384
            before = (root / f'backing/179_{vi}').read_bytes()
            os.environ['FAKE_PUSH_CORRUPT'] = '1'
            try:
                dev.program(parts['vendor'][0], parts['vendor'][1], iter([payload])); raise AssertionError('corrupt push accepted')
            except A.AdbStop as e:
                assert 'differs on the phone' in str(e), e
            finally:
                os.environ.pop('FAKE_PUSH_CORRUPT', None)
            assert (root / f'backing/179_{vi}').read_bytes() == before, 'a corrupted chunk was written'
            try:
                dev.program(parts['vendor'][0], parts['vendor'][1], iter([payload[:-512]])); raise AssertionError('short accepted')
            except A.AdbStop as e:
                assert 'shorter' in str(e), e
        finally:
            A.MIB = old_mib
    finally:
        A.CHUNK = old_chunk


def t_adb_geometry_refused():
    disk = installed_disk('adbgeo')
    root, adb = fake_recovery(disk, geometry_bug='vendor')
    dev = A.AdbRecoveryDevice('HLTE730T-PROBE', adb=str(adb))
    try:
        dev.preflight(); raise AssertionError('geometry mismatch accepted')
    except A.AdbStop as e:
        assert 'geometry differs for vendor' in str(e), e
    assert not (root / 'dev/block/a6lupd').exists()


def t_worker_dry_run():
    kit = work / 'kit'; (kit / 'images').mkdir(parents=True); (kit / 'update').mkdir()
    for t in ('RomFlashLayoutV1.py', 'RomFlashEngineV1.py', 'Write-LaptopRomV1.py', 'RomUpdateLayoutV1.py', 'RomUpdateEngineV1.py',
              'RomUpdateAdbV1.py', 'Write-LaptopRomUpdateV1.py'):
        shutil.copyfile(TOOLS / t, kit / t)
    def pins(imgs, tag):
        return {'rom': tag, 'images': {n: {'file': Path(p).name, 'bytes': b, 'sha256': s} for n, (p, b, s) in imgs.items()}}
    for n, (p, _, _) in NEW.items():
        shutil.copyfile(p, kit / 'images' / Path(p).name)
    (kit / 'images/rom-v1-pins.json').write_text(json.dumps(pins(NEW, 'new')))
    (kit / 'update/prev-rom-v1-pins.json').write_text(json.dumps(pins(PREV, 'prev')))
    (kit / 'update/rom-update-compat.json').write_text(json.dumps(dict(NEW_C, images={n: v[2] for n, v in NEW.items()})))
    (kit / 'update/prev-rom-update-compat.json').write_text(json.dumps(dict(PREV_C, images={n: v[2] for n, v in PREV.items()})))
    sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
    (kit / 'update/rom-update-tools.json').write_text(json.dumps({'files': {t: sha(kit / t) for t in os.listdir(kit) if t.endswith('.py')}}))
    (kit / 'update/SHA256SUMS').write_text(''.join(f'{sha(kit / "update" / f)}  {f}\n' for f in sorted(os.listdir(kit / 'update'))))
    r = subprocess.run([sys.executable, str(kit / 'Write-LaptopRomUpdateV1.py'), '--mode', 'update', '--dry-run'], capture_output=True, text=True)
    out = json.loads(r.stdout)
    assert r.returncode == 0 and out['passed'] and [p[0] for p in out['plan']] == ['boot', 'dtbo', 'vendor', 'system'], (r.stdout, r.stderr)
    # tampered tool -> refused
    with open(kit / 'RomUpdateLayoutV1.py', 'a') as f:
        f.write('\n# tamper\n')
    r = subprocess.run([sys.executable, str(kit / 'Write-LaptopRomUpdateV1.py'), '--mode', 'update', '--dry-run'], capture_output=True, text=True)
    assert r.returncode == 1 and 'tool differs' in r.stdout, r.stdout


def t_hash_only_update():
    disk = installed_disk('hash-only'); before = snap(disk)
    dev = E.FileDevice(disk); c = cap('hash-only-run')
    r = upd(dev, c, backup_policy='hash-only')
    assert r['readback_verified'] and r['invariants_unchanged'] and r['predecessor_verified']
    assert r['backup_complete'] is False and r['backup_policy'] == 'hash-only'
    assert all(not (c / (n+'.bin')).exists() for n in U.UPDATE_WRITABLE)
    manifest = json.loads((c/'backup-manifest.json').read_text())
    assert manifest['kind'] == 'rom-update-hashes' and manifest['backups'] == {}
    after = snap(disk)
    assert all(before[n] == after[n] for n in PROTECTED)
    assert all(head_sha(disk,n,NEW[n][1]) == NEW[n][2] for n in U.UPDATE_WRITABLE)
    rollback_dev = E.FileDevice(disk)
    try:
        UE.run_rollback(rollback_dev, cap('hash-only-rollback'), c, KIT, {}, lambda:None)
        raise AssertionError('hash manifest accepted as a rollback snapshot')
    except E.StopRun:
        assert not progs(rollback_dev)


def t_hash_only_unknown_refused():
    disk = installed_disk('hash-bad')
    with disk.open('r+b') as f:
        f.seek(parts['vendor'][0]*L.SECTOR); f.write(b'bad head')
    dev = E.FileDevice(disk)
    try:
        upd(dev, cap('hash-bad-run'), backup_policy='hash-only')
        raise AssertionError('unknown installed image accepted')
    except E.StopRun as error:
        assert 'expected previous' in str(error) and not progs(dev)


check('hash_only_update_keeps_data_without_snapshots', t_hash_only_update)
check('hash_only_unknown_image_refused', t_hash_only_unknown_refused)
check('update_keeps_data_and_protected_regions', lambda: t_update_keeps_data() and None)
check('update_writes_only_changed', t_update_writes_only_changed)
check('backup_only_rehearsal_no_write', t_backup_only)
check('rollback_keeps_data', t_rollback)
check('rollback_after_failed_update', t_rollback_after_failed_update)
check('refusals_without_any_write', t_refusals)
check('plan_allowlist', t_plan_allowlist)
check('worker_update_guard', t_worker_guard)
check('worker_update_guard_old_base', t_worker_guard_old_base)
check('adb_transport_update', t_adb_transport)
check('adb_transport_hash_only_update', lambda: t_adb_transport('hash-only'))
check('adb_chunked_program', t_adb_chunked_program)
check('adb_geometry_refused', t_adb_geometry_refused)
check('worker_dry_run', t_worker_dry_run)
shutil.rmtree(work, ignore_errors=True)
print('ROM_UPDATE_TESTS', 'PASS' if all(v == 'pass' for v in results.values()) else 'FAIL', json.dumps(results))
sys.exit(0 if all(v == 'pass' for v in results.values()) else 1)
