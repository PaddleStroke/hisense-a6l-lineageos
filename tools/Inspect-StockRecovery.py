#!/usr/bin/env python3
"""Enter existing recovery or verify return; never download or flash an image."""
import argparse
import json
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent
ADB = ROOT / 'tools/platform-tools/adb.exe'
SERIAL = '1e529013'
FINGERPRINT = 'Hisense/HLTE730T/HLTE730T:9/PKQ1.190723.001/L1632.6.01.04:user/release-keys'
PROPERTIES = ['ro.build.fingerprint', 'sys.boot_completed',
              'ro.boot.flash.locked', 'ro.boot.verifiedbootstate']

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('action', choices=['enter', 'verify-return'])
parser.add_argument('output', type=Path)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
target = args.output / (args.action + '.json')
if target.exists():
    raise SystemExit('Refusing to overwrite an existing observation')
report = {'action': args.action, 'serial': SERIAL, 'commands': [],
          'scope': 'Existing recovery reboot and read-only observations; no image installation'}


def run(arguments, *, selected=True):
    command = [str(ADB), *(['-s', SERIAL] if selected else []), *arguments]
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=8)
        entry = {'arguments': arguments, 'exit': result.returncode,
                 'stdout': result.stdout, 'stderr': result.stderr}
    except subprocess.TimeoutExpired:
        entry = {'arguments': arguments, 'exit': None, 'timeout_seconds': 8}
    report['commands'].append(entry)
    target.write_text(json.dumps(report, indent=2) + '\n')
    return entry


def properties():
    found = {}
    for prop in PROPERTIES:
        entry = run(['shell', 'getprop', prop])
        if entry['exit'] != 0:
            raise RuntimeError('Cannot read stock property: ' + prop)
        found[prop] = entry['stdout'].strip()
    return found


try:
    values = properties()
    report['properties'] = values
    valid = (values['ro.build.fingerprint'] == FINGERPRINT
             and values['sys.boot_completed'] == '1'
             and values['ro.boot.flash.locked'] == '1'
             and values['ro.boot.verifiedbootstate'] == 'green')
    if not valid:
        raise RuntimeError('Stock identity, completed-boot or locked/green guard failed')
    if args.action == 'enter':
        result = run(['reboot', 'recovery'])
        if result['exit'] != 0:
            raise RuntimeError('Recovery reboot request failed')
        for delay in [3, 5, 7, 10]:
            time.sleep(delay)
            run(['devices', '-l'], selected=False)
        report['physical_menu_confirmation_required'] = True
    else:
        report['stock_return_verified'] = True
finally:
    target.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
