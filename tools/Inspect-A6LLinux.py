#!/usr/bin/env python3
"""Direct Linux USB comparison for the spare A6L: fixed queries and reboots only."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

SERIAL = '1e529013'
EXPECTED = {
    'ro.build.fingerprint': 'Hisense/HLTE730T/HLTE730T:9/PKQ1.190723.001/L1632.6.01.04:user/release-keys',
    'sys.boot_completed': '1',
    'ro.boot.flash.locked': '1',
    'ro.boot.verifiedbootstate': 'green',
}


def exact_usb(vendor, product):
    found = []
    for device in Path('/sys/bus/usb/devices').glob('*'):
        try:
            if ((device / 'idVendor').read_text().strip() == vendor
                    and (device / 'idProduct').read_text().strip() == product
                    and (device / 'serial').read_text().strip().lower() == SERIAL):
                found.append(str(device))
        except (OSError, UnicodeError):
            continue
    return found


def android_usb():
    # Both Android USB compositions were observed on this exact spare. The
    # fingerprint/locked/green property guard still runs before any reboot.
    return exact_usb('109b', '911f') + exact_usb('109b', '9130')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--preflight-only', action='store_true', help='Check readiness without rebooting')
    parser.add_argument('--all-once', action='store_true', help='Run getvar all instead of repeated product queries')
    parser.add_argument('--fastboot', type=Path, help='Absolute path to a separately verified standard fastboot client')
    parser.add_argument('--require-no-lpm', action='store_true', help='Require the NO_LPM bit before fastboot queries')
    parser.add_argument('--ram-staging-dir', type=Path, help='Use the fixed hash-pinned RAM transfer collector')
    args = parser.parse_args()
    if args.fastboot is not None and (not args.fastboot.is_absolute() or not args.fastboot.is_file()):
        parser.error('--fastboot must identify an existing absolute file')
    if platform.system() != 'Linux' or 'microsoft' in platform.release().lower():
        parser.error('Run this comparison on the Linux laptop, outside WSL.')
    if os.geteuid() == 0:
        parser.error('Run as your normal user. The script requests sudo only for host USB capture.')
    output = (args.output or Path.cwd() / ('a6l-linux-' + datetime.now().strftime('%Y%m%d-%H%M%S'))).resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {'scope': 'Direct Linux USB; fixed getvar queries and reboot only',
              'serial': SERIAL, 'started_utc': datetime.now(timezone.utc).isoformat(),
              'host': {'kernel': platform.release(), 'machine': platform.machine()},
              'commands': [], 'bootloader_reboot_requested': False,
              'stock_return_verified': False}
    report_path = output / 'session.json'

    def save():
        report_path.write_text(json.dumps(report, indent=2) + '\n')

    def run(argv, timeout=10):
        entry = {'argv': list(map(str, argv))}
        try:
            result = subprocess.run(entry['argv'], capture_output=True, text=True, timeout=timeout)
            entry.update(exit=result.returncode, stdout=result.stdout, stderr=result.stderr)
        except subprocess.TimeoutExpired:
            entry.update(exit=None, error='Command timed out', timeout=timeout)
        except OSError as exc:
            entry.update(exit=None, error=str(exc))
        report['commands'].append(entry)
        save()
        return entry

    def required(argv, timeout=10):
        entry = run(argv, timeout)
        if entry['exit'] != 0:
            raise RuntimeError('Command failed: ' + ' '.join(entry['argv']) + '\n'
                               + entry.get('stderr', entry.get('error', '')))
        return entry

    def stock_properties(adb):
        values = {}
        for prop in EXPECTED:
            entry = run([adb, '-s', SERIAL, 'shell', 'getprop', prop], timeout=4)
            if entry['exit'] != 0:
                return None
            values[prop] = entry['stdout'].strip()
        return values

    reboot_requested = False
    collector_dir = output / 'fastboot'
    fastboot = None
    try:
        names = ['adb', 'fastboot', 'python3', 'sudo', 'modprobe', 'mount', 'mountpoint']
        search_path = os.environ.get('PATH', os.defpath) + ':/usr/sbin:/sbin'
        binaries = {name: shutil.which(name, path=search_path) for name in names}
        if args.fastboot is not None:
            binaries['fastboot'] = str(args.fastboot)
        missing = [name for name, path in binaries.items() if not path]
        if missing:
            raise RuntimeError('Missing laptop tools: ' + ', '.join(missing))
        adb, fastboot = binaries['adb'], binaries['fastboot']
        collector = Path(__file__).resolve().with_name('Inspect-FastbootNative-v2.py' if args.ram_staging_dir else 'Inspect-FastbootNative.py')
        if not collector.is_file():
            raise RuntimeError('Keep Inspect-FastbootNative.py beside this script.')
        report['files_sha256'] = {str(path): hashlib.sha256(Path(path).read_bytes()).hexdigest()
                                  for path in [adb, fastboot, str(collector), str(Path(__file__).resolve())]}
        os_release = Path('/etc/os-release')
        if os_release.exists():
            report['host']['os_release'] = os_release.read_text()
        required([adb, 'version'])
        required([fastboot, '--version'])
        # Authorize on the laptop as the normal user, before root USB work.
        state = run([adb, '-s', SERIAL, 'get-state'])
        if state['exit'] != 0 or state.get('stdout', '').strip() != 'device':
            raise RuntimeError('Unlock the spare and accept this laptop\'s USB debugging prompt, then rerun.')
        report['before_android_usb'] = android_usb()
        if len(report['before_android_usb']) != 1:
            raise RuntimeError('The exact spare is not directly connected in its recorded Android USB mode.')
        report['before_properties'] = stock_properties(adb)
        if report['before_properties'] != EXPECTED:
            raise RuntimeError('Spare identity or completed-boot/locked/green state differs from the baseline.')

        print('Preparing passive USB capture; sudo may request your laptop password here.', flush=True)
        if subprocess.run([binaries['sudo'], '-v']).returncode != 0:
            raise RuntimeError('Host USB capture needs sudo; the phone was not rebooted.')
        root_prefix = [binaries['sudo'], '-n']
        if run([binaries['mountpoint'], '-q', '/sys/kernel/debug'])['exit'] != 0:
            required([*root_prefix, binaries['mount'], '-t', 'debugfs', 'debugfs', '/sys/kernel/debug'])
        required([*root_prefix, binaries['modprobe'], 'usbmon'])
        # Test monitor access now, so a host setup failure cannot strand the phone.
        required([*root_prefix, binaries['python3'], '-c',
                  "import os; f=os.open('/sys/kernel/debug/usb/usbmon/0u', os.O_RDONLY|os.O_NONBLOCK); os.close(f)"])
        report['preflight_passed'] = True
        save()
        if args.preflight_only:
            print('Preflight passed. Phone remains in Android. Report: ' + str(report_path))
            return 0

        print('Identity and capture checks passed. Rebooting the spare into its existing bootloader.', flush=True)
        reboot_requested = True
        report['bootloader_reboot_requested'] = True
        required([adb, '-s', SERIAL, 'reboot', 'bootloader'])
        deadline = time.monotonic() + 35
        while len(exact_usb('18d1', 'd00d')) != 1 and time.monotonic() < deadline:
            time.sleep(0.5)
        if len(exact_usb('18d1', 'd00d')) != 1:
            raise RuntimeError('The exact spare did not appear as a bootloader within 35 seconds.')
        bootloader_path = Path(exact_usb('18d1', 'd00d')[0])
        report['bootloader_usb_settings'] = {}
        for attribute in ['quirks', 'speed', 'power/control', 'power/runtime_status',
                          'power/autosuspend_delay_ms', 'power/usb2_hardware_lpm']:
            if (bootloader_path / attribute).exists():
                report['bootloader_usb_settings'][attribute] = (bootloader_path / attribute).read_text().strip()
        save()
        if args.require_no_lpm and not int(report['bootloader_usb_settings'].get('quirks', '0'), 0) & 0x400:
            raise RuntimeError('The bootloader did not receive the required NO_LPM quirk; stopping queries.')
        print('Capturing fixed fastboot queries. The script will then request a normal reboot.', flush=True)
        argv = [*root_prefix, binaries['python3'], str(collector), str(collector_dir), '--fastboot', fastboot]
        if args.all_once:
            argv.append('--all-once')
        if args.ram_staging_dir:
            argv.extend(['--ram-staging-dir', str(args.ram_staging_dir)])
        # The collector has bounded subprocesses and attempts reboot in its finally block.
        entry = run(argv, timeout=190 if args.ram_staging_dir else 90)
        (output / 'collector-output.txt').write_text(entry.get('stdout', '') + entry.get('stderr', ''))
        if entry['exit'] != 0:
            report['collector_error'] = entry.get('error', 'Collector reported failure; inspect its report.')
    except (RuntimeError, OSError) as exc:
        report['error'] = str(exc)
        print(str(exc), file=sys.stderr, flush=True)
    finally:
        if reboot_requested:
            # Return ownership of only the collector directory and its three outputs.
            # The parent directory was newly created by this invocation.
            required_files = [str(collector_dir / name) for name in
                              ['report.json', 'usbmon.txt', 'usbmon-decoded.json']]
            run([binaries['sudo'], '-n', binaries['python3'], '-c',
                 'import os,sys; [(os.chown(p, int(sys.argv[1]), int(sys.argv[2]))) '
                 'for p in sys.argv[3:] if os.path.exists(p) and not os.path.islink(p)]',
                 str(os.getuid()), str(os.getgid()), str(collector_dir), *required_files])
            capture = {}
            try:
                capture = json.loads((collector_dir / 'report.json').read_text())
            except (OSError, ValueError):
                pass
            # Only cover failures before the collector attempted its own return command.
            # Do not keep retrying when a completed collector already attempted reboot.
            if 'reboot' not in capture and fastboot and len(exact_usb('18d1', 'd00d')) == 1:
                fallback = run([binaries['sudo'], '-n', fastboot, '-s', SERIAL, 'reboot'])
                if (fallback['exit'] != 0
                        and "remote: 'unknown command'" in fallback.get('stderr', '')):
                    time.sleep(0.5)
                    run([binaries['sudo'], '-n', fastboot, '-s', SERIAL, 'reboot'])
            print('Waiting up to 90 seconds to verify the original Android state.', flush=True)
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                if len(android_usb()) == 1:
                    values = stock_properties(adb)
                    report['after_properties'] = values
                    if values == EXPECTED:
                        report['stock_return_verified'] = True
                        break
                time.sleep(2)
            report['queries_succeeded'] = capture.get('queries_succeeded', False)
            if args.ram_staging_dir:
                report['ram_transfers_succeeded'] = capture.get('ram_transfers_succeeded', False)
                report['post_transfer_queries_succeeded'] = capture.get('post_transfer_queries_succeeded', False)
            if not report['stock_return_verified']:
                print('Automatic return was not verified. If it shows Press any key to shutdown, '
                      'briefly press a volume key, then Power to start Android. '
                      'If the screen is blank and unresponsive, hold ONLY Power '
                      'for about 20 seconds, as previously tested. Stop after it starts Android.',
                      file=sys.stderr, flush=True)
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
    print('Report: ' + str(report_path))
    if report['stock_return_verified']:
        print('Original Android is verified: same fingerprint, boot complete, locked, green.')
    transfer_ok = not args.ram_staging_dir or (report.get('ram_transfers_succeeded') and report.get('post_transfer_queries_succeeded'))
    if report.get('queries_succeeded') and report['stock_return_verified'] and transfer_ok:
        print('The fixed queries succeeded; compare the saved USB trace with the desktop captures.')
        return 0
    return 1


if __name__ == '__main__':
    raise SystemExit(main())
