"""Resume this spare A6L's firmware-only backup using read commands only.

Run from the workspace root. Requires the prepared, locally patched edl client.
The initial user-area file is retained and renamed only after completion.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import serial.tools.list_ports

BASE = Path('firmware/raw-backup-20260914')
LOADER = Path('firmware/programmer/inspected/USB_Drivers_Hisense_A6L/qpst_qfil_programmer_data/prog_emmc_ufs_firehose_Sdm660_ddr_30060000.elf')
LOADER_HASH = '6003242582a610712c6b32c8f09475fb78a166e4bb3b018e9f01a7b9bb083642'
ADB = 'tools/platform-tools/adb.exe'
CHUNK = 64 * 1024 * 1024


def run(reboot):
    if hashlib.sha256(LOADER.read_bytes()).hexdigest() != LOADER_HASH:
        raise ValueError('Programmer hash mismatch')
    mapping = json.loads((BASE / 'partition-map.json').read_text())
    parts = {p['name']: p for p in mapping['partitions']}
    target = parts['userdata']['offset']
    image = BASE / 'emmc-user-area.bin'
    if not image.exists(): image = BASE / 'emmc-firmware-prefix.bin'
    current = image.stat().st_size
    if current % 512 or current > target:
        raise ValueError('Unexpected partial image length')
    if reboot:
        devices = subprocess.check_output([ADB, 'devices'], text=True).splitlines()[1:]
        if [line.split() for line in devices if line.strip()] != [['1e529013', 'device']]:
            raise ValueError('Expected only the authorized spare A6L')
        model = subprocess.check_output([ADB, '-s', '1e529013', 'shell', 'getprop', 'ro.product.model'], text=True).strip()
        if model != 'HLTE730T': raise ValueError('Unexpected model')
        subprocess.run([ADB, '-s', '1e529013', 'reboot', 'edl'], check=True, timeout=15)
    deadline = time.monotonic() + 30
    while True:
        ports = [p for p in serial.tools.list_ports.comports() if p.vid == 0x05c6 and p.pid == 0x9008]
        if len(ports) == 1: break
        if time.monotonic() > deadline: raise TimeoutError('No unique EDL device')
        time.sleep(.2)
    sys.path.insert(0, str(Path('tools/edl').resolve()))
    sys.argv = ['edl.py', '--loader=' + str(LOADER), '--memory=eMMC', '--serial', '--portname=' + ports[0].device]
    spec = importlib.util.spec_from_file_location('a6l_edl', 'tools/edl/edl.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    app = module.main(module.args)
    try:
        result = app.run()
        if result != 0 or app.fh is None: raise RuntimeError('Firehose connection failed')
        fh = app.fh.firehose
        if fh.cfg.SECTOR_SIZE_IN_BYTES != 512: raise ValueError('Unexpected sector size')
        independent = BASE / 'independent-read'
        independent.mkdir(exist_ok=True)

        def read(offset, size, output):
            if offset % 512 or size % 512: raise ValueError('Unaligned read')
            if not fh.cmd_read(0, offset // 512, size // 512, str(output), display=False):
                raise IOError('Firehose read did not acknowledge success')
            if output.stat().st_size != size: raise IOError('Read length mismatch')

        # Compare both boundaries before appending to an interrupted image.
        for offset in [0, current - 1024 * 1024]:
            if offset < 0: raise ValueError('Partial image too small')
            check = independent / ('boundary-' + str(offset) + '.bin')
            read(offset, 1024 * 1024, check)
            with image.open('rb') as source:
                source.seek(offset)
                if source.read(1024 * 1024) != check.read_bytes():
                    raise ValueError('Partial image boundary differs from phone')
        print('Saved image boundaries match fresh phone reads', flush=True)
        with image.open('ab') as destination, (BASE / 'resume-chunks.jsonl').open('a') as ledger:
            while current < target:
                size = min(CHUNK, target - current)
                chunk = BASE / 'pending-read.bin'
                read(current, size, chunk)
                data = chunk.read_bytes()
                destination.write(data)
                destination.flush()
                os.fsync(destination.fileno())
                ledger.write(json.dumps(dict(offset=current, bytes=size, sha256=hashlib.sha256(data).hexdigest())) + '\n')
                ledger.flush()
                current += size
                print(f'Firmware copied: {current}/{target} bytes', flush=True)
        final = BASE / 'emmc-firmware-prefix.bin'
        if image != final:
            if final.exists(): raise FileExistsError(final)
            image.rename(final)
        read(parts['grow']['offset'], mapping['expected_bytes'] - parts['grow']['offset'], BASE / 'emmc-gpt-tail.bin')
        for name in ['boot', 'recovery', 'vbmeta', 'dtbo', 'modemst1', 'modemst2', 'fsg', 'persist']:
            part = parts[name]
            read(part['offset'], part['bytes'], independent / (name + '.bin'))
            print('Independent read saved:', name, flush=True)
        (BASE / 'firmware-read-complete.json').write_text(json.dumps(dict(prefix_bytes=target, tail_offset=parts['grow']['offset'], userdata_excluded=True)))
        print('Firmware reads complete; run Verify-FirmwareBackup.py before reset', flush=True)
    finally:
        if app.cdc is not None and app.cdc.connected:
            app.cdc.close()


if __name__ == '__main__':
    if hasattr(sys.stdout, 'reconfigure'): sys.stdout.reconfigure(encoding='utf-8')
    run('--reboot-edl' in sys.argv[1:])
