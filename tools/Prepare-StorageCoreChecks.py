"""Derive V21 offline validation; reuse exact tested V20 RAM init source."""
from pathlib import Path
import hashlib
ROOT = Path(__file__).resolve().parents[1]
T = ROOT / 'tools'
K = ROOT / 'firmware/extracted/storage-core-kernel-v21-20260916'
image = bytearray((K / 'Image').read_bytes())
assert b'core_caps_reset_all' in image and b'core_register_mmc_host' in image
assert image[8:16] == bytes(8)
image[8:16] = (0x80000).to_bytes(8, 'little')
h = hashlib.sha256(image).hexdigest()

def base(name):
    s = (T / name).read_text()
    for a,b in [
        ('storage-trace-kernel-v20-r2-20260916', 'storage-core-kernel-v21-20260916'),
        ('storage-trace-ramdisk-v20-20260916', 'storage-core-ramdisk-v21-20260916'),
        ('storage-trace-probe-v20-20260916', 'storage-core-probe-v21-20260916'),
        ('storage-trace-smoke-v20-20260916', 'storage-core-smoke-v21-20260916'),
        ('recovery-probe-storage-trace-v20-20260916', 'recovery-probe-storage-core-v21-20260916'),
        ('7449f5391f55e6e21df495ab4ea0cf4b8c1ec75e96b468827e66aa9458078258', h),
    ]:
        s=s.replace(a,b)
    return s

def write(name,s):
    p=T/name
    assert not p.exists(),p
    p.write_bytes(s.encode())

write('Package-StorageCoreRamdisk.py', base('Package-StorageTraceRamdisk.py'))
write('Test-StorageCoreModuleLoad.py', base('Test-StorageTraceModuleLoad.py'))
write('Test-A6LStorageCore.py', base('Test-A6LStorageTrace.py'))
s=base('Package-RecoveryStorageTrace.py')
s=s.replace("OLD = ROOT / 'firmware/extracted/recovery-probe-storage-module-v19-20260916'", "OLD = ROOT / 'firmware/extracted/recovery-probe-storage-trace-v20-20260916'")
s=s.replace('9c092eb41e8c894bff5aea6a1df442582cb4378d782e48d3883aafa1d04bed4e','c1ba3da846fa31cc9a40a8ba6c2aba434d8711d0f67b795469bf65c6180d4392')
s=s.replace("assert 'a6l_storage_trace' not in old_cmdline\n    cmdline = old_cmdline + ' a6l_storage_trace=1'", "assert 'a6l_storage_trace=1' in old_cmdline\n    cmdline = old_cmdline")
s=s.replace('(8, 12), (16, 20), (64, 576), (576, 608), (608, 1632), (1636, 1644)', '(8, 12), (16, 20), (576, 608), (1636, 1644)')
s=s.replace('V19 DT and power settings preserved; opt-in bounded 250ms storage probe checkpoints, advance fork marker and 1s pre-load pause; no persistent mounts', 'V20 DT, command line, RAM init and power settings preserved; finer sleepable SDHCI setup checkpoints share existing64-pause limit; no persistent mounts')
write('Package-RecoveryStorageCore.py',s)
write('Test-RecoveryStorageCore.py',base('Test-RecoveryStorageTrace.py'))
print('V21 adapted kernel',h)
