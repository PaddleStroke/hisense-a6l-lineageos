#!/usr/bin/env python3
"""Verify the recovery candidate and prepare a one-partition restore description."""
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parent.parent
output = root / 'firmware/extracted/recovery-probe-20260914'
report = json.loads((output / 'report.json').read_text())
assert report['all_checks_passed'] and not report['ready_to_flash']
manifest = json.loads((root / 'firmware/raw-backup-20260914/firmware-verification.json').read_text())
recovery = next(p for p in manifest['partitions'] if p['name'] == 'recovery')
assert recovery['offset'] == 469762048 and recovery['bytes'] == 67108864
assert recovery == report['stock_recovery']
checked = []
for name in ['recovery-diagnostic-unsigned.img', 'restore-stock-recovery.img']:
    entry = next(a for a in report['artifacts'] if a['file'] == name)
    path = output / name
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    assert path.stat().st_size == entry['bytes'] == recovery['bytes'] and digest == entry['sha256']
    checked.append({'file': name, 'bytes': entry['bytes'], 'sha256': digest})
assert checked[1]['sha256'] == recovery['sha256']
sector_size = 512
assert not recovery['offset'] % sector_size and not recovery['bytes'] % sector_size
restore = ET.Element('data')
ET.SubElement(restore, 'program', {
    'SECTOR_SIZE_IN_BYTES': str(sector_size), 'file_sector_offset': '0',
    'filename': 'restore-stock-recovery.img', 'label': 'recovery',
    'num_partition_sectors': str(recovery['bytes'] // sector_size),
    'physical_partition_number': '0', 'start_sector': str(recovery['offset'] // sector_size),
    'sparse': 'false'})
ET.indent(restore)
ET.ElementTree(restore).write(output / 'restore-recovery-only.xml', encoding='utf-8', xml_declaration=True)
prepared = {'scope': 'Offline description only; no restore attempted',
            'files_checked': checked, 'sector_size': sector_size,
            'start_sector': recovery['offset'] // sector_size,
            'sectors': recovery['bytes'] // sector_size,
            'restore_tested': False,
            'requirements': ['Confirm the current device and GPT match this spare backup before any EDL write',
                             'Use only the previously verified A6L eMMC programmer',
                             'After a restore, independently read back and compare the complete recovery partition',
                             'Reboot and verify stock Android and stock recovery return',
                             'No GPT, bootloader, radio, calibration or other partition belongs in this restore']}
(output / 'restore-preflight.json').write_text(json.dumps(prepared, indent=2) + '\n')
print(json.dumps(prepared, indent=2))
