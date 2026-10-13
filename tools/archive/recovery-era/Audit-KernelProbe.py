#!/usr/bin/env python3
"""Crosscheck compiled prototype wiring against both verified stock DTB variants."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import struct
from a6l_fdt import cells, read_fdt, strings


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def phandles(nodes):
    return {cells(p['phandle'])[0]: path for path, p in nodes.items() if 'phandle' in p}


def reservations(nodes):
    result = []
    for path, props in nodes.items():
        if not path.startswith('/reserved-memory/') or path.count('/') != 2 or 'reg' not in props:
            continue
        data = cells(props['reg'])
        if len(data) != 4:
            raise ValueError('Expected one 64-bit address/size reservation')
        start, size = (data[0] << 32) | data[1], (data[2] << 32) | data[3]
        if size:
            result.append((start, start + size, path))
    return sorted(result)


def covered(start, end, spans):
    cursor = start
    for low, high, _ in spans:
        if low <= cursor:
            cursor = max(cursor, high)
        if cursor >= end:
            return True
    return False


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('stock', type=Path)
p.add_argument('kernel_output', type=Path)
p.add_argument('completed_log', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
if 'A6L_KERNEL_PROBE_BUILD_SUCCESS' not in a.completed_log.read_text()[-4096:]:
    raise SystemExit('Kernel build has not completed')
dtb = a.kernel_output / 'arch/arm64/boot/dts/qcom/sdm660-hisense-a6l-probe.dtb'
image = a.kernel_output / 'arch/arm64/boot/Image'
compressed = a.kernel_output / 'arch/arm64/boot/Image.gz'
nodes = read_fdt(dtb.read_bytes())
refs = phandles(nodes)
spans = reservations(nodes)
checks = {'compressed_image_matches': gzip.decompress(compressed.read_bytes()) == image.read_bytes(),
          'arm64_image_magic': image.read_bytes()[56:60] == b'ARM\x64',
          'reserved_regions_do_not_overlap': all(x[1] <= y[0] for x, y in zip(spans, spans[1:]))}
stock_report = json.loads((a.stock / 'report.json').read_text())
comparisons = []
for entry in stock_report['merged']:
    path = a.stock / entry['base'].replace('.dtb', '-merged.dtb')
    if sha(path) != entry['sha256']:
        raise SystemExit('Merged stock tree hash mismatch')
    original = read_fdt(path.read_bytes())
    old_refs = phandles(original)
    result = {}
    pairs = [('/soc/sdhci@c0c4000', '/soc@0/mmc@c0c4000'),
             ('/soc/qusb@c012000', '/soc@0/phy@c012000'),
             ('/soc/serial@0c170000', '/soc@0/serial@c170000'),
             ('/soc/ssusb@a800000/dwc3@a800000', '/soc@0/usb@a8f8800/usb@a800000')]
    for old_path, new_path in pairs:
        before, after = original[old_path], nodes[new_path]
        result[new_path + ':controller_address'] = cells(before['reg'])[0] == cells(after['reg'])[0]
        if 'interrupts' in before and 'interrupts' in after:
            result[new_path + ':gic_interrupt_ids'] = cells(before['interrupts'])[1::3] == cells(after['interrupts'])[1::3]
    supply_pairs = [('/soc/sdhci@c0c4000', '/soc@0/mmc@c0c4000', 'vdd-supply', 'vmmc-supply', 'qcom,vdd-voltage-level'),
                    ('/soc/sdhci@c0c4000', '/soc@0/mmc@c0c4000', 'vdd-io-supply', 'vqmmc-supply', 'qcom,vdd-io-voltage-level'),
                    ('/soc/qusb@c012000', '/soc@0/phy@c012000', 'vdd-supply', 'vdd-supply', None),
                    ('/soc/qusb@c012000', '/soc@0/phy@c012000', 'vdda18-supply', 'vdda-pll-supply', None),
                    ('/soc/qusb@c012000', '/soc@0/phy@c012000', 'vdda33-supply', 'vdda-phy-dpdm-supply', None)]
    for old_path, new_path, old_key, new_key, voltage_key in supply_pairs:
        old_supply = original[old_refs[cells(original[old_path][old_key])[0]]]
        new_supply = nodes[refs[cells(nodes[new_path][new_key])[0]]]
        label = new_path + ':' + new_key
        result[label + ':regulator_identity'] = old_supply['regulator-name'] == new_supply['regulator-name']
        wanted = (cells(original[old_path][voltage_key]) if voltage_key else
                  [cells(old_supply['regulator-min-microvolt'])[0], cells(old_supply['regulator-max-microvolt'])[0]])
        actual = [cells(new_supply['regulator-min-microvolt'])[0], cells(new_supply['regulator-max-microvolt'])[0]]
        result[label + ':voltage_constraints'] = wanted == actual
    result['emmc_bus_width'] = original['/soc/sdhci@c0c4000']['qcom,bus-width'] == nodes['/soc@0/mmc@c0c4000']['bus-width']
    result['usb_high_speed_matches_overlay'] = (original[pairs[-1][0]]['maximum-speed'] ==
                                                nodes[pairs[-1][1]]['maximum-speed'] == b'high-speed\0')
    result['all_stock_fixed_reservations_covered'] = all(covered(lo, hi, spans) for lo, hi, _ in reservations(original))
    comparisons.append({'stock_variant': path.name, 'checks': result, 'passed': all(result.values())})
checks['both_stock_variants_match_selected_wiring'] = all(x['passed'] for x in comparisons)
for label, path in {
    'lcd_mdss': '/soc@0/display-subsystem@c900000',
    'gpu': '/soc@0/gpu@5000000',
    'modem': '/soc@0/remoteproc@4080000',
    'adsp': '/soc@0/remoteproc@15700000',
    'cdsp': '/soc@0/remoteproc@1a300000',
    'charger': '/soc@0/spmi@800f000/pmic@0/charger@1000',
}.items():
    checks[label + '_disabled'] = path in nodes and nodes[path].get('status') == b'disabled\0'
config = dict(line.split('=', 1) for line in (a.kernel_output / '.config').read_text().splitlines()
              if line.startswith('CONFIG_') and '=' in line)
required = 'BPF_SYSCALL BPF_JIT CGROUP_BPF ANDROID_BINDER_IPC ANDROID_BINDERFS USB_DWC3 USB_DWC3_QCOM USB_GADGET USB_CONFIGFS USB_CONFIGFS_ACM PHY_QCOM_QUSB2 MMC_SDHCI_MSM SERIAL_MSM SERIAL_MSM_CONSOLE QCOM_SMEM QCOM_SMD_RPM REGULATOR_QCOM_SMD_RPM PINCTRL_SDM660 HWSPINLOCK_QCOM INTERCONNECT_QCOM_SDM660 BLK_DEV_INITRD DEVTMPFS ARM64_4K_PAGES SERIAL_AMBA_PL011 SERIAL_AMBA_PL011_CONSOLE'.split()
checks['required_drivers_built_in'] = all(config.get('CONFIG_' + key) == 'y' for key in required)
checks['enough_emmc_partition_minors'] = int(config.get('CONFIG_MMC_BLOCK_MINORS', '0')) >= 64
checks['no_rmtfs_or_qseecom_or_inline_crypto'] = all(config.get('CONFIG_' + key, 'n') == 'n'
                                                 for key in ['QCOM_RMTFS_MEM', 'QCOM_QSEECOM', 'MMC_CRYPTO'])
a.output.mkdir(parents=True, exist_ok=False)
files = []
for source, name in [(image, 'Image'), (compressed, 'Image.gz'), (dtb, dtb.name),
                     (a.kernel_output / '.config', 'kernel.config'),
                     (a.kernel_output / 'System.map', 'System.map'),
                     (a.completed_log, 'build.log')]:
    dest = a.output / name
    shutil.copyfile(source, dest)
    files.append({'file': name, 'bytes': dest.stat().st_size, 'sha256': sha(dest), 'source_sha256': sha(source)})
checks['preserved_artifacts_match'] = all(x['sha256'] == x['source_sha256'] for x in files)
report = {'scope': 'Offline wiring/config/image audit; not a phone boot test',
          'kernel_release': (a.kernel_output / 'include/config/kernel.release').read_text().strip(),
          'source_revision': 'e47d622cb6d2440a9eacdc8bb2df32c037bec7b8',
          'checks': checks, 'all_checks_passed': all(checks.values()), 'stock_comparisons': comparisons,
          'artifacts': files,
          'limits': ['Bootloader memory fixups and temporary RAM boot remain unverified',
                     'Inherited extra reservations are retained; only stock fixed-region coverage was checked',
                     'Regulator parent graph, PHY tuning and runtime sequencing need hardware validation',
                     'No display, e-ink, Android vendor ABI or mobile-radio compatibility is established']}
(a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'all_checks_passed': report['all_checks_passed'], 'checks': checks,
                  'report': str(a.output / 'report.json')}, indent=2))
raise SystemExit(0 if report['all_checks_passed'] else 1)
