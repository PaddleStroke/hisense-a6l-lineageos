#!/usr/bin/env python3
"""Offline checks of the RAM test's command and payload boundaries."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('collector', Path(__file__).with_name('Inspect-FastbootNative.py'))
collector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collector)


class RamGuards(unittest.TestCase):
    def test_command_allowlist_rejects_persistent_or_execution_operations(self):
        payloads = [{'path': '/fixed/one.bin'}]
        for command in [['flash', 'recovery', '/fixed/one.bin'], ['erase', 'avb_custom_key'],
                        ['flashing', 'unlock'], ['Hisense', 'unlock'], ['boot', '/fixed/one.bin'],
                        ['continue'], ['set_active', 'a'], ['stage', '/other.bin'],
                        ['getvar', 'product', 'flash', 'boot'], [], ['get_staged', '/tmp/readback']]:
            with self.subTest(command=command):
                self.assertFalse(collector.ram_command_allowed(command, payloads))

    def test_only_reviewed_queries_staging_and_reboot_are_allowed(self):
        payloads = [{'path': '/fixed/one.bin'}]
        for command in [['getvar', 'product', 'getvar', 'unlocked'], ['stage', '/fixed/one.bin'],
                        ['reboot'], ['oem', 'device-info'], ['flashing', 'get_unlock_ability']]:
            self.assertTrue(collector.ram_command_allowed(command, payloads))

    def test_tampered_same_size_file_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            expected = hashlib.sha256(b'original').hexdigest()
            (root / 'fixture.bin').write_bytes(b'altered!')
            with patch.object(collector, 'RAM_PAYLOADS', [('fixture.bin', 8, expected)]):
                with self.assertRaisesRegex(ValueError, 'hash differs'):
                    collector.verify_ram_payloads(root)

    def test_truncated_file_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            (root / 'fixture.bin').write_bytes(b'x')
            with patch.object(collector, 'RAM_PAYLOADS', [('fixture.bin', 8, 'unused')]):
                with self.assertRaisesRegex(ValueError, 'type or size'):
                    collector.verify_ram_payloads(root)


if __name__ == '__main__':
    unittest.main(verbosity=2)
