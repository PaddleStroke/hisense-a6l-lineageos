"""Offline full-payload transfer faults, operation gates and Windows supervision."""
import hashlib
import json
import multiprocessing
from pathlib import Path
from types import SimpleNamespace
import unittest
import WindowsRecoveryTrial as trial
import WindowsRecoverySerialProtocol as protocol
from DiagnosticRecoveryProtocolV11 import PROGRAM_XML


class FakeDevice:
    write_timeout = 5
    def __init__(self, fault=None):
        self.fault = fault
        self.calls = 0
        self.bytes = 0
    def write(self, data):
        self.calls += 1
        if self.fault == self.calls:
            return len(data) - 1
        if self.fault == -self.calls:
            raise TimeoutError('Injected timeout')
        self.bytes += len(data)
        return len(data)


class FakeFirehose:
    def __init__(self, fault=None, initial=True, final=True):
        self.cfg = SimpleNamespace(SECTOR_SIZE_IN_BYTES=512, MemoryName='eMMC', ZLPAwareHost=0,
                                   MaxPayloadSizeToTargetInBytes=1048576)
        self.cdc = SimpleNamespace(is_serial=True, device=FakeDevice(fault), write=lambda data: True)
        self.xml = SimpleNamespace(getresponse=lambda data: data)
        self.initial = initial
        self.final = final
        self.xml_calls = 0
    def xmlsend(self, xml):
        self.xml_calls += 1
        self.cdc.write(xml)
        return SimpleNamespace(resp=self.initial, data={'value': 'ACK' if self.initial else 'NAK', 'rawmode': 'true'})
    def wait_for_data(self):
        return {'value': 'ACK' if self.final else 'NAK', 'rawmode': 'false'}


def dormant_child():
    import time
    time.sleep(60)


class TrialTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.payload = trial.PAYLOADS['install-diagnostic'].read_bytes()
        cls.before = {name: (trial.ROOT / 'captures/windows-edl-readonly-v1/edl' / (name + '.bin')).read_bytes()
                      for name in trial.base.REGIONS}

    def ready_gate(self):
        gate = trial.TrialGate('install-diagnostic')
        for name, data in self.before.items():
            gate.verify_region(name, data)
        return gate

    def test_success_exact_full_payload_no_empty_writes(self):
        fh, report = FakeFirehose(), {}
        protocol.program(fh, self.payload, 'install-diagnostic', report, lambda: None)
        self.assertEqual(fh.cdc.device.calls, 65)
        self.assertEqual(fh.cdc.device.bytes, len(PROGRAM_XML.encode()) + 67108864)
        self.assertTrue(report['write_acknowledged'])
        self.assertFalse(report['raw_mode_pending'])

    def test_short_and_timed_out_writes_never_retry(self):
        for fault in (1, 2, 33, 65, -1, -2, -65):
            with self.subTest(fault=fault):
                fh, report = FakeFirehose(fault=fault), {}
                with self.assertRaises((IOError, TimeoutError)):
                    protocol.program(fh, self.payload, 'install-diagnostic', report, lambda: None)
                self.assertEqual(fh.cdc.device.calls, abs(fault))
                self.assertEqual(fh.xml_calls, 1)
                self.assertFalse(report['write_acknowledged'])
                self.assertTrue(report['raw_mode_pending'])

    def test_missing_ack_stops(self):
        for initial, final, calls in [(False, True, 1), (True, False, 65)]:
            fh, report = FakeFirehose(initial=initial, final=final), {}
            with self.assertRaises(IOError):
                protocol.program(fh, self.payload, 'install-diagnostic', report, lambda: None)
            self.assertEqual(fh.cdc.device.calls, calls)
            self.assertFalse(report['write_acknowledged'])

    def test_wrong_payload_geometry_timeout_no_write(self):
        fh = FakeFirehose()
        with self.assertRaises(ValueError):
            protocol.program(fh, self.payload, 'restore-stock', {}, lambda: None)
        for field, value in [('ZLPAwareHost', 1), ('SECTOR_SIZE_IN_BYTES', 4096),
                             ('MaxPayloadSizeToTargetInBytes', 513)]:
            fh = FakeFirehose()
            setattr(fh.cfg, field, value)
            with self.assertRaises(ValueError):
                protocol.program(fh, self.payload, 'install-diagnostic', {}, lambda: None)
            self.assertEqual(fh.cdc.device.calls, 0)
        device = FakeDevice()
        device.write_timeout = None
        with self.assertRaises(ValueError):
            protocol.exact_write(device, b'data')
        self.assertEqual(device.calls, 0)

    def test_gate_sequence_and_repeated_program_block(self):
        gate = trial.TrialGate('install-diagnostic')
        with self.assertRaises(ValueError):
            gate.begin(PROGRAM_XML)
        gate = self.ready_gate()
        self.assertEqual(gate.begin(PROGRAM_XML), 'program')
        for xml in ('<data><nop/></data>', '<data><power value="off"/></data>', PROGRAM_XML):
            with self.assertRaises(ValueError):
                gate.begin(xml)
        gate.complete_write({'write_acknowledged': True, 'raw_mode_pending': False, 'bytes_transferred': 67108864})
        with self.assertRaises(ValueError):
            gate.begin(PROGRAM_XML)
        with self.assertRaises(ValueError):
            gate.begin('<data><power value="off"/></data>')
        for name, data in self.before.items():
            gate.verify_region(name, self.payload if name == 'recovery' else data)
        with self.assertRaises(ValueError):
            gate.begin('<data><power value="reset"/></data>')
        self.assertEqual(gate.begin('<data><power value="off"/></data>'), 'power')
        with self.assertRaises(ValueError):
            gate.begin('<data><power value="off"/></data>')

    def test_gate_corruption_pending_ambiguity_and_forbidden_commands(self):
        gate = self.ready_gate()
        for xml in ('<data><erase/></data>', '<data><patch/></data>',
                    PROGRAM_XML.replace('917504', '917505'),
                    '<data><nop/><program/></data>', '<data><nop><program/></nop></data>'):
            with self.assertRaises(ValueError):
                gate.begin(xml)
        for field in ('pending', 'ambiguous'):
            gate = self.ready_gate()
            setattr(gate, field, True)
            with self.assertRaises(ValueError):
                gate.begin(PROGRAM_XML)
        gate = trial.TrialGate('install-diagnostic')
        with self.assertRaises(ValueError):
            gate.verify_region('misc-bcb', b'x' * 4096)

    def test_restore_accepts_diagnostic_only_and_preserves_bcb(self):
        gate = trial.TrialGate('restore-stock')
        with self.assertRaises(ValueError):
            gate.verify_region('recovery', self.before['recovery'])
        gate.verify_region('recovery', self.payload)
        for name, data in self.before.items():
            if name != 'recovery':
                gate.verify_region(name, data)
        gate.begin(PROGRAM_XML)
        gate.complete_write({'write_acknowledged': True, 'raw_mode_pending': False, 'bytes_transferred': 67108864})
        for name, data in self.before.items():
            gate.verify_region(name, data)
        self.assertEqual(gate.begin('<data><power value="reset"/></data>'), 'power')

    def test_external_supervisor_can_terminate_spawned_worker(self):
        child = multiprocessing.get_context('spawn').Process(target=dormant_child)
        child.start()
        child.join(.2)
        self.assertTrue(child.is_alive())
        child.kill()
        child.join(5)
        self.assertFalse(child.is_alive())
        self.assertNotEqual(child.exitcode, 0)


if __name__ == '__main__':
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(TrialTests))
    if not result.wasSuccessful():
        raise SystemExit(1)
    trial.base.verify_inputs()
    for mode in trial.MODES:
        trial.verify_payload(trial.PAYLOADS[mode].read_bytes(), mode)
    report = {'passed': True, 'tests': result.testsRun,
              'tested_tools': {name: hashlib.sha256((trial.ROOT / 'tools' / name).read_bytes()).hexdigest()
                               for name in trial.TESTED_FILES},
              'limits': 'Offline tests only. No native Windows persistent transfer has yet been exercised.'}
    trial.base.save(trial.ROOT / 'firmware/extracted/windows-recovery-trial-preflight.json', report)
    print(json.dumps(report, indent=2))
