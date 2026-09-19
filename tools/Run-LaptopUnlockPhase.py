#!/usr/bin/env python3
"""Guarded A6L vendor-unlock phase. Requires an explicit apply flag and prior user approval.

The preflight mode uses ordinary queries only. Neither mode flashes an image.
Run through the reviewed host wrapper, which temporarily pauses fwupd, applies
NO_LPM before enumeration, captures USB traffic, and restores host settings.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import pwd
import re
import socket
import subprocess
import time

SERIAL = '1e529013'
VENDOR_HASH = '83544be427b706817537a6ee888617d50dc6dfa84c89c6d25d1b150fe46aa3d6'
GOOGLE_HASH = 'a686e2c7e8dc9cf4cba0cb8a2eef05f7b2bd682c925abd032fe203215d80b618'
RECOVERY_HASH = '9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621'


def require_text(result, expected):
    if not result['ok'] or any(text not in result['stdout'] + result['stderr'] for text in expected):
        raise RuntimeError('Expected bootloader response was not received: ' + ', '.join(expected))


def vendor_sequence(call, report, save):
    """Only the two reviewed state-changing commands, with no automatic retry."""
    report['runtime_unlock_attempted'] = True
    save()
    require_text(call(['Hisense', 'unlock'], vendor=True), ['OKAY'])
    require_text(call(['oem', 'device-info']),
                 ['Device unlocked: true', 'Device critical unlocked: true'])
    report['runtime_unlock_verified'] = True
    save()
    # This is the point that may alter protected storage. Journal before sending.
    report['persistence_attempted'] = True
    save()
    require_text(call(['erase', 'avb_custom_key']), ['OKAY'])
    report['persistence_acknowledged'] = True
    save()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument('--preflight-only', action='store_true')
    modes.add_argument('--apply-reviewed-unlock', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if os.geteuid() == 0 or pwd.getpwuid(os.getuid()).pw_name != 'pierrelouis' or socket.gethostname().split('.')[0] != 'system76-pc':
        parser.error('Run as the established normal user on the Linux laptop')
    root = Path(__file__).resolve().parent
    args.output.mkdir(mode=0o700, exist_ok=False)
    report_path = args.output / 'session.json'
    report = {'scope': 'Read-only vendor-client preflight' if args.preflight_only else 'Reviewed vendor unlock only; no image flash',
              'started_utc': datetime.now(timezone.utc).isoformat(), 'commands': [],
              'runtime_unlock_attempted': False, 'persistence_attempted': False,
              'persistence_acknowledged': False, 'android_return_verified': False}
    bootloader_requested = False
    google = root / 'google-fastboot-37.0.1-linux/fastboot'
    vendor = root / 'a6l-fastboot-vendor-linux/fastboot'
    spec = importlib.util.spec_from_file_location('baseline', root / 'Inspect-A6LLinux.py')
    baseline = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(baseline)

    def save():
        report_path.write_text(json.dumps(report, indent=2) + '\n')

    def run(argv, timeout=12, vendor_enabled=False):
        entry = {'argv': list(map(str, argv)), 'started_utc': datetime.now(timezone.utc).isoformat()}
        report['commands'].append(entry)
        save()
        environment = dict(os.environ)
        environment.pop('A6L_QUERY_PAD64', None)
        environment.pop('A6L_VENDOR_UNLOCK', None)
        if vendor_enabled:
            # sudo strips caller environment: pass only this exact value via env.
            entry['argv'][2:2] = ['/usr/bin/env', 'A6L_VENDOR_UNLOCK=1']
        try:
            result = subprocess.run(entry['argv'], capture_output=True, text=True,
                                    env=environment, timeout=timeout)
            entry.update(exit=result.returncode, stdout=result.stdout, stderr=result.stderr,
                         ok=result.returncode == 0 and 'FAILED' not in result.stderr)
        except subprocess.TimeoutExpired as exc:
            entry.update(exit=None, ok=False, timeout=timeout,
                         stdout=(exc.stdout or b'').decode(errors='replace'),
                         stderr=(exc.stderr or b'').decode(errors='replace'))
        except OSError as exc:
            entry.update(exit=None, ok=False, stdout='', stderr=str(exc))
        save()
        return entry

    def phone(arguments, vendor=False):
        allowed = [
            ['getvar', 'product', 'getvar', 'unlocked', 'getvar', 'secure'],
            ['getvar', 'product', 'getvar', 'product', 'getvar', 'product'],
            ['oem', 'device-info'], ['flashing', 'get_unlock_ability'], ['reboot'],
        ]
        if args.apply_reviewed_unlock:
            allowed += [['Hisense', 'unlock'], ['erase', 'avb_custom_key']]
        if arguments not in allowed or (arguments == ['Hisense', 'unlock'] and not vendor):
            raise RuntimeError('Command rejected by the phase allowlist')
        if len(baseline.exact_usb('18d1', 'd00d')) != 1:
            raise RuntimeError('Exact spare bootloader disappeared; do not send another command')
        binary = root / 'a6l-fastboot-vendor-linux/fastboot' if vendor else google
        return run(['/usr/bin/sudo', '-n', str(binary), '-s', SERIAL, *arguments],
                   timeout=45 if arguments == ['erase', 'avb_custom_key'] else 15,
                   vendor_enabled=arguments == ['Hisense', 'unlock'])

    try:
        for path, digest in [(google, GOOGLE_HASH), (vendor, VENDOR_HASH),
                             (root / 'stock-recovery/restore-stock-recovery.img', RECOVERY_HASH)]:
            with path.open('rb') as stream:
                actual = hashlib.sha256(stream.read()).hexdigest()
            if actual != digest:
                raise RuntimeError('A reviewed input hash differs: ' + path.name)
        if Path('/sys/module/usbcore/parameters/quirks').read_text().strip().split(',').count('18d1:d00d:k') != 1:
            raise RuntimeError('The reviewed NO_LPM wrapper has not prepared this session')
        state = run(['/usr/bin/systemctl', 'show', 'fwupd.service', '-p', 'LoadState', '-p', 'ActiveState'])
        require_text(state, ['LoadState=masked', 'ActiveState=inactive'])
        for prop, expected in baseline.EXPECTED.items():
            result = run(['/usr/bin/adb', '-s', SERIAL, 'shell', 'getprop', prop])
            if not result['ok'] or result['stdout'].strip() != expected:
                raise RuntimeError('Original spare Android baseline changed: ' + prop)
        battery = run(['/usr/bin/adb', '-s', SERIAL, 'shell', 'dumpsys', 'battery'])
        level = re.search(r'^\s*level:\s*(\d+)\s*$', battery['stdout'], re.M)
        if not battery['ok'] or not level or int(level.group(1)) < 40:
            raise RuntimeError('Battery is not confirmed at least 40 percent')
        report['battery_percent'] = int(level.group(1))
        bootloader_requested = True
        if not run(['/usr/bin/adb', '-s', SERIAL, 'reboot', 'bootloader'])['ok']:
            raise RuntimeError('Bootloader reboot command failed')
        deadline = time.monotonic() + 35
        while not baseline.exact_usb('18d1', 'd00d') and time.monotonic() < deadline:
            time.sleep(0.25)
        devices = baseline.exact_usb('18d1', 'd00d')
        if len(devices) != 1 or not int((Path(devices[0]) / 'quirks').read_text().strip(), 0) & 0x400:
            raise RuntimeError('Exact bootloader with NO_LPM was not found')
        require_text(phone(['getvar', 'product', 'getvar', 'product', 'getvar', 'product'], vendor=True), ['product: sdm660'])
        require_text(phone(['getvar', 'product', 'getvar', 'unlocked', 'getvar', 'secure']),
                     ['product: sdm660', 'unlocked: no', 'secure: yes'])
        require_text(phone(['oem', 'device-info']), ['Device unlocked: false', 'Device critical unlocked: false'])
        require_text(phone(['flashing', 'get_unlock_ability']), ['get_unlock_ability: 1'])
        report['preflight_passed'] = True
        save()
        if args.apply_reviewed_unlock:
            vendor_sequence(phone, report, save)
    except (OSError, RuntimeError, ValueError) as exc:
        report['error'] = str(exc)
        print(str(exc), flush=True)
    finally:
        # Do not issue even a reboot after an unacknowledged persistent operation.
        ambiguous = report['persistence_attempted'] and not report['persistence_acknowledged']
        if bootloader_requested and not ambiguous:
            if len(baseline.exact_usb('18d1', 'd00d')) == 1:
                try:
                    report['reboot'] = phone(['reboot'])
                except RuntimeError as exc:
                    report['reboot_error'] = str(exc)
            deadline = time.monotonic() + 100
            while time.monotonic() < deadline:
                if len(baseline.android_usb()) == 1:
                    state = run(['/usr/bin/adb', '-s', SERIAL, 'get-state'], timeout=4)
                    if not state['ok']:
                        report['requires_android_setup_or_debugging_authorization'] = True
                        break
                    values = {}
                    for prop in baseline.EXPECTED:
                        result = run(['/usr/bin/adb', '-s', SERIAL, 'shell', 'getprop', prop], timeout=4)
                        values[prop] = result['stdout'].strip() if result['ok'] else None
                    report['after_properties'] = values
                    wanted_locked = '0' if report['persistence_acknowledged'] else '1'
                    if (values['ro.build.fingerprint'] == baseline.EXPECTED['ro.build.fingerprint']
                            and values['sys.boot_completed'] == '1'
                            and values['ro.boot.flash.locked'] == wanted_locked
                            and (report['persistence_acknowledged'] or values == baseline.EXPECTED)):
                        report['android_return_verified'] = True
                        break
                time.sleep(2)
        if ambiguous:
            report['stop_reason'] = 'Persistent command outcome is uncertain; no retry or automatic reboot was issued'
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
    print(json.dumps(report, indent=2))
    return 0 if report.get('preflight_passed') and report['android_return_verified'] and not report.get('error') else 1


if __name__ == '__main__':
    raise SystemExit(main())
