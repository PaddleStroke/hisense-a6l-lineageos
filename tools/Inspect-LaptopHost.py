#!/usr/bin/env python3
"""Read-only laptop and spare-ADB readiness report; no phone reboot."""
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import pwd
import shutil
import socket
import subprocess

SERIAL = '1e529013'
assert pwd.getpwuid(os.getuid()).pw_name == 'pierrelouis', 'Unexpected laptop user'
assert socket.gethostname().split('.')[0] == 'system76-pc', 'Unexpected laptop hostname'
report = {'scope': 'Laptop and spare readiness only; no reboot or phone write',
          'utc': datetime.now(timezone.utc).isoformat(), 'hostname': socket.gethostname(),
          'kernel': platform.release(), 'architecture': platform.machine(),
          'os_release': Path('/etc/os-release').read_text(), 'commands': [], 'spare_usb': []}


def run(argv, timeout=10):
    try:
        result = subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=timeout)
        entry = {'argv': argv, 'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}
    except (OSError, subprocess.TimeoutExpired) as exc:
        entry = {'argv': argv, 'exit': None, 'error': str(exc)}
    report['commands'].append(entry)
    return entry


search_path = os.environ.get('PATH', os.defpath) + ':/usr/sbin:/sbin'
report['tools'] = {name: shutil.which(name, path=search_path) for name in
                   ['adb', 'fastboot', 'python3', 'sudo', 'modprobe', 'mount', 'mountpoint']}
run(['dpkg-query', '-W', '-f=${Package} ${Version} ${db:Status-Status}\n',
     'adb', 'fastboot', 'python3', 'openssh-server'])
run(['dpkg', '--audit'])
run(['systemctl', 'is-active', 'ssh'])
run(['id'])
run(['sudo', '-n', 'true'])
for device in Path('/sys/bus/usb/devices').glob('*'):
    try:
        if (device / 'serial').read_text().strip().lower() != SERIAL:
            continue
        report['spare_usb'].append({'path': str(device),
                                    **{p: (device / p).read_text().strip() for p in
                                       ['idVendor', 'idProduct', 'serial', 'busnum', 'devnum']}})
    except (OSError, UnicodeError):
        continue
if report['tools']['fastboot']:
    run([report['tools']['fastboot'], '--version'])
if report['tools']['adb']:
    adb = report['tools']['adb']
    run([adb, 'version'])
    state = run([adb, '-s', SERIAL, 'get-state'])
    if state['exit'] == 0 and state.get('stdout', '').strip() == 'device':
        report['properties'] = {}
        for prop in ['ro.build.fingerprint', 'sys.boot_completed', 'ro.boot.flash.locked',
                     'ro.boot.verifiedbootstate']:
            entry = run([adb, '-s', SERIAL, 'shell', 'getprop', prop])
            report['properties'][prop] = entry.get('stdout', '').strip() if entry['exit'] == 0 else None
print(json.dumps(report, indent=2))
