"""Generate V25 checks from the verified V24 packaging workflow."""
from pathlib import Path
import hashlib
ROOT=Path(__file__).resolve().parents[1]
T=ROOT/'tools'
K=ROOT/'firmware/extracted/storage-cmd0-kernel-v25-20260916'
image=bytearray((K/'Image').read_bytes())
assert b'cmd0_chipselect_high' in image and b'cmd0_dispatch_request' in image
assert image[8:16]==bytes(8)
image[8:16]=(0x80000).to_bytes(8,'little')
digest=hashlib.sha256(image).hexdigest()
assert (K/'sdhci-msm.ko').read_bytes()==(ROOT/'firmware/extracted/storage-scan-kernel-v24-20260916/sdhci-msm.ko').read_bytes()
for old in ['Package-StorageScanRamdisk.py','Test-StorageScanModuleLoad.py','Test-A6LStorageScan.py','Package-RecoveryStorageScan.py','Test-RecoveryStorageScan.py']:
    s=(T/old).read_text().replace('storage-scan','storage-cmd0').replace('v24','v25')
    s=s.replace('7335a99fd4fcffdce926654ea10a1599eb86b28f036505acc2e23637e6447ff5',digest)
    if old=='Package-RecoveryStorageScan.py':
        s=s.replace('recovery-probe-storage-host-v23-20260916','recovery-probe-storage-scan-v24-20260916')
        s=s.replace('fcc665755df0dfb3324f606701f8072dcdda3cef98227e416c593ba9ff113afa','6cc9c9940c068057ed58f3a210213b444ef2321eee01e1aa96b082e275a28731')
        s=s.replace('V23 DT/command line/init/module/power settings preserved; add16 card-scan checkpoints and extend shared trace bound to96','V24 DT/command line/init/module/power settings preserved; add16 CMD0 process-context checkpoints with unchanged96-pause bound')
    p=T/old.replace('StorageScan','StorageCmd0')
    assert not p.exists(),p
    p.write_bytes(s.encode())
print('V25 adapted kernel',digest)
