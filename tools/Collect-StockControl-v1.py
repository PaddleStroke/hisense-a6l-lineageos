#!/usr/bin/env python3
"""Record physical USB transitions for the stock UI control, without USB I/O."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--seconds', type=int, default=240, choices=range(10, 601))
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
report = {'scope': 'Read sysfs USB identities only; stock recovery UI requires user observation',
          'started_utc': datetime.now(timezone.utc).isoformat(), 'usb_observations': []}
deadline = time.monotonic() + args.seconds
last, fastboot_seen, android_seen = None, False, False
try:
    while time.monotonic() < deadline:
        state = {}
        for name in ['idVendor', 'idProduct', 'serial']:
            try:
                state[name] = (Path('/sys/bus/usb/devices/3-2') / name).read_text().strip()
            except OSError:
                pass
        if state != last:
            report['usb_observations'].append({'utc': datetime.now(timezone.utc).isoformat(), 'state': state})
            temporary = args.output / 'report.json.tmp'
            temporary.write_text(json.dumps(report, indent=2) + '\n')
            temporary.replace(args.output / 'report.json')
            last = state
        if state.get('serial') == '1e529013':
            fastboot_seen |= state.get('idVendor') == '18d1' and state.get('idProduct') == 'd00d'
            if fastboot_seen and state.get('idVendor') == '109b' and state.get('idProduct') in ('911f', '9130'):
                android_seen = True
                break
        time.sleep(.25)
finally:
    report.update(fastboot_seen=fastboot_seen, android_usb_seen=android_seen,
                  recovery_ui_boot_verified=False,
                  finished_utc=datetime.now(timezone.utc).isoformat())
    temporary = args.output / 'report.json.tmp'
    temporary.write_text(json.dumps(report, indent=2) + '\n')
    temporary.replace(args.output / 'report.json')
print(json.dumps(report, indent=2))
raise SystemExit(0 if android_seen else 1)
