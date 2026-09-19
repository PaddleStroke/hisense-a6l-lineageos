"""Derive V34 packaging from the validated recovery container workflow."""
from pathlib import Path
import py_compile
ROOT = Path(__file__).resolve().parents[1]
T = ROOT / 'tools'
s = (T / 'Package-RecoveryStorageNoReset.py').read_text()
s = s.replace('"""V33: omit only the optional eMMC GCC block-reset DT property; V32 binaries unchanged."""',
              '"""Package V34 Linux 6.19.10 with audited A6L DT and matching module."""')
s = s.replace("OLD = ROOT / 'firmware/extracted/recovery-probe-storage-state-v32-20260917'",
              "OLD = ROOT / 'firmware/extracted/recovery-probe-storage-noreset-v33-20260917'\nKERNEL = ROOT / 'firmware/extracted/baseline-6.19-kernel-20260917'")
for a,b in [('storage-state-ramdisk-v32-20260917','baseline-6.19-ramdisk-20260917'),
            ('storage-state-probe-v32-20260917','baseline-6.19-probe-r2-20260917'),
            ('storage-state-smoke-v32-20260917','baseline-6.19-smoke-20260917')]:
    s = s.replace(a,b)
s = s.replace("OUT = ROOT / 'firmware/extracted/recovery-probe-storage-noreset-v33-20260917'",
              "OUT = ROOT / 'firmware/extracted/recovery-probe-baseline-v34-20260917'")
s = s.replace(" == '01e882200d5c99b1199d15697721de50dc0114e3465f62945ead2b68a5ee2c12'", '')
s = s.replace("assert b'A6L_USB_CONNECT_HELD' in kernel and b'A6L_USB_EVENT_TRACE armed' in kernel and b'A6L_USB_IRQ_BEGIN' in kernel",
              "assert b'A6L_USB_CONNECT_HELD' in kernel and b'A6L_CMD0_CALLBACK' not in kernel\n    assert b'Linux version 6.19.10-a6l-probe' in kernel")
s = s.replace('47a31604c4d41743406201c85ae5ea7eb68999cf3bb0a41756c3a5bf224f9278',
              '365b880a19edbf7945bc82ca672cc853ea90fd059aaf9a480bd7e43039666305')
begin = s.index("    base_before = ")
end = s.index('    payload = ',begin)
s = s[:begin] + '''    base = (KERNEL / 'base.dtb').read_bytes()
    audit = json.loads((KERNEL / 'device-tree-audit.json').read_text())
    assert audit['passed'] and sha(base) == audit['base_sha256']
    (OUT / 'base.dtb').write_bytes(base)
''' + s[end:]
begin = s.index("              'scope': ")
end = s.index("              'remaining': ",begin)
s = s[:begin] + '''              'scope': 'V34 Linux 6.19.10 SDM660 baseline; normal MMC core, A6L boot/USB support, optional LED bypass; same RAM init and board-specific settings as V33',
              'base_sha256': sha(base), 'source_revision': 'a587e4f18b483d0a17579e6325c861b303988bda',
''' + s[end:]
p=T/'Package-RecoveryBaseline.py'
assert not p.exists()
p.write_bytes(s.encode())
py_compile.compile(str(p),doraise=True)
print('V34 baseline packaging script prepared.')
