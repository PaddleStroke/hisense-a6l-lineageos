"""Create the V17 DT-only packaging and validation tools without USB access."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'

def write(name, source):
    target = TOOLS / name
    assert not target.exists(), target
    target.write_bytes(source.encode())

source = (TOOLS / 'build-a6l-usb-only-dtb.sh').read_text()
source = source.replace('# Preserve the compiled V11 DTB. This target has a separate name.',
                        '# Preserve prior DT artifacts; build a separately named V17 target.')
source = source.replace('sdm660-hisense-a6l-recovery.dts; do',
                        'sdm660-hisense-a6l-recovery.dts sdm660-hisense-a6l-usb-only.dts; do')
source = source.replace('cp "$workspace/device/hisense/a6l/kernel/sdm660-hisense-a6l-usb-only.dts"',
                        'cp "$workspace/device/hisense/a6l/kernel/sdm660-hisense-a6l-usb-load.dts"')
source = source.replace('qcom/sdm660-hisense-a6l-usb-only.dtb', 'qcom/sdm660-hisense-a6l-usb-load.dtb')
write('build-a6l-usb-load-dtb.sh', source)

source = (TOOLS / 'Package-RecoveryUsbOnly.py').read_text()
for before, after in [
    ('change only the eMMC DT status from V11', 'apply stock pm660_l10 load to V16'),
    ('recovery-probe-regulator-constraints-20260916', 'recovery-probe-staged-usb-v16-20260916'),
    ('recovery-probe-usb-only-20260916', 'recovery-probe-usb-load-v17-20260916'),
    ('54ec8ad057bbb4bd52c63ad2c82ee68efd0a4828ff674cfdc51acbb41a12259a', '5b28bc987c7f0962662d5a7b72693d177b456405b20f998fc3235576225317f5'),
    ('sdm660-hisense-a6l-usb-only.dtb', 'sdm660-hisense-a6l-usb-load.dtb'),
    ('4cc5b6b159bdcef531a4e2916d2e4b7a40598e666f11b61625901d8f41a62ca8', '7bb565ac584a8ec05fa95145e22912a018c240bef7ffd7665dc62113f6587b70'),
    ('adc5b3effef201c73725d61ac7c0afd920a1e50394404dea0e2863f46b0da774', '74001cd3ac32ca6875fdfae532b6b43bee00467b6f9550d8258f7d2bcf0860a5'),
    ('V11 with eMMC controller disabled; identical kernel, RAM filesystem, USB/regulator wiring, overlay, command line and load addresses', 'V16 with stock-matched pm660_l10 14000 uA system load and load-setting permission; identical voltages, kernel, RAM, USB wiring, overlay, command line and load addresses'),
]:
    assert before in source, before
    source = source.replace(before, after)
start = source.index("    target = ('/soc@0/mmc@c0c4000', 'status')")
end = source.index('    old_payload =', start)
source = source[:start] + '''    targets = [n for n in before if before[n].get('regulator-name') == b'pm660_l10\\0']
    assert len(targets) == 1, targets
    target = targets[0]
    assert set(changes) == {(target, 'regulator-system-load'), (target, 'regulator-allow-set-load')}, changes
    assert 'regulator-system-load' not in before[target]
    assert 'regulator-allow-set-load' not in before[target]
    assert after[target]['regulator-system-load'] == (14000).to_bytes(4, 'big')
    assert after[target]['regulator-allow-set-load'] == b''
''' + source[end:]
write('Package-RecoveryUsbLoad.py', source)
source = (TOOLS / 'Test-RecoveryUsbEventTrace.py').read_text().replace(
    'recovery-probe-staged-usb-v16-20260916', 'recovery-probe-usb-load-v17-20260916')
write('Test-RecoveryUsbLoad.py', source)
print('Created V17 DT-only build, package and ABL validation tools')
