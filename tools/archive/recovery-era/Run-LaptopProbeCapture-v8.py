#!/usr/bin/env python3
"""Capture a physical diagnostic boot and wait for manual return to original Android."""
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import pwd
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parent
CAPTURE = ROOT / 'capture-probe-serial-v8'
SERIAL = '1e529013'
EXPECTED = {'ro.build.fingerprint': 'Hisense/HLTE730T/HLTE730T:9/PKQ1.190723.001/L1632.6.01.04:user/release-keys',
            'sys.boot_completed': '1', 'ro.boot.flash.locked': '0', 'ro.boot.verifiedbootstate': 'orange'}


def usb(vid, pid):
    found = []
    for item in Path('/sys/bus/usb/devices').glob('*'):
        try:
            if (item / 'idVendor').read_text().strip() == vid and (item / 'idProduct').read_text().strip() == pid:
                found.append(item.name)
        except OSError:
            pass
    return found


def main():
    assert os.geteuid() != 0 and pwd.getpwuid(os.getuid()).pw_name == 'pierrelouis'
    assert socket.gethostname().split('.')[0] == 'system76-pc'
    CAPTURE.mkdir(mode=0o700, exist_ok=False)
    report = {'scope': 'One Android-to-bootloader restart and USB capture; no image writes',
              'started_utc': datetime.now(timezone.utc).isoformat(), 'commands': [],
              'android_return_verified': False, 'services_restored': False}
    masked = []
    boot_requested = False
    exit_code = 1

    def save():
        temporary = CAPTURE / 'session.json.tmp'
        temporary.write_text(json.dumps(report, indent=2) + '\n')
        temporary.replace(CAPTURE / 'session.json')

    def run(argv, required=True, timeout=10):
        entry = {'argv': argv}
        report['commands'].append(entry)
        save()
        try:
            result = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
            entry.update(exit=result.returncode, stdout=result.stdout, stderr=result.stderr)
        except subprocess.TimeoutExpired:
            entry.update(exit=None, stdout='', stderr='Command timed out')
        save()
        if required and entry['exit'] != 0:
            raise RuntimeError('Host/ADB command failed: ' + ' '.join(argv))
        return entry

    def properties():
        values = {}
        for prop in EXPECTED:
            r = run(['/usr/bin/adb', '-s', SERIAL, 'shell', 'getprop', prop], required=False, timeout=4)
            values[prop] = r['stdout'].strip() if r['exit'] == 0 else None
        return values

    try:
        install = json.loads((ROOT / 'capture-diagnostic-install-v8/edl/report.json').read_text())
        if not (install.get('mode') == 'install-diagnostic' and install.get('readback_verified')
                and install.get('power_acknowledged') and install.get('power_action') == 'off'
                and not install.get('error') and install.get('target_sha256') == '5abb7746867573e4afd1243713eb05e47d40b3d0d261ddee08c57b95b789486f'):
            raise RuntimeError('Verified diagnostic installation/poweroff evidence is required')
        report['before'] = properties()
        if report['before'] != EXPECTED:
            raise RuntimeError('Expected the verified original Android baseline')
        if (Path('/sys/bus/usb/devices/3-2') / 'serial').read_text().strip() != SERIAL:
            raise RuntimeError('Spare is not on the established port')
        print('Enter the laptop password for read-only diagnostic USB log capture.', flush=True)
        if subprocess.run(['/usr/bin/sudo', '-v']).returncode:
            raise RuntimeError('Sudo authorization failed before changing phone mode')
        for unit in ['fwupd.service', 'ModemManager.service']:
            state = run(['/usr/bin/systemctl', 'show', unit, '-p', 'LoadState', '-p', 'ActiveState'])['stdout']
            override = Path('/run/systemd/system') / unit
            if 'LoadState=loaded' not in state:
                raise RuntimeError('Unexpected service state: ' + unit)
            active = 'ActiveState=active' in state
            if active and unit == 'fwupd.service':
                r = run(['/usr/bin/busctl', '--system', 'get-property', 'org.freedesktop.fwupd', '/', 'org.freedesktop.fwupd', 'Status'])
                if r['stdout'].strip() != 'u 1':
                    raise RuntimeError('fwupd is busy')
            if active and unit == 'ModemManager.service':
                r = run(['/usr/bin/mmcli', '-L'])
                if 'No modems were found' not in r['stdout'] + r['stderr']:
                    raise RuntimeError('Do not interrupt an existing modem')
            if override.exists() or override.is_symlink():
                raise RuntimeError('A service override already exists: ' + unit)
            masked.append((unit, active))
            run(['/usr/bin/sudo', '-n', '/usr/bin/systemctl', 'mask', '--runtime', '--now', unit])
        report['phase'] = 'ready_for_physical_boot'
        save()
        print('Restarting the spare into fastboot. Wait for the menu instructions in Codex.', flush=True)
        boot_requested = True
        run(['/usr/bin/adb', '-s', SERIAL, 'reboot', 'bootloader'])
        with (CAPTURE / 'worker.log').open('x') as log:
            result = subprocess.run(['/usr/bin/sudo', '-n', '/usr/bin/timeout', '--signal=TERM', '--kill-after=3s', '250s',
                                     '/usr/bin/python3', str(ROOT / 'Collect-ProbeSerial-v1.py'),
                                     str(CAPTURE / 'serial'), '--seconds', '240'],
                                    stdout=log, stderr=subprocess.STDOUT, timeout=260)
        report['worker_exit'] = result.returncode
        report['phase'] = 'waiting_for_manual_android_return'
        save()
        print('Capture finished. Use only Power for a forced normal restart when instructed; leave USB connected.', flush=True)
        exit_code = result.returncode
    except (Exception, KeyboardInterrupt) as error:
        report['error'] = str(error)
        print(str(error), flush=True)
    finally:
        if boot_requested:
            deadline = time.monotonic() + 240
            while time.monotonic() < deadline:
                if not usb('05c6', '9008'):
                    values = properties()
                    report['after'] = values
                    if values == EXPECTED:
                        report['android_return_verified'] = True
                        break
                elif not report.get('manual_reboot_notice') and report.get('error'):
                    report['manual_reboot_notice'] = True
                    print('EDL appeared during the diagnostic boot; keep USB connected for inspection.', flush=True)
                save()
                time.sleep(2)
        restored = True
        for unit, active in reversed(masked):
            try:
                if usb('05c6', '9008') or usb('18d1', 'd00d') or usb('1d6b', '0104'):
                    raise RuntimeError('Development USB mode still present; leave probing services paused pending normal return')
                override = Path('/run/systemd/system') / unit
                if not override.is_symlink() or os.readlink(override) != '/dev/null':
                    raise RuntimeError('Service override changed externally')
                run(['/usr/bin/sudo', '-n', '/usr/bin/systemctl', 'unmask', '--runtime', unit])
                if active:
                    run(['/usr/bin/sudo', '-n', '/usr/bin/systemctl', 'start', unit])
                state = run(['/usr/bin/systemctl', 'show', unit, '-p', 'LoadState', '-p', 'ActiveState'])['stdout']
                if 'LoadState=loaded' not in state or ('ActiveState=active' if active else 'ActiveState=inactive') not in state:
                    raise RuntimeError('Service restoration verification failed')
            except Exception as error:
                restored = False
                report.setdefault('cleanup_errors', []).append(str(error))
        report['services_restored'] = restored
        report['phase'] = 'finished'
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
    print(json.dumps(report, indent=2))
    return 0 if exit_code == 0 and report['android_return_verified'] and report['services_restored'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
