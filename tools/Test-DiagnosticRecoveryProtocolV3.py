"""Fault-injection checks for the fixed recovery write boundary; no USB access."""
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import DiagnosticRecoveryProtocolV3 as protocol


class Endpoint:
    def __init__(self, fail_at=None):
        self.calls = []
        self.fail_at = fail_at

    def write(self, data, timeout):
        self.calls.append(bytes(data))
        return len(data) - 1 if len(self.calls) == self.fail_at else len(data)


class WriterChecks(unittest.TestCase):
    def test_geometry_and_extra_operations_rejected(self):
        self.assertEqual(protocol.check_program_xml(protocol.PROGRAM_XML), 'program')
        for wrong in [protocol.PROGRAM_XML.replace('917504', '917505'),
                      protocol.PROGRAM_XML.replace('131072', '131073'),
                      protocol.PROGRAM_XML.replace('physical_partition_number="0"', 'physical_partition_number="1"'),
                      protocol.PROGRAM_XML.replace('/></data>', '/><erase/></data>'),
                      protocol.PROGRAM_XML.replace('program ', 'program filename="other.img" ')]:
            with self.subTest(xml=wrong), self.assertRaises(ValueError):
                protocol.check_program_xml(wrong)

    def test_wrong_image_rejected_before_usb(self):
        with self.assertRaises(ValueError):
            protocol.program_recovery(None, b'not an image', 'install-diagnostic', {}, lambda: None)

    def test_cross_mode_payload_and_unrecognized_modes_rejected(self):
        from pathlib import Path
        directory = Path(__file__).resolve().parents[1] / 'firmware/extracted/recovery-probe-earlycon-20260915'
        if not directory.exists():
            directory = Path(__file__).resolve().parent
            files = [('install-diagnostic', directory / 'ram-staging/recovery-diagnostic-earlycon.img'),
                     ('restore-stock', directory / 'stock-recovery/restore-stock-recovery.img')]
        else:
            files = [('install-diagnostic', directory / 'recovery-diagnostic-unsigned.img'),
                     ('restore-stock', directory.parent / 'recovery-probe-20260914/restore-stock-recovery.img')]
        for mode, path in files:
            data = path.read_bytes()
            self.assertEqual(protocol.verify_payload(data, mode), protocol.TARGETS[mode])
            for wrong in [key for key in protocol.TARGETS if key != mode] + ['arbitrary', '']:
                with self.assertRaises(ValueError):
                    protocol.verify_payload(data, wrong)
            with self.assertRaises(ValueError):
                protocol.verify_payload(bytearray(data), mode)
            with self.assertRaises(ValueError):
                protocol.verify_payload(data[:-1], mode)

    def fake(self, initial=None, final=None, short_at=None):
        endpoint = Endpoint(short_at)
        cdc = SimpleNamespace(EP_OUT=endpoint, write=lambda *args: None)
        if initial is None:
            initial = {'value': 'ACK', 'rawmode': 'true'}
        if final is None:
            final = {'value': 'ACK', 'rawmode': 'false'}
        fh = SimpleNamespace(cdc=cdc, cfg=SimpleNamespace(SECTOR_SIZE_IN_BYTES=512,
            MemoryName='emmc', MaxPayloadSizeToTargetInBytes=512),
            xml=SimpleNamespace(getresponse=lambda data: data), wait_for_data=lambda: final)
        def xmlsend(data):
            cdc.write(data)
            return SimpleNamespace(resp=initial.get('value') == 'ACK', data=initial)
        fh.xmlsend = xmlsend
        return fh, endpoint

    def exercise(self, fh, report):
        # Small inert payload tests transfer state; the real payload validator is
        # independently exercised above and must be hash-checked before deployment.
        with patch.object(protocol, 'verify_payload', return_value=protocol.DIAGNOSTIC_HASH):
            protocol.program_recovery(fh, b'A' * 1024, 'install-diagnostic', report, lambda: None)

    def test_initial_failure_never_sends_payload(self):
        for initial in [{'value': 'NAK'}, {'value': 'ACK'}, {'value': 'ACK', 'rawmode': 'false'}]:
            fh, endpoint = self.fake(initial=initial)
            report = {}
            with self.assertRaises(IOError):
                self.exercise(fh, report)
            self.assertEqual(len(endpoint.calls), 1)
            self.assertEqual(report['bytes_transferred'], 0)
            self.assertFalse(report['write_acknowledged'])

    def test_short_write_is_never_retried(self):
        fh, endpoint = self.fake(short_at=2)
        original = fh.cdc.write
        report = {}
        with self.assertRaises(IOError):
            self.exercise(fh, report)
        self.assertEqual(len(endpoint.calls), 2)
        self.assertEqual(report['bytes_transferred'], 0)
        self.assertTrue(report['raw_mode_pending'])
        self.assertIs(fh.cdc.write, original)

    def test_final_ack_required_and_not_readback(self):
        for final in [{'value': 'NAK', 'rawmode': 'false'}, {'value': 'ACK'}, {}]:
            fh, endpoint = self.fake(final=final)
            report = {}
            with self.assertRaises(IOError):
                self.exercise(fh, report)
            self.assertFalse(report['write_acknowledged'])
        fh, endpoint = self.fake()
        report = {}
        self.exercise(fh, report)
        self.assertEqual(report['bytes_transferred'], 1024)
        self.assertEqual([len(c) for c in endpoint.calls[1:]], [512, 0, 512, 0])
        self.assertTrue(report['write_acknowledged'])
        self.assertFalse(report['raw_mode_pending'])
        self.assertFalse(report['independent_readback_verified'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
