#!/usr/bin/env python3
"""Observe physical A6L fastboot entry, query state, and reboot. No image writes."""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parent
SERIAL = '1e529013'
EXPECTED = {
    'ro.build.fingerprint': 'Hisense/HLTE730T/HLTE730T:9/PKQ1.190723.001/L1632.6.01.04:user/release-keys',
    'sys.boot_completed': '1', 'ro.boot.flash.locked': '0', 'ro.boot.verifiedbootstate': 'orange',
}
GOOGLE_HASH = 'a686e2c7e8dc9cf4cba0cb8a2eef05f7b2bd682c925abd032fe203215d80b618'
ALLOWED = (('getvar', 'product', 'getvar', 'unlocked', 'getvar', 'secure'),
           ('oem', 'device-info'), ('reboot',))


def port_state():
    port = Path('/sys/bus/usb/devices/3-2')
    state = {}
    for name in ['idVendor', 'idProduct', 'serial', 'quirks']:
        try:
            state[name] = (port / name).read_text().strip()
        except OSError:
            pass
    return state


def exact_fastboot():
    found = []
    for port in Path('/sys/bus/usb/devices').glob('*'):
        try:
            if ((port / 'idVendor').read_text().strip(), (port / 'idProduct').read_text().strip()) == ('18d1', 'd00d'):
                found.append(port.name)
        except OSError:
            pass
    state = port_state()
    try:
        no_lpm = int(state.get('quirks', '0'), 0) & 0x400 != 0
    except ValueError:
        no_lpm = False
    return found == ['3-2'] and state.get('serial') == SERIAL and no_lpm


def checked_fastboot_argv(binary, arguments):
    if tuple(arguments) not in ALLOWED:
        raise ValueError('Only the fixed queries and normal reboot are permitted')
    return ['/usr/bin/sudo', '-n', str(binary), '-s', SERIAL, *arguments]


def main():
    assert os.geteuid() != 0 and pwd.getpwuid(os.getuid()).pw_name == 'pierrelouis'
    assert socket.gethostname().split('.')[0] == 'system76-pc'
    capture = ROOT / 'capture-button-entry-v2'
    capture.mkdir(mode=0o700, exist_ok=False)
    report = {'scope': 'Physical button entry; fixed fastboot queries and normal reboot only',
              'started_utc': datetime.now(timezone.utc).isoformat(), 'commands': [],
              'phase': 'preflight', 'usb_transitions': [], 'physical_entry_observed': False,
              'queries_verified': False, 'android_return_verified': False,
              'service_restored': False, 'quirks_restored': False}
    binary = ROOT / 'google-fastboot-37.0.1-linux/fastboot'
    mask = Path('/run/systemd/system/fwupd.service')
    quirk_path = Path('/sys/module/usbcore/parameters/quirks')
    masked = quirk_attempted = False
    previous = during = None
    active = False
    armed = False

    def save():
        (capture / 'session.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(argv, required=True, timeout=12):
        entry = {'argv': argv, 'started_utc': datetime.now(timezone.utc).isoformat()}
        report['commands'].append(entry)
        save()
        try:
            result = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
            entry.update(exit=result.returncode, stdout=result.stdout, stderr=result.stderr)
        except subprocess.TimeoutExpired as error:
            entry.update(exit=None, stdout=(error.stdout or b'').decode(errors='replace'),
                         stderr=(error.stderr or b'').decode(errors='replace'), timed_out=True)
        save()
        if required and entry['exit'] != 0:
            raise RuntimeError('Command failed: ' + ' '.join(argv))
        return entry

    def properties():
        values = {}
        for name in EXPECTED:
            r = run(['/usr/bin/adb', '-s', SERIAL, 'shell', 'getprop', name], required=False, timeout=3)
            values[name] = r['stdout'].strip() if r['exit'] == 0 else None
        return values

    def replace_quirks(old, new):
        code = ('import pathlib,sys; p=pathlib.Path("/sys/module/usbcore/parameters/quirks"); '
                'assert p.read_text().strip()==sys.argv[1], "Quirks changed externally"; '
                'p.write_text(sys.argv[2]+"\\n"); assert p.read_text().strip()==sys.argv[2]')
        run(['/usr/bin/sudo', '-n', '/usr/bin/python3', '-c', code, old, new])

    def phone(arguments):
        argv = checked_fastboot_argv(binary, arguments)
        if not exact_fastboot():
            raise RuntimeError('Exact spare bootloader with NO_LPM is absent')
        return run(argv, timeout=15)

    def observe_usb():
        current = port_state()
        if not report['usb_transitions'] or report['usb_transitions'][-1]['state'] != current:
            report['usb_transitions'].append({'utc': datetime.now(timezone.utc).isoformat(), 'state': current})
            save()
        return current

    try:
        if hashlib.sha256(binary.read_bytes()).hexdigest() != GOOGLE_HASH:
            raise ValueError('Official fastboot binary differs')
        report['before'] = properties()
        if report['before'] != EXPECTED or port_state().get('serial') != SERIAL:
            raise RuntimeError('Spare does not match its unlocked Android baseline')
        battery = run(['/usr/bin/adb', '-s', SERIAL, 'shell', 'dumpsys', 'battery'])['stdout']
        level = re.search(r'^\s*level:\s*(\d+)\s*$', battery, re.M)
        if not level or int(level.group(1)) < 40:
            raise RuntimeError('Battery must be at least 40 percent')
        report['battery_percent'] = int(level.group(1))
        if mask.exists() or mask.is_symlink():
            raise RuntimeError('Existing service override; stop before host changes')
        previous = quirk_path.read_text().strip()
        entries = previous.split(',') if previous else []
        if any(e.lower().startswith('18d1:d00d:') for e in entries):
            raise RuntimeError('Existing bootloader quirk; stop before host changes')
        during = ','.join(entries + ['18d1:d00d:k'])
        report['quirks'] = {'before': previous, 'during': during}
        report['phase'] = 'waiting_for_laptop_password'
        save()
        print('Enter the LAPTOP password. This session does not flash or erase anything.', flush=True)
        if subprocess.run(['/usr/bin/sudo', '-v']).returncode:
            raise RuntimeError('Sudo authorization failed')
        state = run(['/usr/bin/systemctl', 'show', 'fwupd.service', '-p', 'LoadState', '-p', 'ActiveState'])['stdout']
        if 'LoadState=loaded' not in state:
            raise RuntimeError('Unexpected fwupd state')
        active = 'ActiveState=active' in state
        report['fwupd_was_active'] = active
        if active:
            status = run(['/usr/bin/busctl', '--system', 'get-property', 'org.freedesktop.fwupd', '/', 'org.freedesktop.fwupd', 'Status'])
            if status['stdout'].strip() != 'u 1':
                raise RuntimeError('fwupd is busy')
        masked = True
        run(['/usr/bin/sudo', '-n', '/usr/bin/systemctl', 'mask', '--runtime', '--now', 'fwupd.service'])
        state = run(['/usr/bin/systemctl', 'show', 'fwupd.service', '-p', 'LoadState', '-p', 'ActiveState'])['stdout']
        if (not mask.is_symlink() or os.readlink(mask) != '/dev/null'
                or 'LoadState=masked' not in state or 'ActiveState=inactive' not in state):
            raise RuntimeError('fwupd pause was not verified')
        quirk_attempted = True
        replace_quirks(previous, during)
        report['phase'] = 'ready_for_manual_button_entry'
        report['ready_utc'] = datetime.now(timezone.utc).isoformat()
        save()
        print('READY: unplug the phone; shut it down using the Android Power menu.\n'
              'Once off, hold ONLY Volume Up and reconnect the USB cable to the same laptop port.\n'
              'Release Volume Up when the bootloader screen appears. Do not press Power.\n'
              'The collector will query state and reboot automatically. Waiting up to four minutes.', flush=True)
        armed = True
        deadline = time.monotonic() + 240
        absent_since = None
        saw_disconnect = False
        while time.monotonic() < deadline:
            current = observe_usb()
            if not current:
                absent_since = absent_since or time.monotonic()
                saw_disconnect |= time.monotonic() - absent_since >= 2
            else:
                absent_since = None
            if exact_fastboot():
                if not saw_disconnect:
                    raise RuntimeError('Bootloader appeared without the required USB disconnect')
                report['physical_entry_observed'] = True
                report['phase'] = 'querying_bootloader'
                save()
                break
            time.sleep(0.25)
        else:
            raise RuntimeError('Physical bootloader entry was not observed within four minutes')
        query = phone(['getvar', 'product', 'getvar', 'unlocked', 'getvar', 'secure'])
        combined = query['stdout'] + query['stderr']
        if any(s not in combined for s in ['product: sdm660', 'unlocked: yes', 'secure: yes']):
            raise RuntimeError('Bootloader identity or persisted unlock differs')
        info = phone(['oem', 'device-info'])
        combined = info['stdout'] + info['stderr']
        if any(s not in combined for s in ['Device unlocked: true', 'Device critical unlocked: true']):
            raise RuntimeError('Both persisted unlock flags were not confirmed')
        report['queries_verified'] = True
        save()
    except (Exception, KeyboardInterrupt) as error:
        report['error'] = str(error)
        print(str(error), flush=True)
    finally:
        if armed and exact_fastboot():
            try:
                report['phase'] = 'rebooting_from_bootloader'
                save()
                report['reboot'] = phone(['reboot'])
            except Exception as error:
                report['reboot_error'] = str(error)
        if armed:
            report['phase'] = 'waiting_for_android'
            save()
            print('Waiting up to two minutes for Android. If no bootloader appeared, power on and reconnect the spare.', flush=True)
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline:
                state = observe_usb()
                if state.get('idVendor') == '109b' and state.get('serial') == SERIAL:
                    report['after'] = properties()
                    if report['after'] == EXPECTED:
                        report['android_return_verified'] = True
                        break
                time.sleep(2)
        if quirk_attempted:
            try:
                current = quirk_path.read_text().strip()
                if current == during:
                    replace_quirks(during, previous)
                elif current != previous:
                    raise RuntimeError('Quirks changed externally; do not overwrite')
                report['quirks_restored'] = quirk_path.read_text().strip() == previous
            except Exception as error:
                report['quirk_restore_error'] = str(error)
        if masked:
            try:
                current = port_state()
                if (current.get('idVendor'), current.get('idProduct')) in [('18d1', 'd00d'), ('05c6', '9008')]:
                    raise RuntimeError('Phone remains in a sensitive USB mode; leave fwupd paused pending restart')
                if not mask.is_symlink() or os.readlink(mask) != '/dev/null':
                    raise RuntimeError('Service override changed externally')
                run(['/usr/bin/sudo', '-n', '/usr/bin/systemctl', 'unmask', '--runtime', 'fwupd.service'])
                if active:
                    run(['/usr/bin/sudo', '-n', '/usr/bin/systemctl', 'start', 'fwupd.service'])
                state = run(['/usr/bin/systemctl', 'show', 'fwupd.service', '-p', 'LoadState', '-p', 'ActiveState'])['stdout']
                report['service_restored'] = ('LoadState=loaded' in state
                                              and ('ActiveState=active' if active else 'ActiveState=inactive') in state)
            except Exception as error:
                report['service_restore_error'] = str(error)
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        report['phase'] = 'finished'
        save()
    print(json.dumps({k: v for k, v in report.items() if k not in ('commands', 'usb_transitions')}, indent=2))
    return 0 if all(report[k] for k in ('physical_entry_observed', 'queries_verified', 'android_return_verified',
                                      'quirks_restored', 'service_restored')) and not report.get('error') else 1


if __name__ == '__main__':
    raise SystemExit(main())
