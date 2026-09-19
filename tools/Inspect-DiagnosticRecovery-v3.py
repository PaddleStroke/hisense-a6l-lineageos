#!/usr/bin/env python3
"""Check diagnostic/restore imports, both payloads and XML guards without opening USB."""
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
spec = importlib.util.spec_from_file_location('writer', root / 'Write-LaptopDiagnosticRecovery-v3.py')
writer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(writer)
import DiagnosticRecoveryProtocolV2 as protocol
digests = {}
for mode, name in [('restore-stock', 'stock-recovery/restore-stock-recovery.img'), ('install-diagnostic', 'ram-staging/recovery-diagnostic-v2-unsigned.img')]:
    payload = (root / name).read_bytes()
    digests[mode] = protocol.verify_payload(payload, mode)
assert writer.REGIONS['vbmeta'] == (291811328, 65536)
for mode, filename in [('install-diagnostic', 'ram-staging/recovery-diagnostic-unsigned.img'), ('install-diagnostic', 'stock-recovery/restore-stock-recovery.img'), ('restore-stock', 'ram-staging/recovery-diagnostic-v2-unsigned.img')]:
    try:
        protocol.verify_payload((root / filename).read_bytes(), mode)
    except ValueError:
        pass
    else:
        raise AssertionError('Old or cross-mode image was accepted')
assert writer.allowed_xml(protocol.PROGRAM_XML) == 'program'
for xml in ['<data><erase/></data>', '<data><patch/></data>',
            '<data><power value="reset_to_edl"/></data>',
            '<data><power value="off" extra="1"/></data>',
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
                  'payload_bytes': len(payload), 'payload_sha256': digests,
                  'worker_sha256': hashlib.sha256((root / 'Write-LaptopDiagnosticRecovery-v3.py').read_bytes()).hexdigest(),
                  'protocol_sha256': hashlib.sha256((root / 'DiagnosticRecoveryProtocolV2.py').read_bytes()).hexdigest(),
                  'passed': True}, indent=2))
