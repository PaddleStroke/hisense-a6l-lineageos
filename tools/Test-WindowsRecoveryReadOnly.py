#!/usr/bin/env python3
"""Offline guard and actual serial-import checks, with hardware access blocked."""
import importlib.util
import hashlib
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('reader', ROOT / 'tools/WindowsRecoveryReadOnly.py')
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)

def read_xml(sector='917504', count='131072', lun='0', size='512'):
    return f'<data><read SECTOR_SIZE_IN_BYTES="{size}" physical_partition_number="{lun}" start_sector="{sector}" num_partition_sectors="{count}"/></data>'

class GuardTests(unittest.TestCase):
    def test_persistent_and_compound_commands_rejected(self):
        for xml in ['<data><program/></data>', '<data><erase/></data>', '<data><patch/></data>', '<data><nop/><program/></data>', '<data><nop><program/></nop></data>']:
            with self.subTest(xml=xml), self.assertRaises(ValueError):
                reader.ReadGate().begin(xml)

    def test_fixed_geometry_only(self):
        for xml in [read_xml(sector='917505'), read_xml(count='131073'), read_xml(lun='1'), read_xml(size='4096')]:
            with self.subTest(xml=xml), self.assertRaises(ValueError):
                reader.ReadGate().begin(xml)
        self.assertEqual(reader.ReadGate().begin(read_xml()), 'read')
        self.assertEqual(reader.ReadGate().begin(read_xml(sector='1', count='1')), 'read')

    def test_unfinished_read_and_ambiguity_block_reset(self):
        gate = reader.ReadGate()
        gate.verified = set(reader.REGIONS)
        gate.begin(read_xml())
        with self.assertRaises(ValueError):
            gate.begin('<data><power value="reset"/></data>')
        gate.response({'value': 'ACK', 'rawmode': 'false'})
        gate.ambiguous = True
        with self.assertRaises(ValueError):
            gate.begin('<data><power value="reset"/></data>')

    def test_reset_requires_all_reads_and_cannot_repeat(self):
        gate = reader.ReadGate()
        with self.assertRaises(ValueError):
            gate.begin('<data><power value="reset"/></data>')
        gate.verified = set(reader.REGIONS)
        with self.assertRaises(ValueError):
            gate.begin('<data><power value="reset" extra="1"/></data>')
        self.assertEqual(gate.begin('<data><power value="reset"/></data>'), 'power')
        with self.assertRaises(ValueError):
            gate.begin('<data><power value="reset"/></data>')

    def test_wrong_identity_and_incomplete_region_rejected(self):
        wrong = dict(reader.IDENTITY, serial='00000000')
        with self.assertRaises(ValueError):
            reader.check_identity(wrong)
        reader.check_identity(dict(reader.IDENTITY))
        gate = reader.ReadGate()
        with self.assertRaises(ValueError):
            gate.verify_region('misc-bcb', b'\0')
        with self.assertRaises(ValueError):
            gate.verify_region('misc-bcb', b'x' * 4096)
        gate.verify_region('misc-bcb', bytes(4096))
        self.assertEqual(gate.verified, {'misc-bcb'})

def main():
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(GuardTests))
    assert result.wasSuccessful()
    count = reader.verify_inputs()
    import serial
    import usb.core
    def no_hardware(*args, **kwargs):
        raise AssertionError('Offline preflight attempted hardware access')
    serial.Serial.open = no_hardware
    usb.core.find = no_hardware
    module = reader.edl_module('COM3')
    assert module.args['--serial'] and module.args['--portname'] == 'COM3'
    assert module.args['getstorageinfo'] and module.args['--memory'] == 'eMMC'
    app = module.main(module.args)
    assert app.imported is True and app.parse_cmd(module.args) == ''
    class ReachedConnection(Exception):
        pass
    def stop_before_connection(*args, **kwargs):
        raise ReachedConnection()
    app.doconnect = stop_before_connection
    app.console_cmd = lambda *args: ''
    try:
        app.run()
    except ReachedConnection:
        pass
    else:
        raise AssertionError('Did not reach expected serial connection boundary')
    assert app.cdc.is_serial is True
    report = {'passed': True, 'manifest_sha256': reader.MANIFEST_HASH, 'pinned_files': count,
              'tested_tools': {name: hashlib.sha256((ROOT / 'tools' / name).read_bytes()).hexdigest()
                               for name in ('WindowsRecoveryReadOnly.py', 'Test-WindowsRecoveryReadOnly.py')},
              'guard_tests': result.testsRun, 'serial_imported_mode_passed': True, 'hardware_access_disabled': True,
              'limits': 'No physical read, serial flash transfer or physical timeout validation yet.'}
    reader.save(ROOT / 'firmware/extracted/windows-edl-readonly-preflight-v1.json', report)
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
