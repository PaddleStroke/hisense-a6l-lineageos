#!/usr/bin/env python3
"""Install the fixed diagnostic recovery, verify full readback and power off."""
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import pwd
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parent
CAPTURE = ROOT / 'capture-diagnostic-install-v5'
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
    report = {'scope': 'One fixed diagnostic recovery write, full readback and poweroff',
              'started_utc': datetime.now(timezone.utc).isoformat(), 'commands': [],
              'edl_disconnected_after_poweroff': False, 'services_restored': False}
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
        run(['/usr/bin/python3', str(ROOT / 'Inspect-RecoveryImports-v4.py')])
        run(['/usr/bin/python3', str(ROOT / 'Inspect-DiagnosticRecovery-v5.py')], timeout=30)
        report['before'] = properties()
        if report['before'] != EXPECTED:
            raise RuntimeError('Spare differs from its verified post-unlock baseline')
        port = Path('/sys/bus/usb/devices/3-2')
        if (port / 'serial').read_text().strip() != SERIAL:
            raise RuntimeError('Spare is not on the established physical port')
        battery = run(['/usr/bin/adb', '-s', SERIAL, 'shell', 'dumpsys', 'battery'])['stdout']
        import re
        level = re.search(r'^\s*level:\s*(\d+)\s*$', battery, re.M)
        if not level or int(level.group(1)) < 40:
            raise RuntimeError('Battery is not confirmed at least 40 percent')
        print('Enter the laptop password for the diagnostic RECOVERY installation, full readback and poweroff.', flush=True)
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
        print('Services paused. Verifying spare/GPT/stock/empty boot message, installing diagnostic recovery, reading back, then powering off.', flush=True)
        boot_requested = True
        run(['/usr/bin/adb', '-s', SERIAL, 'reboot', 'edl'])
        deadline = time.monotonic() + 30
        while not usb('05c6', '9008') and time.monotonic() < deadline:
            time.sleep(0.25)
        if usb('05c6', '9008') != ['3-2']:
            raise RuntimeError('Expected EDL identity did not appear on the established port')
        with (CAPTURE / 'worker.log').open('x') as log:
            result = subprocess.run(['/usr/bin/sudo', '-n', '/usr/bin/timeout', '--signal=TERM', '--kill-after=3s', '130s',
                                     '/usr/bin/python3', str(ROOT / 'Write-LaptopDiagnosticRecovery-v5.py'), '--mode', 'install-diagnostic',
                                     '--output', str(CAPTURE / 'edl')], stdout=log, stderr=subprocess.STDOUT, timeout=140)
        report['worker_exit'] = result.returncode
        save()
        if result.returncode:
            raise RuntimeError('Diagnostic install stopped; keep USB connected and do not restart until the write state is reviewed')
        print('Diagnostic installation and full readback passed. Poweroff acknowledged; waiting for EDL to disappear.', flush=True)
        exit_code = 0
    except (Exception, KeyboardInterrupt) as error:
        report['error'] = str(error)
        print(str(error), flush=True)
    finally:
        if boot_requested:
            deadline = time.monotonic() + 30
            gone_since = None
            while time.monotonic() < deadline:
                if not usb('05c6', '9008'):
                    gone_since = gone_since or time.monotonic()
                    if time.monotonic() - gone_since >= 3:
                        report['edl_disconnected_after_poweroff'] = exit_code == 0
                        break
                else:
                    gone_since = None
                save()
                time.sleep(.25)
            port = Path('/sys/bus/usb/devices/3-2')
            report['usb_after'] = {}
            for name in ['idVendor', 'idProduct', 'serial']:
                try:
                    report['usb_after'][name] = (port / name).read_text().strip()
                except OSError:
                    pass
        restored = True
        for unit, active in reversed(masked):
            try:
                if usb('05c6', '9008') or usb('18d1', 'd00d'):
                    raise RuntimeError('EDL/fastboot still present; leave probing services paused pending review')
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
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
    print(json.dumps(report, indent=2))
    return 0 if exit_code == 0 and report['edl_disconnected_after_poweroff'] and report['services_restored'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
