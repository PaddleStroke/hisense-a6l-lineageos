#!/usr/bin/env python3
"""Query the spare's bootloader and reboot to Android; no unlock/download/flash."""
import argparse
import json
from pathlib import Path
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('platform_tools', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--serial', required=True)
a = p.parse_args()
adb = str((a.platform_tools / 'adb.exe').resolve())
fastboot = str((a.platform_tools / 'fastboot.exe').resolve())
report = {'scope': 'Read-only fastboot queries and reboot; no unlock, download, erase or flash', 'commands': []}


def run(executable, args, timeout=10):
    command = [executable, '-s', a.serial, *args]
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        entry = {'arguments': args, 'tool': Path(executable).name, 'exit': result.returncode,
                 'stdout': result.stdout, 'stderr': result.stderr}
    except subprocess.TimeoutExpired:
        entry = {'arguments': args, 'tool': Path(executable).name, 'timeout': timeout, 'exit': None}
    report['commands'].append(entry)
    return entry


fingerprint = run(adb, ['shell', 'getprop', 'ro.build.fingerprint'])
expected = 'Hisense/HLTE730T/HLTE730T:9/PKQ1.190723.001/L1632.6.01.04:user/release-keys'
if fingerprint['exit'] != 0 or fingerprint['stdout'].strip() != expected:
    raise SystemExit('Spare stock fingerprint guard failed')
a.output.mkdir(parents=True, exist_ok=False)
run(adb, ['reboot', 'bootloader'])
found = False
try:
    for _ in range(30):
        result = subprocess.run([fastboot, 'devices'], capture_output=True, text=True, timeout=5)
        if any(line.split() and line.split()[0] == a.serial for line in result.stdout.splitlines()):
            found = True
            break
        time.sleep(0.5)
    report['fastboot_detected'] = found
    if found:
        for variable in ['product', 'unlocked', 'secure', 'is-userspace', 'max-download-size',
                         'partition-size:boot', 'partition-size:recovery', 'has-slot:boot']:
            run(fastboot, ['getvar', variable])
finally:
    if found:
        run(fastboot, ['reboot'])
    (a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
raise SystemExit(0 if found else 1)
