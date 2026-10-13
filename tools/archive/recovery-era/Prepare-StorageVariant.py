"""Prepare V18: enable eMMC only, retaining the verified V17 USB configuration."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'

def write(name, source):
    target = TOOLS / name
    assert not target.exists(), target
    target.write_bytes(source.encode())

source = (TOOLS / 'build-a6l-usb-load-dtb.sh').read_text()
source = source.replace('V17 target', 'V18 target')
source = source.replace('sdm660-hisense-a6l-usb-only.dts; do', 'sdm660-hisense-a6l-usb-only.dts sdm660-hisense-a6l-usb-load.dts; do')
source = source.replace('cp "$workspace/device/hisense/a6l/kernel/sdm660-hisense-a6l-usb-load.dts"', 'cp "$workspace/device/hisense/a6l/kernel/sdm660-hisense-a6l-storage.dts"')
source = source.replace('qcom/sdm660-hisense-a6l-usb-load.dtb', 'qcom/sdm660-hisense-a6l-storage.dtb')
write('build-a6l-storage-dtb.sh', source)

source = (TOOLS / 'Package-RecoveryUsbLoad.py').read_text()
for before, after in [
    ('apply stock pm660_l10 load to V16', 'enable eMMC enumeration on verified V17'),
    ("OLD = ROOT / 'firmware/extracted/recovery-probe-staged-usb-v16-20260916'", "OLD = ROOT / 'firmware/extracted/recovery-probe-usb-load-v17-20260916'"),
    ("OUT = ROOT / 'firmware/extracted/recovery-probe-usb-load-v17-20260916'", "OUT = ROOT / 'firmware/extracted/recovery-probe-storage-v18-20260916'"),
    ('5b28bc987c7f0962662d5a7b72693d177b456405b20f998fc3235576225317f5', '50bd78a2a94a7d151defbe637b28492207bdd6b5f857cc413175b13535658c15'),
    ('sdm660-hisense-a6l-usb-load.dtb', 'sdm660-hisense-a6l-storage.dtb'),
    ('V16 with stock-matched pm660_l10 14000 uA system load and load-setting permission; identical voltages, kernel, RAM, USB wiring, overlay, command line and load addresses', 'V17 with only eMMC controller status enabled; identical voltages, loads, kernel, RAM, USB wiring, overlay, command line and load addresses; RAM init does not open block devices or mount persistent filesystems'),
]:
    assert before in source, before
    source = source.replace(before, after)
start = source.index('    targets = [n for n in before')
end = source.index('    old_payload =', start)
source = source[:start] + '''    target = ('/soc@0/mmc@c0c4000', 'status')
    assert changes == [target], changes
    assert before[target[0]][target[1]] == b'disabled\\0'
    assert after[target[0]][target[1]] == b'okay\\0'
''' + source[end:]
write('Package-RecoveryStorage.py', source)
source = (TOOLS / 'Test-RecoveryUsbLoad.py').read_text().replace('recovery-probe-usb-load-v17-20260916', 'recovery-probe-storage-v18-20260916')
write('Test-RecoveryStorage.py', source)
print('Prepared V18 storage enumeration build and packaging tools')
