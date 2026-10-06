#!/usr/bin/env python3
"""V74 -> V75-usb -> V74 recovery transition chain + recovery-partition write simulation (agent usbrec, 26 Sep 2026).
Offline: no USB, no phone. Uses the REAL generated v75u/v75r policy + protocol modules and the real saved device reads of the
last install (laptop: capture-diagnostic-install-user-v74c/edl *-after-write.bin = the phone right after V74 was written;
repo fallback: captures/capture-diagnostic-restore-user-v13/edl devinfo/misc-bcb) on a sparse virtual eMMC with the fixed
Firehose regions of the Write-LaptopDiagnosticRecovery tools. The program operation is parsed from the tools' own PROGRAM_XML.
Run from the directory holding the tools (laptop kit root or repo tools/)."""
import hashlib, importlib, json, sys, tempfile, xml.etree.ElementTree as ET
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sha = lambda b: hashlib.sha256(b).hexdigest()
V71 = '417164b78c3bc7c43eba3039caab240194b5a69000b78737c50c3503f398bcf9'
V74 = '24ede49b0f34b10f216617cb007c757e495fa2df1b6cd1ddd0afd63f9819da62'
V75U = '8ecb8e9e4c5289cfe63c23eb32b749178935a6588673cea3a1b1ebee3c94b304'
STOCK = '9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621'
laptop = (HERE / 'ram-staging').is_dir()
if laptop:
    fx = HERE / 'capture-diagnostic-install-user-v74c/edl'
    reads = {n: (fx / f'{n}-after-write.bin').read_bytes() for n in ('primary', 'tail', 'recovery', 'devinfo', 'misc-bcb', 'vbmeta')}
    img = {'v75u': HERE / 'ram-staging/recovery-diagnostic-staged-usb-v75u.img', 'v75r': HERE / 'ram-staging/recovery-diagnostic-staged-usb-v75r.img',
           'stock': HERE / 'stock-recovery/restore-stock-recovery.img'}
else:
    R = HERE.parent; fx = R / 'captures/capture-diagnostic-restore-user-v13/edl'
    img = {'v75u': R / 'firmware/extracted/recovery-v75usb-candidate-20260925/recovery-diagnostic-unsigned.img',
           'v75r': R / 'firmware/extracted/recovery-v74-candidate-20260923/recovery-diagnostic-unsigned.img',
           'stock': R / 'firmware/extracted/recovery-probe-20260914/restore-stock-recovery.img'}
    reads = {'devinfo': (fx / 'devinfo.bin').read_bytes(), 'misc-bcb': (fx / 'misc-bcb.bin').read_bytes(),
             'primary': (fx / 'primary.bin').read_bytes(), 'tail': (fx / 'tail.bin').read_bytes(),
             'recovery': img['v75r'].read_bytes(), 'vbmeta': None}
payload = {k: p.read_bytes() for k, p in img.items()}
assert sha(payload['v75u']) == V75U and sha(payload['v75r']) == V74 and sha(payload['stock']) == STOCK
assert sha(reads['recovery']) == V74, 'fixture recovery is not V74'
results = {}
def check(name, cond):
    results[name] = bool(cond); print(('PASS ' if cond else 'FAIL ') + name, flush=True)

mods = {}
for tag in ('v75u', 'v75r'):
    T = importlib.import_module('RecoveryTransition' + tag.upper())
    P = importlib.import_module('DiagnosticRecoveryProtocol' + tag.upper())
    spec = importlib.util.spec_from_file_location('w' + tag, HERE / f'Write-LaptopDiagnosticRecovery-user-{tag}.py')
    W = importlib.util.module_from_spec(spec); spec.loader.exec_module(W)
    mods[tag] = (T, P, W)
W = mods['v75u'][2]
assert mods['v75r'][2].REGIONS == W.REGIONS

def accepts(tag, rec_digest):
    try:
        mods[tag][0].verify_transition(rec_digest, reads['misc-bcb'], reads['devinfo']); return True
    except ValueError:
        return False

# --- policy chain on the real saved device state
check('v75u_accepts_current_V74', accepts('v75u', V74))
check('v75r_refuses_current_V74', not accepts('v75r', V74))
for other, label in ((V71, 'V71'), (STOCK, 'stock'), (V75U, 'V75U'), ('0' * 64, 'unknown')):
    check('v75u_refuses_' + label, not accepts('v75u', other))
check('v75r_accepts_V75U', accepts('v75r', V75U))
for other, label in ((V71, 'V71'), (STOCK, 'stock'), (V74, 'V74')):
    check('v75r_refuses_' + label, not accepts('v75r', other))
# --- payload pinning
def payload_ok(tag, mode, data):
    try:
        mods[tag][1].verify_payload(data, mode); return True
    except ValueError:
        return False
check('v75u_install_payload_is_V75U', payload_ok('v75u', 'install-diagnostic', payload['v75u']) and not payload_ok('v75u', 'install-diagnostic', payload['v75r']))
check('v75r_install_payload_is_V74', payload_ok('v75r', 'install-diagnostic', payload['v75r']) and not payload_ok('v75r', 'install-diagnostic', payload['v75u']))
check('restore_payload_is_stock_both', all(payload_ok(t, 'restore-stock', payload['stock']) for t in mods))

# --- write simulation on a sparse virtual eMMC holding the fixed regions
with tempfile.TemporaryDirectory() as td:
    disk = Path(td) / 'emmc.img'
    end = max(o + n for o, n in W.REGIONS.values())
    with open(disk, 'wb') as f:
        f.truncate(end + (1 << 20))
        for name, (off, n) in W.REGIONS.items():
            data = reads.get(name) if name != 'vbmeta' or reads.get('vbmeta') else bytes(n)
            if data is None:
                data = bytes(n)
            assert len(data) == n, (name, len(data), n)
            f.seek(off); f.write(data)
    def read_regions():
        with open(disk, 'rb') as f:
            out = {}
            for name, (off, n) in W.REGIONS.items():
                f.seek(off); out[name] = f.read(n)
            return out
    def simulate(tag, mode='install-diagnostic'):
        T, P, Wm = mods[tag]
        pre = read_regions()
        for name, (off, n) in Wm.REGIONS.items():      # every read the worker is allowed to do
            x = f'<data><read SECTOR_SIZE_IN_BYTES="512" num_partition_sectors="{n // 512}" physical_partition_number="0" start_sector="{off // 512}"/></data>'
            assert Wm.allowed_xml(x) == 'read'
        if mode == 'install-diagnostic':
            T.verify_transition(sha(pre['recovery']), pre['misc-bcb'], pre['devinfo'])
        data = payload[tag] if mode == 'install-diagnostic' else payload['stock']
        digest = P.verify_payload(data, mode)
        assert Wm.allowed_xml(P.PROGRAM_XML) == 'program'
        node = ET.fromstring(P.PROGRAM_XML)[0]
        off = int(node.get('start_sector')) * 512; n = int(node.get('num_partition_sectors')) * 512
        assert (off, n) == Wm.REGIONS['recovery'] and len(data) == n
        with open(disk, 'r+b') as f:
            f.seek(off); f.write(data)
        post = read_regions()
        assert sha(post['recovery']) == digest
        assert all(post[k] == pre[k] for k in post if k != 'recovery'), 'a non-recovery region changed'
        return digest
    try:
        check('sim_install_v75u_over_V74', simulate('v75u') == V75U)
        try:
            simulate('v75u'); check('sim_second_v75u_refused', False)
        except ValueError:
            check('sim_second_v75u_refused', True)
        check('sim_rollback_v75r_to_V74', simulate('v75r') == V74)
        try:
            simulate('v75r'); check('sim_second_v75r_refused', False)
        except ValueError:
            check('sim_second_v75r_refused', True)
        check('sim_reinstall_v75u_after_rollback', simulate('v75u') == V75U)
        check('sim_restore_stock_any_recovery', simulate('v75u', 'restore-stock') == STOCK)
    except Exception as e:
        check('sim_exception ' + repr(e), False)
    for bad in ('<data><erase/></data>', '<data><power value="reset_to_edl"/></data>',
                mods['v75u'][1].PROGRAM_XML.replace('917504', '671744')):
        try:
            W.allowed_xml(bad); check('guard_rejects ' + bad[:40], False)
        except ValueError:
            check('guard_rejects ' + bad[:40], True)
ok = all(results.values())
print(json.dumps({'fixture': str(fx), 'laptop': laptop, 'results': results}, indent=1))
print('V75U_CHAIN', 'PASS' if ok else 'FAIL')
raise SystemExit(0 if ok else 1)
