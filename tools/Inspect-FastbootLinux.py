#!/usr/bin/env python3
"""Inspect an already-connected spare bootloader and reboot; never write images."""
import argparse
import json
from pathlib import Path
import subprocess


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--serial', required=True, choices=['1e529013'])
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
report = {'scope': 'Read-only getvar queries, followed by reboot to stock',
          'serial': args.serial, 'commands': []}


def save():
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


def run(arguments):
    try:
        result = subprocess.run(['/usr/bin/fastboot', *arguments],
                                capture_output=True, text=True, timeout=10)
        entry = {'arguments': arguments, 'exit': result.returncode,
                 'command_succeeded': result.returncode == 0 and 'FAILED' not in result.stderr,
                 'stdout': result.stdout, 'stderr': result.stderr}
    except subprocess.TimeoutExpired as exc:
        entry = {'arguments': arguments, 'exit': None, 'command_succeeded': False, 'timeout': 10,
                 'stdout': (exc.stdout or b'').decode(errors='replace'),
                 'stderr': (exc.stderr or b'').decode(errors='replace')}
    report['commands'].append(entry)
    save()
    return entry


devices = run(['devices'])
found = devices['exit'] == 0 and any(
    line.split() and line.split()[0] == args.serial
    for line in devices['stdout'].splitlines())
report['fastboot_detected'] = found
save()
if not found:
    raise SystemExit('Expected spare serial is not visible; no device command sent')

try:
    for variable in ['product', 'version-bootloader', 'unlocked', 'secure',
                     'is-userspace', 'max-download-size', 'partition-size:boot',
                     'partition-size:recovery', 'partition-size:system',
                     'partition-size:vendor', 'partition-size:dtbo',
                     'partition-type:system', 'has-slot:boot', 'has-slot:system']:
        entry = run(['-s', args.serial, 'getvar', variable])
        if entry['exit'] is None:
            report['queries_stopped'] = 'Transport timeout'
            break
finally:
    report['reboot'] = run(['-s', args.serial, 'reboot'])
    save()
print(json.dumps(report, indent=2))
raise SystemExit(0 if report['reboot']['command_succeeded'] else 1)
