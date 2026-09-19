"""Prepare V36 checks for minimal Linux 7.2.3 with the successful supply fix."""
import py_compile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
T = ROOT / 'tools'


def save(name, value):
    p = T / name
    assert not p.exists(), p
    p.write_bytes(value.encode())
    py_compile.compile(str(p), doraise=True)


def convert(value):
    return value.replace('baseline-6.19', 'baseline-7.2').replace('6.19.10', '7.2.3')


for old, new in [
    ('Package-BaselineRamdisk.py', 'Package-Kernel72Ramdisk.py'),
    ('Test-BaselineModuleLoad.py', 'Test-Kernel72ModuleLoad.py'),
    ('Test-A6LBaseline.py', 'Test-A6LKernel72.py'),
]:
    save(new, convert((T / old).read_text()))

s = convert((T / 'Package-RecoveryEmmcLoad.py').read_text())
s = s.replace('recovery-probe-baseline-v34-20260917', 'recovery-probe-emmc-load-v35-20260917')
s = s.replace("OUT = ROOT / 'firmware/extracted/recovery-probe-emmc-load-v35-20260917'",
              "OUT = ROOT / 'firmware/extracted/recovery-probe-kernel72-v36-20260917'")
s = s.replace('31e451b709ead424d93e4623922c9f6549f5229795853123be006611f7fa0706',
              '129621db2ac07c8c955426a5a974a4ffc8ba587eb883d0e6c76e6c01a2f76c00')
s = s.replace("before = (OLD / 'base.dtb').read_bytes()", "before = (KERNEL / 'base.dtb').read_bytes()")
s = s.replace("    assert sha(before) == '21e2c1640287016dc755076d7d203be2900c60847ce4219d15b1edef50f7a49c'",
              "    audit = json.loads((KERNEL / 'device-tree-audit.json').read_text())\n"
              "    assert audit['passed'] and sha(before) == audit['base_sha256']")
s = s.replace('a587e4f18b483d0a17579e6325c861b303988bda', 'e47d622cb6d2440a9eacdc8bb2df32c037bec7b8')
s = s.replace('V35 adds only regulator-allow-set-load to the two eMMC supplies. Same V34 7.2.3 kernel, RAM init/module, voltages, clocks and storage path. No fixed extra load votes.',
              'V36 Linux 7.2.3 with the same minimal A6L diagnostic support as V35 and both eMMC load permissions. Matching module; identical RAM init. No experimental MMC command, IRQ or barrier changes.')
s = s.replace('V35: enable the existing eMMC driver current-load requests on both supplies.',
              'V36: compare minimal Linux 7.2.3 with the working V35 supply permissions.')
save('Package-RecoveryKernel72.py', s)

s = (T / 'Test-RecoveryEmmcLoad.py').read_text()
s = s.replace('recovery-probe-emmc-load-v35-20260917', 'recovery-probe-kernel72-v36-20260917')
s = s.replace('recovery-probe-baseline-v34-20260917/base.dtb', 'baseline-7.2-kernel-20260917/base.dtb')
save('Test-RecoveryKernel72.py', s)
print('V36 RAM, module, console and package checks prepared; no phone access.')
