"""Generate V24 offline checks using the verified V23 packaging workflow."""
from pathlib import Path
import hashlib
ROOT=Path(__file__).resolve().parents[1]
T=ROOT/'tools'
K=ROOT/'firmware/extracted/storage-scan-kernel-v24-20260916'
image=bytearray((K/'Image').read_bytes())
assert b'scan_entry' in image and b'scan_reset_card' in image
assert image[8:16]==bytes(8)
image[8:16]=(0x80000).to_bytes(8,'little')
digest=hashlib.sha256(image).hexdigest()
assert (K/'sdhci-msm.ko').read_bytes()==(ROOT/'firmware/extracted/storage-host-kernel-v23-20260916/sdhci-msm.ko').read_bytes()
for old in ['Package-StorageHostRamdisk.py','Test-StorageHostModuleLoad.py','Test-A6LStorageHost.py','Package-RecoveryStorageHost.py','Test-RecoveryStorageHost.py']:
    s=(T/old).read_text().replace('storage-host','storage-scan').replace('v23','v24')
    s=s.replace('230bab281c9ece9bdd724b801355fec53ba5653fef59e57a04bdafdf0d6f74cc',digest)
    if old=='Package-RecoveryStorageHost.py':
        s=s.replace('recovery-probe-storage-noled-v22-20260916','recovery-probe-storage-host-v23-20260916')
        s=s.replace('19ea35f9ce8b97683c195aa2a26fce811170a04064483c539eddaba1782720d3','fcc665755df0dfb3324f606701f8072dcdda3cef98227e416c593ba9ff113afa')
        s=s.replace('V22 DT/command line/init/no-LED module/power settings preserved; add bounded MMC host and initial power-up checkpoints','V23 DT/command line/init/module/power settings preserved; add16 card-scan checkpoints and extend shared trace bound to96')
    p=T/old.replace('StorageHost','StorageScan')
    assert not p.exists(),p
    p.write_bytes(s.encode())
print('V24 adapted kernel',digest)
