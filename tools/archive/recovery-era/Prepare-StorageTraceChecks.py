"""Generate V20 offline checks from the archived V19 checks."""
from pathlib import Path
import hashlib

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
KERNEL = ROOT / 'firmware/extracted/storage-trace-kernel-v20-r2-20260916'
image = bytearray((KERNEL / 'Image').read_bytes())
assert b'A6L_STORAGE_CHECKPOINT' in image
assert image[8:16] == bytes(8)
image[8:16] = (0x80000).to_bytes(8, 'little')
kernel_hash = hashlib.sha256(image).hexdigest()

def write(name, source):
    p = TOOLS / name
    assert not p.exists(), p
    p.write_bytes(source.encode())

def base(name):
    s = (TOOLS / name).read_text()
    for old, new in [
        ('storage-module-kernel-v19-20260916', 'storage-trace-kernel-v20-r2-20260916'),
        ('storage-module-ramdisk-v19-20260916', 'storage-trace-ramdisk-v20-20260916'),
        ('storage-module-probe-v19-20260916', 'storage-trace-probe-v20-20260916'),
        ('storage-module-smoke-v19-r2-20260916', 'storage-trace-smoke-v20-20260916'),
        ('storage-module-smoke-v19-20260916', 'storage-trace-smoke-v20-20260916'),
        ('recovery-probe-storage-module-v19-20260916', 'recovery-probe-storage-trace-v20-20260916'),
        ('A6L_STAGED_STORAGE_V19', 'A6L_STAGED_STORAGE_V20'),
        ('4a93a3ef95ec44c820fdacf24085c68ce64718f27cdc7140cf676c14a744673c', kernel_hash),
    ]:
        s = s.replace(old, new)
    return s

s = base('Package-StorageModuleRamdisk.py').replace('Package V19', 'Package V20')
s = s.replace("assert b'A6L_STORAGE_MODULE_LOAD_BEGIN' in data", "assert b'A6L_STORAGE_MODULE_FORK_ARMED' in data\nassert b'A6L_STORAGE_MODULE_LOAD_BEGIN pause_ms=1000' in data")
write('Package-StorageTraceRamdisk.py', s)
s = base('Test-StorageModuleLoad.py')
write('Test-StorageTraceModuleLoad.py', s)
s = base('Test-A6LStorageModule.py').replace('a6l_usb_trace=1\'', 'a6l_usb_trace=1 a6l_storage_trace=1\'')
write('Test-A6LStorageTrace.py', s)
s = base('Package-RecoveryStorageModule.py')
s = s.replace("OLD = ROOT / 'firmware/extracted/recovery-probe-storage-v18-20260916'", "OLD = ROOT / 'firmware/extracted/recovery-probe-storage-module-v19-20260916'")
s = s.replace('a983ef25bdca4a06dae88ab74a4db8582b0a4507f01e14ad6bb2a9354c123ee6', '9c092eb41e8c894bff5aea6a1df442582cb4378d782e48d3883aafa1d04bed4e')
s = s.replace('cmdline = old_cmdline\n', "assert 'a6l_storage_trace' not in old_cmdline\n    cmdline = old_cmdline + ' a6l_storage_trace=1'\n")
s = s.replace('(8, 12), (16, 20), (576, 608), (1636, 1644)', '(8, 12), (16, 20), (64, 576), (576, 608), (608, 1632), (1636, 1644)')
s = s.replace('V18 device tree and USB preserved; only SDHCI_MSM changes to module, RAM init loads it at 20s after USB transmission; no persistent mounts',
    'V19 DT and power settings preserved; opt-in bounded 250ms storage probe checkpoints, advance fork marker and 1s pre-load pause; no persistent mounts')
write('Package-RecoveryStorageTrace.py', s)
write('Test-RecoveryStorageTrace.py', base('Test-RecoveryStorageModule.py'))
print('V20 adapted kernel SHA256', kernel_hash)
