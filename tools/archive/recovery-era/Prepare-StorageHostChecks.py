"""Generate V23 packaging/check tools from V22, preserving the no-LED module."""
from pathlib import Path
import hashlib
ROOT=Path(__file__).resolve().parents[1]
T=ROOT/'tools'
K=ROOT/'firmware/extracted/storage-host-kernel-v23-20260916'
image=bytearray((K/'Image').read_bytes())
assert b'power_initial_ios' in image and b'mmc_device_add' in image
assert image[8:16]==bytes(8)
image[8:16]=(0x80000).to_bytes(8,'little')
digest=hashlib.sha256(image).hexdigest()
assert b'A6L_STORAGE_V22 optional activity LED disabled' in (K/'sdhci-msm.ko').read_bytes()
for old,new in [
    ('Package-StorageNoLedRamdisk.py','Package-StorageHostRamdisk.py'),
    ('Test-StorageNoLedModuleLoad.py','Test-StorageHostModuleLoad.py'),
    ('Test-A6LStorageNoLed.py','Test-A6LStorageHost.py'),
    ('Package-RecoveryStorageNoLed.py','Package-RecoveryStorageHost.py'),
    ('Test-RecoveryStorageNoLed.py','Test-RecoveryStorageHost.py'),
]:
    s=(T/old).read_text()
    for a,b in [
        ('storage-noled-kernel-v22','storage-host-kernel-v23'),
        ('storage-noled-ramdisk-v22','storage-host-ramdisk-v23'),
        ('storage-noled-probe-v22','storage-host-probe-v23'),
        ('storage-noled-smoke-v22','storage-host-smoke-v23'),
        ('recovery-probe-storage-noled-v22','recovery-probe-storage-host-v23'),
        ('5717b756307399dae8767922963ef592d6661edf2ddb8a457487f3fe06f37643',digest),
    ]:
        s=s.replace(a,b)
    if old=='Package-RecoveryStorageNoLed.py':
        s=s.replace("OLD = ROOT / 'firmware/extracted/recovery-probe-storage-core-v21-20260916'", "OLD = ROOT / 'firmware/extracted/recovery-probe-storage-noled-v22-20260916'")
        s=s.replace('d4fa4f8b5b50e3c7b9e123ad5cde913d5e5000fee3535620af4e50773b13d407','19ea35f9ce8b97683c195aa2a26fce811170a04064483c539eddaba1782720d3')
        s=s.replace('V21 kernel/DT/command line/init preserved; only matching storage module adds A6L-only NO_LED quirk; regulator and storage settings unchanged','V22 DT/command line/init/no-LED module/power settings preserved; add bounded MMC host and initial power-up checkpoints')
    p=T/new
    assert not p.exists(),p
    p.write_bytes(s.encode())
print('V23 adapted kernel',digest)
