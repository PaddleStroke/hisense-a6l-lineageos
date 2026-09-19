"""Audit the 6.19 A6L DT against V33, resolving phandles before comparison."""
import hashlib
import json
from pathlib import Path
from a6l_fdt import read_fdt, cells

ROOT = Path(__file__).resolve().parents[1]
OLD = ROOT / 'firmware/extracted/recovery-probe-storage-noreset-v33-20260917/base.dtb'
OUT = ROOT / 'firmware/extracted/baseline-6.19-kernel-20260917'
data = (OUT / 'base.dtb').read_bytes()
old, new = read_fdt(OLD.read_bytes()), read_fdt(data)
DSP = ['/soc@0/remoteproc@15700000', '/soc@0/remoteproc@1a300000']
REFS = {'interrupts-extended': '#interrupt-cells', 'iommus': '#iommu-cells',
        'qcom,smem-states': '#qcom,smem-state-cells', 'thermal-sensors': '#thermal-sensor-cells',
        'cooling-device': '#cooling-cells', 'clocks': '#clock-cells', 'resets': '#reset-cells',
        'power-domains': '#power-domain-cells', 'phys': '#phy-cells', 'interconnects': '#interconnect-cells'}
SINGLE = {'memory-region', 'trip', 'nvmem-cells', 'operating-points-v2',
          'pinctrl-0', 'pinctrl-1', 'interrupt-parent'}


def canonical(tree):
    handles = {cells(p['phandle'])[0]: n for n,p in tree.items() if 'phandle' in p}
    result = {}
    for path, props in tree.items():
        if path == '/__symbols__':
            continue
        values = {}
        for key, raw in props.items():
            if key in ('phandle', 'linux,phandle'):
                continue
            if key in REFS or key in SINGLE or key.endswith('-supply'):
                v = cells(raw); resolved = []
                while v:
                    handle = v.pop(0)
                    if handle == 0:
                        resolved.append([None, []])
                        continue
                    assert handle in handles, (path, key, handle)
                    provider = handles[handle]
                    count = cells(tree[provider][REFS[key]])[0] if key in REFS else 0
                    assert len(v) >= count
                    resolved.append([provider, v[:count]])
                    v = v[count:]
                values[key] = resolved
            else:
                values[key] = raw.hex()
        result[path] = values
    return result


for path in DSP + ['/soc@0/remoteproc@4080000']:
    assert new[path]['status'] == old[path]['status'] == b'disabled\0'
a,b = canonical(old),canonical(new)
changed = []
for path in sorted(a.keys() | b.keys()):
    if a.get(path) == b.get(path):
        continue
    change = {'path': path, 'before': a.get(path), 'after': b.get(path)}
    allowed = any(path.startswith(p + '/') for p in DSP)
    if path in ('/reserved-memory/adsp-region', '/reserved-memory/adsp-region@f6000000'):
        allowed = True
    if path == '/timer':
        before, after = dict(a[path]), dict(b[path])
        av,bv = cells(old[path]['interrupts']),cells(new[path]['interrupts'])
        assert len(av) == len(bv) == 12
        assert [v & ~0xf00 if i % 3 == 2 else v for i,v in enumerate(bv)] == av
        before.pop('interrupts');after.pop('interrupts')
        assert before == after
        allowed = True
    assert allowed, change
    changed.append(change)
assert new['/soc@0/mmc@c0c4000'] == old['/soc@0/mmc@c0c4000']
assert new['/soc@0/phy@c012000'] == old['/soc@0/phy@c012000']
assert new['/']['compatible'] == b'hisense,hlte730t\0qcom,sdm660\0'
assert new['/reserved-memory/adsp-region@f6000000']['reg'] == bytes.fromhex('00000000f60000000000000000800000')
assert 'no-map' in new['/reserved-memory/adsp-region@f6000000']
assert '/__symbols__' in new and 'a6l_recovery_chosen' in new['/__symbols__']
report = {'passed': True, 'base_sha256': hashlib.sha256(data).hexdigest(),
          'previous_sha256': hashlib.sha256(OLD.read_bytes()).hexdigest(),
          'mmc_and_usb_phy_nodes_byte_identical': True,
          'all_other_active_nodes_semantically_identical_except_timer_mask': True,
          'differences': changed,
          'explanation': 'Older branch has fixed ADSP shared-memory reservation, legacy timer CPU mask, and different children under disabled DSPs. All A6L regulator constraints, GPIO reservations, storage/USB wiring and boot display/secure reservations match.'}
(OUT / 'device-tree-audit.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'passed':True, 'changed_nodes':len(changed), 'base_sha256':report['base_sha256']}))
