#!/usr/bin/env python3
"""Offline tests for command restrictions and fastboot response parsing."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('packets', Path(__file__).with_name('Inspect-FastbootPackets.py'))
packets = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packets)


class ProtocolTests(unittest.TestCase):
    def test_exact_wire_bytes(self):
        self.assertEqual(packets.encode_command('getvar:product'), b'getvar:product')
        self.assertEqual(packets.encode_command('reboot'), b'reboot')

    def test_non_query_commands_and_injection_rejected(self):
        for command in ['flash:recovery', 'download:00001000', 'erase:userdata',
                        'Hisense unlock', 'flashing unlock', 'continue',
                        'getvar:product\0erase:userdata', 'getvar:product\n', 'getvar:all']:
            with self.subTest(command=command), self.assertRaises(ValueError):
                packets.encode_command(command)

    def test_status_and_padding(self):
        self.assertEqual(packets.parse_packet(b'OKAYsdm660'), ('OKAY', 'sdm660'))
        self.assertEqual(packets.parse_packet(b'FAILunknown command\0'), ('FAIL', 'unknown command'))
        self.assertEqual(packets.parse_packet(b'INFOstate'), ('INFO', 'state'))
        self.assertEqual(packets.parse_packet(b'OKAY'), ('OKAY', ''))

    def test_download_and_bad_framing_rejected(self):
        for packet in [b'', b'OKA', b'DATA00001000', b'BOOT', b'OKAY' + b'a' * 61]:
            with self.subTest(packet=packet), self.assertRaises(ValueError):
                packets.parse_packet(packet)


if __name__ == '__main__':
    unittest.main()
