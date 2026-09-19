#!/usr/bin/env python3
"""Offline fault injection: laptop preflight must not reboot an unready phone."""
from contextlib import ExitStack, redirect_stderr, redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('a6l_linux', Path(__file__).with_name('Inspect-A6LLinux.py'))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class PreflightGuards(unittest.TestCase):
    def simulate(self, failure=None):
        calls = []

        def execute(argv, **kwargs):
            calls.append(argv)
            code, output, error = 0, '', ''
            if 'get-state' in argv:
                code, output = (1, '') if failure == 'unauthorized' else (0, 'device\n')
            elif 'getprop' in argv:
                prop = argv[-1]
                output = runner.EXPECTED[prop]
                if failure == 'wrong-firmware' and prop == 'ro.build.fingerprint':
                    output = 'Hisense/another-phone-or-firmware'
            elif any('/usbmon/0u' in str(arg) for arg in argv) and failure == 'no-monitor':
                code, error = 1, 'Permission denied opening usbmon'
            elif 'reboot' in argv:
                self.fail('Preflight sent a phone reboot')
            return subprocess.CompletedProcess(argv, code, output, error)

        with tempfile.TemporaryDirectory() as directory, ExitStack() as stack:
            output = Path(directory) / 'capture'
            stack.enter_context(patch.object(sys, 'argv', ['Inspect-A6LLinux.py', '--preflight-only',
                                                           '--output', str(output)]))
            stack.enter_context(patch.object(runner.platform, 'system', return_value='Linux'))
            stack.enter_context(patch.object(runner.platform, 'release', return_value='fixture-native-linux'))
            stack.enter_context(patch.object(runner.os, 'geteuid', return_value=1000, create=True))
            stack.enter_context(patch.object(runner.shutil, 'which', return_value=sys.executable))
            stack.enter_context(patch.object(runner, 'exact_usb',
                                             side_effect=lambda vendor, product:
                                             ['fixture-usb-device'] if product == '9130' else []))
            stack.enter_context(patch.object(runner.subprocess, 'run', side_effect=execute))
            stack.enter_context(redirect_stdout(io.StringIO()))
            stack.enter_context(redirect_stderr(io.StringIO()))
            result = runner.main()
            report = json.loads((output / 'session.json').read_text())
        return result, report, calls

    def test_unauthorized_stops_before_sudo_or_reboot(self):
        result, report, calls = self.simulate('unauthorized')
        self.assertEqual(result, 1)
        self.assertFalse(report['bootloader_reboot_requested'])
        self.assertFalse(any('-v' in argv for argv in calls))

    def test_different_firmware_stops_before_sudo_or_reboot(self):
        result, report, calls = self.simulate('wrong-firmware')
        self.assertEqual(result, 1)
        self.assertFalse(report['bootloader_reboot_requested'])
        self.assertFalse(any('-v' in argv for argv in calls))

    def test_monitor_failure_leaves_android_running(self):
        result, report, calls = self.simulate('no-monitor')
        self.assertEqual(result, 1)
        self.assertFalse(report['bootloader_reboot_requested'])
        self.assertIn('Permission denied', report['error'])

    def test_valid_preflight_does_not_enter_bootloader(self):
        result, report, calls = self.simulate()
        self.assertEqual(result, 0)
        self.assertTrue(report['preflight_passed'])
        self.assertFalse(report['bootloader_reboot_requested'])
        self.assertEqual(report['before_properties'], runner.EXPECTED)


if __name__ == '__main__':
    unittest.main(verbosity=2)
