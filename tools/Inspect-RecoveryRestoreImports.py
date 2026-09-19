#!/usr/bin/env python3
"""Check restore worker imports, payload and XML geometry without opening USB."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parent
sys.path[:0] = [str(root / 'a6l-recovery-kit/deps'), str(root / 'a6l-recovery-kit/edl')]
import usb.core


def no_usb(*args, **kwargs):
    raise RuntimeError('Offline restore preflight must not access USB')


usb.core.find = no_usb
spec = importlib.util.spec_from_file_location('writer', root / 'Write-LaptopStockRecovery-v1.py')
writer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(writer)
import RecoveryWriteProtocol as protocol
payload = (root / 'stock-recovery/restore-stock-recovery.img').read_bytes()
digest = protocol.verify_stock_payload(payload)
assert writer.allowed_xml(protocol.PROGRAM_XML) == 'program'
for xml in ['<data><erase/></data>', '<data><patch/></data>',
            protocol.PROGRAM_XML.replace('917504', '917505'),
            protocol.PROGRAM_XML.replace('131072', '131073'),
            protocol.PROGRAM_XML.replace('physical_partition_number="0"', 'physical_partition_number="1"')]:
    try:
        writer.allowed_xml(xml)
    except ValueError:
        continue
    raise AssertionError('Unsafe XML accepted: ' + xml)
sys.argv = writer.edl_arguments(root / 'a6l-recovery-kit')
spec = importlib.util.spec_from_file_location('edl', root / 'a6l-recovery-kit/edl/edl.py')
edl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(edl)
assert writer.create_edl_app(edl).imported is True
print(json.dumps({'scope': 'Offline restore imports/geometry/payload check; no USB opened',
                  'payload_bytes': len(payload), 'payload_sha256': digest,
                  'worker_sha256': hashlib.sha256((root / 'Write-LaptopStockRecovery-v1.py').read_bytes()).hexdigest(),
                  'protocol_sha256': hashlib.sha256((root / 'RecoveryWriteProtocol.py').read_bytes()).hexdigest(),
                  'passed': True}, indent=2))
