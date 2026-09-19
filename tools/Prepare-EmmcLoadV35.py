"""Derive DT-only V35 packaging/ABL checks from the checked 6.19 baseline."""
from pathlib import Path
import py_compile
ROOT = Path(__file__).resolve().parents[1]
T = ROOT / 'tools'
OLD = 'recovery-probe-baseline-v34-20260917'
NEW = 'recovery-probe-emmc-load-v35-20260917'
s=(T/'Package-RecoveryBaseline.py').read_text()
s=s.replace("OLD = ROOT / 'firmware/extracted/recovery-probe-storage-noreset-v33-20260917'",
            "OLD = ROOT / 'firmware/extracted/"+OLD+"'")
s=s.replace("OUT = ROOT / 'firmware/extracted/"+OLD+"'", "OUT = ROOT / 'firmware/extracted/"+NEW+"'")
s=s.replace('365b880a19edbf7945bc82ca672cc853ea90fd059aaf9a480bd7e43039666305','31e451b709ead424d93e4623922c9f6549f5229795853123be006611f7fa0706')
start=s.index("    base = (KERNEL / 'base.dtb').read_bytes()")
end=s.index('    payload = ',start)
s=s[:start]+'''    before = (OLD / 'base.dtb').read_bytes()
    assert sha(before) == '21e2c1640287016dc755076d7d203be2900c60847ce4219d15b1edef50f7a49c'
    expected = read_fdt(before)
    (OUT / 'base.dtb').write_bytes(before)
    nodes = ['/remoteproc/glink-edge/rpm-requests/regulators-0/l4',
             '/remoteproc/glink-edge/rpm-requests/regulators-1/l8']
    for node in nodes:
        assert 'regulator-allow-set-load' not in expected[node]
        run('fdtput', '-t', 's', OUT / 'base.dtb', node, 'regulator-allow-set-load')
        expected[node]['regulator-allow-set-load'] = b''
    base = (OUT / 'base.dtb').read_bytes()
    assert read_fdt(base) == expected, 'Unexpected change beyond two eMMC load permissions'
''' + s[end:]
s=s.replace('V34 Linux 6.19.10 SDM660 baseline; normal MMC core, A6L boot/USB support, optional LED bypass; same RAM init and board-specific settings as V33',
            'V35 adds only regulator-allow-set-load to the two eMMC supplies. Same V34 6.19.10 kernel, RAM init/module, voltages, clocks and storage path. No fixed extra load votes.')
s=s.replace('"""Package V34 Linux 6.19.10 with audited A6L DT and matching module."""',
            '"""V35: enable the existing eMMC driver current-load requests on both supplies."""')
for name,value in [('Package-RecoveryEmmcLoad.py',s)]:
    p=T/name;assert not p.exists();p.write_bytes(value.encode());py_compile.compile(str(p),doraise=True)
s=(T/'Test-RecoveryBaseline.py').read_text().replace(OLD,NEW)
start=s.index('        audit = ');end=s.index('        merge = ',start)
s=s[:start]+'''        expected_base = read_fdt((ROOT / 'firmware/extracted/recovery-probe-baseline-v34-20260917/base.dtb').read_bytes())
        for node in ['/remoteproc/glink-edge/rpm-requests/regulators-0/l4', '/remoteproc/glink-edge/rpm-requests/regulators-1/l8']:
            assert 'regulator-allow-set-load' not in expected_base[node]
            expected_base[node]['regulator-allow-set-load'] = b''
        assert read_fdt(base) == expected_base
''' + s[end:]
p=T/'Test-RecoveryEmmcLoad.py';assert not p.exists();p.write_bytes(s.encode());py_compile.compile(str(p),doraise=True)
print('V35 two-property DT experiment prepared; no phone access.')
