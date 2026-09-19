#!/usr/bin/env python3
"""Bounded passive USB setup trace following only the spare's physical port."""
import argparse
from collections import defaultdict
import json
import os
from pathlib import Path
import select
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
args = parser.parse_args()
assert args.output.is_dir()
assert os.geteuid() == 0
port = Path('/sys/bus/usb/devices/3-2')
assert (port / 'serial').read_text().strip().lower() == '1e529013'
assert (port / 'idVendor').read_text().strip() == '109b'
allowed = {int((port / 'devnum').read_text())}


def other_ports():
    found = {}
    for device in Path('/sys/bus/usb/devices').glob('3-*'):
        if device.name == '3-2' or not (device / 'idVendor').exists():
            continue
        try:
            found[device.name] = int((device / 'devnum').read_text())
        except OSError:
            continue
    return found


others = other_ports()
excluded = set(others.values()) | {1}  # other devices and the root hub
monitor = os.open('/sys/kernel/debug/usb/usbmon/3u', os.O_RDONLY | os.O_NONBLOCK)
trace = []
pending = defaultdict(list)
partial = b''
errors = []
device_snapshots = {}
deadline = time.monotonic() + 300
(args.output / 'ready.json').write_text(json.dumps({'port': '3-2', 'bus': 3, 'other_device_addresses_excluded': sorted(excluded)}))
try:
    while time.monotonic() < deadline and not (args.output / 'stop').exists():
        if other_ports() != others:
            raise RuntimeError('Another USB port changed; stopping the setup capture')
        try:
            if (port / 'serial').read_text().strip().lower() == '1e529013':
                address = int((port / 'devnum').read_text())
                allowed.add(address)
                trace.extend(pending.pop(address, []))
                if address not in device_snapshots:
                    snapshot = {}
                    for attribute in ['idVendor', 'idProduct', 'serial', 'speed', 'quirks',
                                      'power/control', 'power/runtime_status',
                                      'power/autosuspend_delay_ms', 'power/usb2_hardware_lpm']:
                        if (port / attribute).exists():
                            snapshot[attribute] = (port / attribute).read_text().strip()
                    device_snapshots[address] = snapshot
        except OSError:
            pass
        if not select.select([monitor], [], [], 0.02)[0]:
            continue
        try:
            chunk = os.read(monitor, 65536)
        except BlockingIOError:
            continue
        if not chunk:
            break
        partial += chunk
        while b'\n' in partial:
            raw, partial = partial.split(b'\n', 1)
            line = raw.decode('ascii', errors='replace')
            parts = line.split()
            pipe = parts[3].split(':') if len(parts) > 3 else []
            if len(pipe) != 4 or int(pipe[1]) != 3:
                continue
            address = int(pipe[2])
            if address in excluded:
                continue
            if address == 0 or address in allowed:
                trace.append(line)
            else:
                # Retain only setup records until cached sysfs identifies the new
                # address as this physical port. Never retain another device's bulk
                # or interrupt payload, such as Bluetooth HID traffic.
                if pipe[0].startswith('C'):
                    pending[address].append(line)
            if len(trace) + sum(map(len, pending.values())) > 40000:
                raise RuntimeError('Capture record limit reached')
except Exception as exc:
    errors.append(str(exc))
finally:
    os.close(monitor)
    trace.sort(key=lambda line: int(line.split()[1]))
    (args.output / 'usbmon-setup.txt').write_text('\n'.join(trace) + '\n')
    report = {'port': '3-2', 'bus': 3, 'allowed_phone_addresses': sorted(allowed),
              'excluded_addresses': sorted(excluded), 'trace_lines': len(trace),
              'device_snapshots': device_snapshots,
              'errors': errors, 'unresolved_setup_records_discarded': sum(map(len, pending.values())),
              'address_zero_note': 'Enumeration traffic; other physical ports were required to remain unchanged'}
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
