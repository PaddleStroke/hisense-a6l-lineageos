#!/usr/bin/env python3
"""Offline supplier-reference map; not proof of successful runtime probing."""
import fnmatch
import hashlib
import json
from pathlib import Path
import re
from a6l_fdt import cells, read_fdt, strings

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path('/home/a6l/kernel/out-a6l-probe')
OUT = ROOT / 'firmware/extracted/probe-dependencies-20260915'
dtb = ROOT / 'firmware/extracted/recovery-probe-visible-userspace-20260915/base.dtb'
nodes = read_fdt(dtb.read_bytes())
handles = {cells(p['phandle'])[0]: n for n, p in nodes.items() if 'phandle' in p}
aliases = []
metadata = (BUILD / 'modules.builtin.modinfo').read_bytes()
for item in metadata.split(b'\0'):
    value = item.decode(errors='replace')
    if '.alias=of:' in value:
        module, pattern = value.split('.alias=', 1)
        aliases.append((module, pattern))

args = {'clocks': '#clock-cells', 'resets': '#reset-cells',
        'power-domains': '#power-domain-cells', 'phys': '#phy-cells',
        'iommus': '#iommu-cells', 'interconnects': '#interconnect-cells',
        'hwlocks': '#hwlock-cells', 'io-channels': '#io-channel-cells',
        'interrupts-extended': '#interrupt-cells', 'qcom,smem-states': '#qcom,smem-state-cells'}
plain = {'memory-region', 'nvmem-cells', 'interrupt-parent'}
edges, errors = [], []
for node, props in nodes.items():
    for key, data in props.items():
        if key not in args and key not in plain and not key.endswith('-supply') and not re.fullmatch(r'pinctrl-\d+', key):
            continue
        try:
            values = cells(data)
            i = 0
            while i < len(values):
                handle = values[i]
                i += 1
                if handle == 0:
                    continue
                provider = handles[handle]
                count = cells(nodes[provider][args[key]])[0] if key in args else 0
                assert i + count <= len(values)
                edges.append({'consumer': node, 'property': key,
                              'provider': provider, 'arguments': values[i:i+count]})
                i += count
        except (KeyError, ValueError, AssertionError) as error:
            errors.append({'node': node, 'property': key, 'error': repr(error)})

roots = [n for n in nodes if n.endswith(('/phy@c012000', '/usb@a800000', '/mmc@c0c4000', '/pmic@0/temp-alarm@2400'))]
assert len(roots) == 4, roots
selected, pending = set(), list(roots)
while pending:
    node = pending.pop()
    if node in selected:
        continue
    selected.add(node)
    pending.extend(e['provider'] for e in edges if e['consumer'] == node)
    if node != '/':
        pending.append(node.rsplit('/', 1)[0] or '/')

inventory = {}
for node in sorted(selected):
    props = nodes[node]
    compat = strings(props.get('compatible', b''))
    name = node.rsplit('/', 1)[-1].split('@')[0]
    matches = sorted({module for module, pattern in aliases for comp in compat
                      if fnmatch.fnmatchcase(f'of:N{name}T(null)C{comp}', pattern)})
    inventory[node] = {'compatible': compat, 'status': strings(props.get('status', b'okay\0')),
                       'builtin_alias_matches': matches,
                       'regulator_name': strings(props.get('regulator-name', b''))}

config = dict(line.split('=', 1) for line in (BUILD / '.config').read_text().splitlines()
              if line.startswith('CONFIG_') and '=' in line)
keys = ['QCOM_SMEM', 'QCOM_SMD_RPM', 'RPMSG_QCOM_SMD', 'RPMSG_QCOM_GLINK_SMEM',
        'RPMSG_QCOM_GLINK_RPM', 'REGULATOR_QCOM_SMD_RPM', 'QCOM_CLK_SMD_RPM', 'SDM_GCC_660',
        'INTERCONNECT_QCOM_SDM660', 'NVMEM_QCOM_QFPROM', 'QCOM_SPMI_ADC5', 'QCOM_SPMI_VADC',
        'QCOM_VADC_COMMON', 'QCOM_SPMI_TEMP_ALARM', 'SPMI_MSM_PMIC_ARB', 'MFD_SPMI_PMIC',
        'USB_DWC3', 'USB_DWC3_QCOM', 'PHY_QCOM_QUSB2', 'MMC_SDHCI_MSM']
report = {'scope': 'Explicit DT supplier references and parent ancestry for four observed deferred devices; no runtime success inferred',
          'dtb_sha256': hashlib.sha256(dtb.read_bytes()).hexdigest(), 'roots': roots,
          'nodes': inventory, 'edges': [e for e in edges if e['consumer'] in selected],
          'config': {k: config.get('CONFIG_' + k, 'n/absent') for k in keys},
          'parse_errors': errors,
          'limits': ['Built-in alias matches are candidates, not evidence of runtime binding',
                     'No alias can also mean a built-in declaration or child device handled by its parent',
                     'Dependencies requested dynamically by drivers are not fully represented',
                     'Disabled nodes and parent status require interpretation, not automatic patching']}
OUT.mkdir(exist_ok=True)
(OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
(OUT / 'modules.builtin.modinfo').write_bytes(metadata)
(OUT / 'kernel.config').write_bytes((BUILD / '.config').read_bytes())
lines = ['# Diagnostic supplier map', '', 'Generated from the exact V10 DTB and current built-in driver metadata.', '',
         '| Node | Compatible | Built-in alias matches |', '| --- | --- | --- |']
for node, entry in inventory.items():
    lines.append(f"| `{node}` | {', '.join(entry['compatible'])} | {', '.join(entry['builtin_alias_matches']) or 'No match; inspect parent/source'} |")
lines += ['', '## Explicit dependency edges', '']
for edge in report['edges']:
    lines.append(f"- `{edge['consumer']}` → `{edge['provider']}` via `{edge['property']}`, arguments {edge['arguments']}.")
lines += ['', '## Limits', '', *['- ' + item for item in report['limits']]]
(OUT / 'report.md').write_text('\n'.join(lines) + '\n')
print(json.dumps({'nodes': len(inventory), 'edges': len(report['edges']), 'parse_errors': errors, 'config': report['config']}, indent=2))
