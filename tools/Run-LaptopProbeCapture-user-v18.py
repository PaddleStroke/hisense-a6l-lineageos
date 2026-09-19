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
CAPTURE = ROOT / 'capture-probe-serial-user-v18'
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
    host_pause_requested = False
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
        inhibition = run(['/usr/bin/gnome-session-inhibit', '--list'])['stdout']
        if not all(word in inhibition for word in ['A6L-v18', 'suspend', 'idle']):
            raise RuntimeError('V18 requires an active desktop idle/suspend inhibitor')
        install = json.loads((ROOT / 'capture-diagnostic-install-user-v18/edl/report.json').read_text())
        if not (install.get('mode') == 'install-diagnostic' and install.get('readback_verified')
                and install.get('power_acknowledged') and install.get('power_action') == 'off'
                and not install.get('error') and install.get('target_sha256') == 'a983ef25bdca4a06dae88ab74a4db8582b0a4507f01e14ad6bb2a9354c123ee6'):
            raise RuntimeError('Verified diagnostic installation/poweroff evidence is required')
        report['before'] = properties()
        if report['before'] != EXPECTED:
            raise RuntimeError('Expected the verified original Android baseline')
        if (Path('/sys/bus/usb/devices/3-2') / 'serial').read_text().strip() != SERIAL:
            raise RuntimeError('Spare is not on the established port')
        host_state = json.loads(run(['/usr/bin/sudo', '-n', '/usr/local/sbin/a6l-host-control', 'status'])['stdout'])
        if host_state['owned_pause'] is not None:
            raise RuntimeError('An earlier host pause needs inspection/cleanup')
        host_pause_requested = True
        run(['/usr/bin/sudo', '-n', '/usr/local/sbin/a6l-host-control', 'pause'])
        report['phase'] = 'ready_for_physical_boot'
        save()
        print('Restarting the spare into fastboot. Wait for the menu instructions in Codex.', flush=True)
        boot_requested = True
        run(['/usr/bin/adb', '-s', SERIAL, 'reboot', 'bootloader'])
        with (CAPTURE / 'worker.log').open('x') as log:
            result = subprocess.run(['/usr/bin/timeout', '--signal=TERM', '--kill-after=3s', '340s',
                                     '/usr/bin/python3', str(ROOT / 'Collect-ProbeSerial-v18.py'),
                                     str(CAPTURE / 'serial'), '--seconds', '330'],
                                    stdout=log, stderr=subprocess.STDOUT, timeout=350)
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
        restored = not host_pause_requested
        if host_pause_requested:
            try:
                run(['/usr/bin/sudo', '-n', '/usr/local/sbin/a6l-host-control', 'resume'])
                host_state = json.loads(run(['/usr/bin/sudo', '-n', '/usr/local/sbin/a6l-host-control', 'status'])['stdout'])
                if host_state['owned_pause'] is not None:
                    raise RuntimeError('Host pause remains after cleanup')
                restored = True
            except Exception as error:
                report.setdefault('cleanup_errors', []).append(str(error))
        report['services_restored'] = restored
        report['phase'] = 'finished'
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
    print(json.dumps(report, indent=2))
    return 0 if exit_code == 0 and report['android_return_verified'] and report['services_restored'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
