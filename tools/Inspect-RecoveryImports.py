#!/usr/bin/env python3
"""Parse the exact EDL arguments and load the actual libraries without USB access."""
import importlib.util
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parent
kit = root / 'a6l-recovery-kit'
spec = importlib.util.spec_from_file_location('reader', root / 'Read-LaptopRecovery-v4.py')
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)
sys.path[:0] = [str(kit / 'deps'), str(kit / 'edl')]
import usb.core


def no_usb(*args, **kwargs):
    raise RuntimeError('Import preflight must not enumerate or access USB')


usb.core.find = no_usb
sys.argv = reader.edl_arguments(kit)
spec = importlib.util.spec_from_file_location('edl_preflight', kit / 'edl/edl.py')
edl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(edl)
assert edl.args['getstorageinfo'] and edl.args['--memory'] == 'eMMC'
assert edl.args['--vid'] == '05c6' and edl.args['--pid'] == '9008'
assert edl.args['--loader'] == str(kit / 'programmer.elf')
assert not edl.args['--serial']
from types import SimpleNamespace
from edlclient.Library.firehose import firehose, response
config = firehose.cfg()
config.MemoryName = 'eMMC'
config.TargetName = 'MSM8996'
calls = []


def xml_request(data):
    calls.append(reader.allowed_xml(data))
    return response(resp=True)


def read_probe(lun, sector, count, display):
    data = f'<data><read SECTOR_SIZE_IN_BYTES="512" physical_partition_number="{lun}" start_sector="{sector}" num_partition_sectors="{count}"/></data>'
    calls.append(reader.allowed_xml(data))
    assert (lun, sector, count) == (0, 1, 1)
    return response(resp=True)


fake = SimpleNamespace(cfg=config, xmlsend=xml_request, cdc=SimpleNamespace(read=lambda **kwargs: b''),
                       args=edl.args, info=lambda *args: None, supported_functions=[],
                       cmd_read_buffer=read_probe, parse_storage=lambda: calls.append('storage'),
                       getluns=lambda args: [0])
assert firehose.configure(fake, 0) is True
assert calls == ['configure', 'read', 'storage']

# Exercise the actual main.run connection handoff with inert stand-ins for USB
# and the already hardware-tested Sahara path. No nominal command may dispatch.
connection = SimpleNamespace(connected=True, timeout=0, close=lambda: (_ for _ in ()).throw(
    AssertionError('Imported connection was closed prematurely')))
edl.usb_class = lambda **kwargs: connection
edl.sahara = lambda *args, **kwargs: SimpleNamespace(programmer=None)


class InertFirehose:
    connected = False

    def connect(self, sahara):
        self.connected = True
        return True

    def handle_firehose(self, *args):
        raise AssertionError('Imported mode dispatched a CLI command')


edl.firehose_client = lambda *args: InertFirehose()
app = reader.create_edl_app(edl)
assert app.imported is True and app.parse_cmd(edl.args) == ''
app.doconnect = lambda loop: {'mode': 'firehose'}
assert app.run() == 0 and app.fh.connected and connection.connected
print(json.dumps({'actual_client_argument_parsing_passed': True,
                  'actual_client_configuration_path_passed': True,
                  'actual_imported_connection_handoff_passed': True, 'usb_access_disabled': True}))
