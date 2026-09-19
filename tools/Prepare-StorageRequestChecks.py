"""Generate V26 checks from the verified V25 packaging workflow."""
from pathlib import Path
import hashlib
ROOT=Path(__file__).resolve().parents[1]
T=ROOT/'tools'
K=ROOT/'firmware/extracted/storage-request-kernel-v26-20260916'
image=bytearray((K/'Image').read_bytes())
assert b'A6L_STORAGE_EVENT' in image and b'command_write_returned' in image
assert image[8:16]==bytes(8)
image[8:16]=(0x80000).to_bytes(8,'little')
digest=hashlib.sha256(image).hexdigest()
assert hashlib.sha256((K/'sdhci-msm.ko').read_bytes()).hexdigest()=='0066e67c3d004c5b744629df87440005b50cdc9bc6cbb9e2296c95d0b07591cb'
for old in ['Package-StorageCmd0Ramdisk.py','Test-StorageCmd0ModuleLoad.py','Test-A6LStorageCmd0.py','Package-RecoveryStorageCmd0.py','Test-RecoveryStorageCmd0.py']:
    s=(T/old).read_text().replace('storage-cmd0','storage-request').replace('v25','v26')
    s=s.replace('386202084d486afaee0c9bf12ea412e0e8402c5026091498d19b66070330099b',digest)
    if old=='Package-RecoveryStorageCmd0.py':
        s=s.replace('recovery-probe-storage-scan-v24-20260916','recovery-probe-storage-cmd0-v25-20260916')
        s=s.replace('6cc9c9940c068057ed58f3a210213b444ef2321eee01e1aa96b082e275a28731','911de6c83d41a2be646a70e2b850f7d2aa795e937262c1b44c74ee2259e575e7')
        s=s.replace('V24 DT/command line/init/module/power settings preserved; add16 CMD0 process-context checkpoints with unchanged96-pause bound','V25 hardware settings and RAM init preserved; add five process-context pauses and bounded non-sleeping command/IRQ events')
    p=T/old.replace('StorageCmd0','StorageRequest')
    assert not p.exists(),p
    p.write_bytes(s.encode())
print('V26 adapted kernel',digest)
