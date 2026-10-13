"""Exercise the EDL read-only boundary without USB or a phone."""
import importlib.util
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch

with patch.dict(sys.modules, {'pwd': sys.modules.get('pwd', types.ModuleType('pwd'))}):
    spec = importlib.util.spec_from_file_location('reader', Path(__file__).with_name('Read-LaptopRecovery.py'))
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)


def read_xml(offset, size, lun=0):
    return f'<data><read SECTOR_SIZE_IN_BYTES="512" physical_partition_number="{lun}" start_sector="{offset // 512}" num_partition_sectors="{size // 512}"/></data>'


class ReadGuards(unittest.TestCase):
    def test_persistent_operations_rejected(self):
        for tag in ['program', 'erase', 'patch', 'setbootablestoragedrive', 'ufs', 'benchmark']:
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                reader.allowed_xml(f'<data><{tag}/></data>')

    def test_only_fixed_regions_allowed(self):
        for offset, size in reader.REGIONS.values():
            self.assertEqual(reader.allowed_xml(read_xml(offset, size)), 'read')
            for wrong in [read_xml(offset + 512, size), read_xml(offset, size + 512), read_xml(offset, size, 1)]:
                with self.assertRaises(ValueError):
                    reader.allowed_xml(wrong)

    def test_multiple_operations_rejected(self):
        with self.assertRaises(ValueError):
            reader.allowed_xml('<data><nop/><program/></data>')

    def test_client_configuration_probe_is_exactly_one_header_sector(self):
        self.assertEqual(reader.allowed_xml(read_xml(512, 512)), 'read')
        for wrong in [read_xml(0, 512), read_xml(512, 1024), read_xml(512, 512, 1)]:
            with self.assertRaises(ValueError):
                reader.allowed_xml(wrong)

    def test_reset_and_emmc_only(self):
        self.assertEqual(reader.allowed_xml('<data><power value="reset"/></data>'), 'power')
        self.assertEqual(reader.allowed_xml('<data><configure MemoryName="eMMC"/></data>'), 'configure')
        for xml in ['<data><power value="edl"/></data>', '<data><configure MemoryName="ufs"/></data>']:
            with self.assertRaises(ValueError):
                reader.allowed_xml(xml)


if __name__ == '__main__':
    unittest.main(verbosity=2)
