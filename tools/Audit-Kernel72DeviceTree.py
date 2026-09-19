"""Verify minimal 7.2 A6L DT retains the complete prior 7.2 board definition."""
import hashlib
import json
from pathlib import Path
from a6l_fdt import read_fdt, cells

ROOT = Path(__file__).resolve().parents[1]
OLD = ROOT / 'firmware/extracted/recovery-probe-storage-noreset-v33-20260917/base.dtb'
OUT = ROOT / 'firmware/extracted/baseline-7.2-kernel-20260917'
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


a, b = canonical(old), canonical(new)
assert a == b, [p for p in sorted(a.keys() | b.keys()) if a.get(p) != b.get(p)]
assert new['/']['compatible'] == b'hisense,hlte730t\0qcom,sdm660\0'
assert '/__symbols__' in new and 'a6l_recovery_chosen' in new['/__symbols__']
for node in ['/remoteproc/glink-edge/rpm-requests/regulators-0/l4', '/remoteproc/glink-edge/rpm-requests/regulators-1/l8']:
    assert 'regulator-allow-set-load' not in new[node]
report = dict(passed=True, base_sha256=hashlib.sha256(data).hexdigest(), previous_sha256=hashlib.sha256(OLD.read_bytes()).hexdigest(), all_nodes_semantically_identical=True, explanation='Same 7.2 board definition as V33; packaging will add only the two V35 eMMC supply permissions.')
(OUT / 'device-tree-audit.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report))
