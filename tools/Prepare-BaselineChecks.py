"""Derive the established diskless checks for the Linux 6.19.10 baseline."""
from pathlib import Path
import py_compile

ROOT = Path(__file__).resolve().parents[1]
T = ROOT / 'tools'
REPLACEMENTS = {
    'storage-state-kernel-v32-20260917': 'baseline-6.19-kernel-20260917',
    'storage-state-smoke-v32-20260917': 'baseline-6.19-smoke-20260917',
    'storage-state-ramdisk-v32-20260917': 'baseline-6.19-ramdisk-20260917',
    'storage-state-probe-v32-20260917': 'baseline-6.19-probe-20260917',
    '/home/a6l/kernel/out-a6l-probe': '/home/a6l/kernel/out-a6l-baseline-6.19',
}


def convert(s):
    for before, after in REPLACEMENTS.items():
        s = s.replace(before, after)
    return s


def save(name, s):
    p = T / name
    assert not p.exists(), p
    p.write_bytes(s.encode())
    py_compile.compile(str(p), doraise=True)


save('Test-BaselineModuleLoad.py', convert((T / 'Test-StorageStateModuleLoad.py').read_text()))
s = convert((T / 'Test-A6LStorageState.py').read_text())
begin = s.index('        milestones = ')
end = s.index('                      pid1_ready=', begin)
s = s[:begin] + '        checks = dict(' + s[end:].lstrip()
s = s.replace("    assert b'A6L_STORAGE_CHECKPOINT' in image", "    assert b'A6L_USB_CONNECT_HELD' in image\n    assert b'A6L_CMD0_CALLBACK' not in image")
s = s.replace("[('enabled', 'hisense,hlte730t', 0x23ff000)]", "[('enabled', 'hisense,hlte730t', 0x23ff000), ('wrong-board', 'linux,dummy-virt', 0x23ff000), ('short-reservation', 'hisense,hlte730t', 0x1000)]")
save('Test-A6LBaseline.py', s)

s = (T / 'Test-RecoveryStorageNoReset.py').read_text()
s = s.replace('recovery-probe-storage-noreset-v33-20260917', 'recovery-probe-baseline-v34-20260917')
begin = s.index('        previous_tree = ')
end = s.index("        merge = module(", begin)
s = s[:begin] + '''        audit = json.loads((ROOT / 'firmware/extracted/baseline-6.19-kernel-20260917/device-tree-audit.json').read_text())
        assert audit['passed'] and sha(base) == audit['base_sha256']
''' + s[end:]
s = s.replace("assert sha(kernel) == '01e882200d5c99b1199d15697721de50dc0114e3465f62945ead2b68a5ee2c12'", "assert sha(kernel) == package['kernel_sha256']")
save('Test-RecoveryBaseline.py', s)
print('Baseline QEMU module/PID1/console guards and captured-ABL tests prepared.')
