#!/usr/bin/env python3
"""One fixed USB comparison with idle fwupd paused only for this session."""
from datetime import datetime, timezone
import argparse
import importlib.util
import json
import os
from pathlib import Path
import pwd
import socket
import subprocess
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--google-client', action='store_true')
parser.add_argument('--no-lpm', action='store_true', help='Temporarily add only the bootloader NO_LPM quirk')
parser.add_argument('--ram-transfer', action='store_true', help='Use the verified no-LPM setup to stage two fixed payloads in RAM')
parser.add_argument('--vendor-query', action='store_true', help='Read-only preflight using the guarded source-built client')
parser.add_argument('--vendor-unlock', action='store_true', help='Apply the reviewed vendor unlock only after explicit user approval')
args = parser.parse_args()
if sum([args.ram_transfer, args.vendor_query, args.vendor_unlock]) > 1:
    parser.error('Choose only one follow-up phase')
vendor_phase = args.vendor_query or args.vendor_unlock
if args.ram_transfer or vendor_phase:
    args.no_lpm = True
if args.no_lpm:
    args.google_client = True
assert os.geteuid() != 0, 'Run as the normal laptop user'
assert pwd.getpwuid(os.getuid()).pw_name == 'pierrelouis'
assert socket.gethostname().split('.')[0] == 'system76-pc'
ROOT = Path(__file__).resolve().parent
case = ('vendor-unlock' if args.vendor_unlock else 'vendor-query') if vendor_phase else ('ram-transfer' if args.ram_transfer else ('no-lpm' if args.no_lpm else ('google' if args.google_client else 'quiet')))
TARGET = ROOT / ('capture-' + case)
REPORT = ROOT / ('fwupd-' + case + '-session.json')
assert not REPORT.exists() and not TARGET.exists(), 'Refusing to overwrite an earlier test'
spec = importlib.util.spec_from_file_location('a6l_runner', ROOT / 'Inspect-A6LLinux.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
report = {'scope': 'Fixed native USB test with fwupd paused temporarily',
          'started_utc': datetime.now(timezone.utc).isoformat(), 'commands': [],
          'service_restored': False}
runtime_mask = Path('/run/systemd/system/fwupd.service')
masked = False
was_active = False
test_exit = 1
monitor = None
monitor_log = None
setup_dir = ROOT / (case + '-usb-setup')
quirks_path = Path('/sys/module/usbcore/parameters/quirks')
previous_quirks = None
test_quirks = None
quirks_attempted = False


def save():
    REPORT.write_text(json.dumps(report, indent=2) + '\n')


def run(argv, required=True):
    result = subprocess.run(argv, capture_output=True, text=True, timeout=25)
    report['commands'].append({'argv': argv, 'exit': result.returncode,
                               'stdout': result.stdout, 'stderr': result.stderr})
    save()
    if required and result.returncode != 0:
        raise RuntimeError('Command failed: ' + ' '.join(argv) + '\n' + result.stderr)
    return result


def replace_quirks(expected, replacement):
    # Compare immediately before changing this runtime parameter. Preserve all
    # pre-existing entries and refuse to overwrite an external concurrent change.
    code = (
        'import pathlib,sys; p=pathlib.Path("/sys/module/usbcore/parameters/quirks"); '
        'assert p.read_text().strip()==sys.argv[1], "USB quirks changed externally"; '
        'p.write_text(sys.argv[2]+"\\n"); '
        'assert p.read_text().strip()==sys.argv[2], "USB quirk readback differs"'
    )
    return run(['/usr/bin/sudo', '-n', sys.executable, '-c', code, expected, replacement])


try:
    assert not runtime_mask.exists() and not runtime_mask.is_symlink(), 'A runtime unit override already exists'
    for prop, expected in runner.EXPECTED.items():
        assert run(['/usr/bin/adb', '-s', runner.SERIAL, 'shell', 'getprop', prop]).stdout.strip() == expected
    if args.ram_transfer:
        ram_spec = importlib.util.spec_from_file_location('ram_collector', ROOT / 'Inspect-FastbootNative-v2.py')
        ram_collector = importlib.util.module_from_spec(ram_spec)
        ram_spec.loader.exec_module(ram_collector)
        report['ram_payloads'] = ram_collector.verify_ram_payloads(ROOT / 'ram-staging')
        report['scope'] = 'Fixed RAM-only staging comparison with reversible host USB settings'
        save()
    print('The spare matches its baseline. Enter the laptop password for sudo.', flush=True)
    if subprocess.run(['/usr/bin/sudo', '-v']).returncode != 0:
        raise RuntimeError('Sudo authorization failed; no service or phone mode changed')
    state = run(['/usr/bin/systemctl', 'show', 'fwupd.service', '-p', 'LoadState', '-p', 'ActiveState'])
    assert 'LoadState=loaded' in state.stdout, 'Unexpected fwupd unit state'
    was_active = 'ActiveState=active' in state.stdout
    report['was_active'] = was_active
    if was_active:
        status = run(['/usr/bin/busctl', '--system', 'get-property', 'org.freedesktop.fwupd',
                      '/', 'org.freedesktop.fwupd', 'Status'])
        assert status.stdout.strip() == 'u 1', 'fwupd is not idle; do not interrupt firmware activity'
    # Set the cleanup flag before the operation, including partial-success cases.
    masked = True
    run(['/usr/bin/sudo', '-n', '/usr/bin/systemctl', 'mask', '--runtime', '--now', 'fwupd.service'])
    assert runtime_mask.is_symlink() and os.readlink(runtime_mask) == '/dev/null'
    state = run(['/usr/bin/systemctl', 'show', 'fwupd.service', '-p', 'LoadState', '-p', 'ActiveState'])
    assert 'LoadState=masked' in state.stdout and 'ActiveState=inactive' in state.stdout
    report['fwupd_paused'] = True
    save()
    if args.no_lpm:
        previous_quirks = quirks_path.read_text().strip()
        entries = previous_quirks.split(',') if previous_quirks else []
        assert all(not e.lower().startswith('18d1:d00d:') for e in entries), 'Bootloader already has a runtime USB quirk'
        assert not runner.exact_usb('18d1', 'd00d'), 'Apply the quirk before bootloader enumeration'
        test_quirks = ','.join(entries + ['18d1:d00d:k'])
        report['usb_quirks'] = {'before': previous_quirks, 'during': test_quirks,
                                'restored': False, 'persistent': False}
        save()
        quirks_attempted = True
        replace_quirks(previous_quirks, test_quirks)
        print('Temporary bootloader-only USB NO_LPM quirk applied.', flush=True)
    print('fwupd is temporarily paused. Running phase: ' + case, flush=True)
    command = [sys.executable, str(ROOT / 'Inspect-A6LLinux.py'), '--output', str(TARGET)]
    if args.google_client:
        run(['/usr/bin/sudo', '-n', '/usr/sbin/modprobe', 'usbmon'])
        setup_dir.mkdir(mode=0o700, exist_ok=False)
        monitor_log = (setup_dir / 'monitor-output.txt').open('x')
        monitor = subprocess.Popen(['/usr/bin/sudo', '-n', sys.executable,
                                     str(ROOT / ('Capture-LaptopUsbSetup-v3.py' if args.ram_transfer or vendor_phase else ('Capture-LaptopUsbSetup-v2.py' if args.no_lpm else 'Capture-LaptopUsbSetup.py'))), str(setup_dir)],
                                    stdout=monitor_log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 10
        while not (setup_dir / 'ready.json').exists() and time.monotonic() < deadline:
            if monitor.poll() is not None:
                raise RuntimeError('Full USB setup monitor failed before any reboot')
            time.sleep(0.1)
        assert (setup_dir / 'ready.json').exists(), 'Full USB setup monitor did not become ready'
        command = [sys.executable, str(ROOT / ('Inspect-A6LLinux-v4.py' if args.ram_transfer else ('Inspect-A6LLinux-v3.py' if args.no_lpm else 'Inspect-A6LLinux-v2.py'))), '--output', str(TARGET),
                   '--fastboot', str(ROOT / 'google-fastboot-37.0.1-linux/fastboot')]
        if args.no_lpm:
            command.append('--require-no-lpm')
        if args.ram_transfer:
            command.extend(['--ram-staging-dir', str(ROOT / 'ram-staging')])
        if vendor_phase:
            command = [sys.executable, str(ROOT / 'Run-LaptopUnlockPhase.py'), '--output', str(TARGET),
                       '--apply-reviewed-unlock' if args.vendor_unlock else '--preflight-only']
            report['scope'] = 'Reviewed vendor unlock phase' if args.vendor_unlock else 'Read-only vendor-client preflight'
            save()
    test_exit = subprocess.run(command).returncode
    report['test_exit'] = test_exit
except (AssertionError, OSError, RuntimeError, subprocess.SubprocessError) as exc:
    report['error'] = str(exc)
    print(str(exc), file=sys.stderr, flush=True)
finally:
    if monitor is not None:
        (setup_dir / 'stop').touch()
        try:
            monitor.wait(timeout=5)
        except subprocess.TimeoutExpired:
            report['monitor_error'] = 'Recorder did not stop promptly; its own bounded deadline remains in force'
        if monitor_log is not None:
            monitor_log.close()
    if quirks_attempted:
        try:
            current_quirks = quirks_path.read_text().strip()
            if current_quirks == test_quirks:
                replace_quirks(test_quirks, previous_quirks)
            elif current_quirks != previous_quirks:
                raise RuntimeError('USB quirks changed externally; refusing to overwrite them')
            report['usb_quirks']['restored'] = quirks_path.read_text().strip() == previous_quirks
            print('USB quirk setting restored: ' + str(report['usb_quirks']['restored']), flush=True)
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            report['quirk_restore_error'] = str(exc)
            test_exit = 1
        save()
    if masked:
        # Avoid restarting the probing service while the spare is still in bootloader.
        if runner.exact_usb('18d1', 'd00d'):
            report['waiting_for_stock_return_before_service_restore'] = True
            save()
            print('If the spare shows Press any key to shutdown, briefly press a volume key, then Power to boot. '
                  'For a blank unresponsive screen, hold only Power for about 20 seconds. '
                  'Waiting up to two minutes before restoring fwupd.', flush=True)
            deadline = time.monotonic() + 120
            while runner.exact_usb('18d1', 'd00d') and time.monotonic() < deadline:
                time.sleep(2)
        try:
            if runner.exact_usb('18d1', 'd00d'):
                raise RuntimeError('Spare still in bootloader: fwupd remains paused pending manual recovery')
            if runtime_mask.is_symlink() and os.readlink(runtime_mask) == '/dev/null':
                run(['/usr/bin/sudo', '-n', '/usr/bin/systemctl', 'unmask', '--runtime', 'fwupd.service'])
            elif runtime_mask.exists() or runtime_mask.is_symlink():
                raise RuntimeError('Runtime unit override changed unexpectedly; do not overwrite it')
            if was_active:
                run(['/usr/bin/sudo', '-n', '/usr/bin/systemctl', 'start', 'fwupd.service'])
            restored = run(['/usr/bin/systemctl', 'show', 'fwupd.service', '-p', 'LoadState', '-p', 'ActiveState'])
            report['service_restored'] = ('LoadState=loaded' in restored.stdout and
                                          ('ActiveState=active' if was_active else 'ActiveState=inactive') in restored.stdout)
            print('Firmware-management service restored: ' + str(report['service_restored']), flush=True)
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            report['restore_error'] = str(exc)
            print(str(exc), file=sys.stderr, flush=True)
    report['finished_utc'] = datetime.now(timezone.utc).isoformat()
    save()
raise SystemExit(test_exit if report['service_restored'] else 1)
