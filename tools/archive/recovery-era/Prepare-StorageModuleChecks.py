"""Create the offline V19 RAM/kernel and captured-ABL validation tools."""
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
KERNEL = ROOT / 'firmware/extracted/storage-module-kernel-v19-20260916'
image = bytearray((KERNEL / 'Image').read_bytes())
assert image[8:16] == bytes(8)
image[8:16] = (0x80000).to_bytes(8, 'little')
kernel_hash = hashlib.sha256(image).hexdigest()

def write(name, source):
    p = TOOLS / name
    assert not p.exists(), p
    p.write_bytes(source.encode())

source = (TOOLS / 'Test-A6LUsbEventTrace.py').read_text()
source = source.replace('staged-usb-probe-v16-20260916', 'storage-module-probe-v19-20260916')
source = source.replace('staged-usb-ramdisk-v16-20260916', 'storage-module-ramdisk-v19-20260916')
source = source.replace('A6L_STAGED_USB_V16 config_after=0 bind_after=4 connect_after=8 serial_after=12 heartbeat=2',
                        'A6L_STAGED_STORAGE_V19 config_after=0 bind_after=4 connect_after=8 serial_after=12 module_after=20 heartbeat=2')
source = source.replace("no_panic=b'Kernel panic' not in data,", "no_panic=b'Kernel panic' not in data,\n                      no_storage_before_usb=b'A6L_STORAGE_MODULE_LOAD_BEGIN' not in data and b'A6L_STORAGE_MODULE_FORK_BEGIN' not in data,")
write('Test-A6LStorageModule.py', source)

source = (TOOLS / 'Package-RecoveryUsbEventTrace.py').read_text()
source = source.replace('Package the tested V15 kernel/RAM pair; preserve all device-tree wiring.', 'Package V19 with delayed storage module and exact V18 device tree.')
source = source.replace('recovery-probe-staged-usb-v15-20260916', 'recovery-probe-storage-v18-20260916')
source = source.replace('staged-usb-ramdisk-v16-20260916', 'storage-module-ramdisk-v19-20260916')
source = source.replace('staged-usb-probe-v16-20260916', 'storage-module-probe-v19-20260916')
source = source.replace('recovery-probe-staged-usb-v16-20260916', 'recovery-probe-storage-module-v19-20260916')
source = source.replace('7bb565ac584a8ec05fa95145e22912a018c240bef7ffd7665dc62113f6587b70', kernel_hash)
source = source.replace('7dbab370e9580ec3ac25bd24a9edadfedef5334447a754d83b789a72d0b1271d', 'a983ef25bdca4a06dae88ab74a4db8582b0a4507f01e14ad6bb2a9354c123ee6')
source = source.replace("assert 'a6l_manual_usb=1' in old_cmdline and 'a6l_usb_trace' not in old_cmdline\n    cmdline = old_cmdline + ' a6l_usb_trace=1'", "assert 'a6l_manual_usb=1' in old_cmdline and 'a6l_usb_trace=1' in old_cmdline\n    cmdline = old_cmdline")
source = source.replace('(8, 12), (16, 20), (64, 576), (576, 608), (608, 1632), (1636, 1644)', '(8, 12), (16, 20), (576, 608), (1636, 1644)')
source = source.replace('A6L opt-in bounded USB event trace, stages0/4/8/12 seconds; identical V15 DT, voltage constraints and load addresses',
                        'V18 device tree and USB preserved; only SDHCI_MSM changes to module, RAM init loads it at 20s after USB transmission; no persistent mounts')
source = source.replace("assert test['passed']", "assert test['passed']\n    assert json.loads((ROOT / 'firmware/extracted/storage-module-smoke-v19-r2-20260916/report.json').read_text())['passed']")
write('Package-RecoveryStorageModule.py', source)
source = (TOOLS / 'Test-RecoveryStorage.py').read_text().replace('recovery-probe-storage-v18-20260916', 'recovery-probe-storage-module-v19-20260916').replace('7bb565ac584a8ec05fa95145e22912a018c240bef7ffd7665dc62113f6587b70', kernel_hash)
write('Test-RecoveryStorageModule.py', source)
print('V19 adapted kernel SHA256', kernel_hash)
