"""Validate allowed direct replacements against actual saved device reads."""
from pathlib import Path
import unittest
import RecoveryTransitionV21 as policy

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'captures/capture-diagnostic-restore-user-v13/edl'

class TransitionChecks(unittest.TestCase):
    def setUp(self):
        self.info = (FIXTURE / 'devinfo.bin').read_bytes()
        self.bcb = (FIXTURE / 'misc-bcb.bin').read_bytes()

    def test_exact_stock_and_previous_with_each_known_boot_message(self):
        for image in (policy.STOCK, policy.PREVIOUS):
            for bcb in (bytes(4096), self.bcb):
                result = policy.verify_transition(image, bcb, self.info)
                self.assertTrue(result['boot_message_preserved'])

    def test_unknown_or_partial_image_rejected(self):
        for image in ('', '0' * 64, policy.PREVIOUS[:-1]):
            with self.assertRaises(ValueError):
                policy.verify_transition(image, self.bcb, self.info)

    def test_boot_prefix_is_not_enough(self):
        for bcb in (self.bcb[:-1], self.bcb[:-1] + b'x',
                    b'boot-recovery' + bytes(4096-13),
                    b'bootonce-bootloader' + bytes(4096-19-1) + b'x'):
            with self.assertRaises(ValueError):
                policy.verify_transition(policy.PREVIOUS, bcb, self.info)

    def test_modified_device_state_rejected(self):
        for info in (self.info[:-1], bytes(4096), self.info[:-1] + bytes([self.info[-1] ^ 1])):
            with self.assertRaises(ValueError):
                policy.verify_transition(policy.STOCK, bytes(4096), info)

if __name__ == '__main__':
    unittest.main(verbosity=2)
