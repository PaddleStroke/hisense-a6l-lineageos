#!/usr/bin/env python3
"""Fault injection for the two-command vendor unlock sequence; no device access."""
import importlib.util
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch

# The target module uses pwd only in its Linux entry point.
with patch.dict(sys.modules, {'pwd': sys.modules.get('pwd', types.ModuleType('pwd'))}):
    spec = importlib.util.spec_from_file_location('phase', Path(__file__).with_name('Run-LaptopUnlockPhase.py'))
    phase = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(phase)


class Sequence(unittest.TestCase):
    def exercise(self, fail=None):
        report = {'runtime_unlock_attempted': False, 'persistence_attempted': False,
                  'persistence_acknowledged': False}
        calls = []
        checkpoints = []

        def call(command, vendor=False):
            calls.append((command, vendor))
            if command[0] == 'erase':
                self.assertTrue(report['persistence_attempted'])
            text = ('Device unlocked: true\nDevice critical unlocked: true\nOKAY'
                    if command[0] == 'oem' else 'OKAY')
            return {'ok': command[0] != fail, 'stdout': '', 'stderr': text}

        try:
            phase.vendor_sequence(call, report, lambda: checkpoints.append(dict(report)))
        except RuntimeError:
            pass
        return report, calls, checkpoints

    def test_runtime_command_failure_never_persists(self):
        report, calls, _ = self.exercise('Hisense')
        self.assertEqual(len(calls), 1)
        self.assertFalse(report['persistence_attempted'])

    def test_failed_runtime_readback_never_persists(self):
        report, calls, _ = self.exercise('oem')
        self.assertEqual(len(calls), 2)
        self.assertFalse(report['persistence_attempted'])

    def test_persistent_failure_is_journaled_and_never_retried(self):
        report, calls, checkpoints = self.exercise('erase')
        self.assertTrue(report['persistence_attempted'])
        self.assertFalse(report['persistence_acknowledged'])
        self.assertEqual(sum(c[0][0] == 'erase' for c in calls), 1)
        self.assertTrue(checkpoints[-1]['persistence_attempted'])

    def test_exact_sequence_and_success(self):
        report, calls, _ = self.exercise()
        self.assertEqual(calls, [(['Hisense', 'unlock'], True),
                                 (['oem', 'device-info'], False),
                                 (['erase', 'avb_custom_key'], False)])
        self.assertTrue(report['persistence_acknowledged'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
