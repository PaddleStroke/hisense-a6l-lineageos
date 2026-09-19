#!/usr/bin/env python3
"""Short, authenticated readiness gate for graphics tests on existing V38.

Does not repeat the storage benchmark or open the serial port. No phone writes.
Retain Collect-ProbeSerial-v38.py for full startup regression captures.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import time

SERIAL = 'HLTE730T-PROBE'
EXPECTED = ['v38', '7.2.3-a6l-probe+', '1', '1', '1']
CHECK = ('getprop ro.a6l.ramdiag; uname -r; getprop ro.adb.secure; '
         'getprop ro.secure; getprop ro.debuggable; id; cat /proc/mounts')
VIRTUAL = {'rootfs', 'tmpfs', 'devpts', 'proc', 'sysfs', 'selinuxfs',
           'configfs', 'functionfs', 'debugfs'}


def valid_response(exit_code, output):
    lines = output.splitlines()
    return (exit_code == 0 and len(lines) > 6
            and lines[:5] == EXPECTED and 'uid=0(root)' in lines[5]
            and all(len(line.split()) >= 3 and line.split()[2] in VIRTUAL
                    for line in lines[6:])
            and any('tmpfs /tmp tmpfs ' in line for line in lines[6:]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--menu-seconds', type=int, default=1800)
    args = parser.parse_args()
    assert 10 <= args.menu_seconds <= 3600
    args.output.mkdir(mode=0o700, parents=True, exist_ok=False)
    report = {'scope': 'Authenticated V38 readiness only; no storage benchmark',
              'started_utc': datetime.now(timezone.utc).isoformat(),
              'observations': [], 'passed': False}
    def save():
        temporary = args.output / 'report.json.tmp'
        temporary.write_text(json.dumps(report, indent=2) + '\n')
        temporary.replace(args.output / 'report.json')
    save()
    deadline = time.monotonic() + args.menu_seconds
    departed = False
    saw_fastboot = False
    last = None
    stable_since = None
    try:
        while time.monotonic() < deadline:
            usb = {}
            for name in ('idVendor', 'idProduct', 'serial'):
                try:
                    usb[name] = (Path('/sys/bus/usb/devices/3-2') / name).read_text().strip()
                except OSError:
                    pass
            if usb != last:
                report['observations'].append({'usb': usb, 'utc': datetime.now(timezone.utc).isoformat()})
                last = usb
                save()
            fastboot = usb == {'idVendor': '18d1', 'idProduct': 'd00d', 'serial': '1e529013'}
            saw_fastboot |= fastboot
            if saw_fastboot and not fastboot and not departed:
                departed = True
                deadline = time.monotonic() + 90  # Failure ceiling, not a fixed wait.
            if usb == {'idVendor': '1d6b', 'idProduct': '0104', 'serial': SERIAL}:
                if not departed:
                    departed = True
                    deadline = time.monotonic() + 90
                try:
                    result = subprocess.run(['/usr/bin/adb', '-s', SERIAL, 'shell', CHECK],
                                            capture_output=True, text=True, timeout=6)
                except subprocess.TimeoutExpired:
                    stable_since = None
                    report['adb_timeouts'] = report.get('adb_timeouts', 0) + 1
                    time.sleep(1)
                    continue
                valid = valid_response(result.returncode, result.stdout)
                report['last_check'] = {'exit': result.returncode, 'stdout': result.stdout,
                                        'stderr': result.stderr, 'valid': valid}
                save()
                if valid:
                    if stable_since is None:
                        stable_since = time.monotonic()
                    elif time.monotonic() - stable_since >= 4:
                        report['passed'] = True
                        break
                else:
                    stable_since = None
            else:
                stable_since = None
            time.sleep(1)
        report['stop_reason'] = 'authenticated_ready' if report['passed'] else 'deadline'
    except Exception as error:
        report['error'] = repr(error)
    finally:
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
