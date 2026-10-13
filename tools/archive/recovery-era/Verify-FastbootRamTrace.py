#!/usr/bin/env python3
"""Check the completed RAM-only USB transcript offline, including byte counts."""
import json
from pathlib import Path

root = Path(__file__).resolve().parent.parent / 'captures/fastboot-laptop-ram-transfer-20260915'
lines = (root / 'fastboot/usbmon.txt').read_text().splitlines()
pending, current, transfers, commands = {}, None, [], []
for line in lines:
    parts = line.split()
    tag, event, pipe, status = parts[0], parts[2], parts[3], parts[4]
    length = int(parts[5])
    payload = bytes.fromhex(''.join(parts[parts.index('=') + 1:])) if '=' in parts else b''
    if event == 'C':
        assert status == '0', line
    if pipe.startswith('Bo:') and event == 'S':
        if current and current['data_acknowledged'] and current['completed_bytes'] < current['bytes']:
            pending[tag] = ('payload', length)
        else:
            command = payload.decode('ascii')
            assert len(payload) == length, 'A command was truncated in the trace'
            commands.append(command)
            pending[tag] = ('command', length)
            if command.startswith('download:'):
                assert current is None
                current = {'bytes': int(command.split(':')[1], 16),
                           'completed_bytes': 0, 'data_acknowledged': False}
    if pipe.startswith('Bo:') and event == 'C':
        kind, submitted = pending.pop(tag)
        assert submitted == length
        if kind == 'payload':
            current['completed_bytes'] += length
    if pipe.startswith('Bi:') and event == 'C':
        if payload.startswith(b'DATA'):
            # This ABL sends DATA plus eight hex digits and a trailing NUL.
            assert current and int(payload[4:].rstrip(b'\0'), 16) == current['bytes']
            current['data_acknowledged'] = True
        elif payload.startswith(b'OKAY') and current and current['data_acknowledged']:
            assert current['completed_bytes'] == current['bytes']
            transfers.append(current)
            current = None
assert not current and not pending
assert [t['bytes'] for t in transfers] == [1048576, 67108864]
allowed = {'download:00100000', 'download:04000000', 'oem device-info',
           'flashing get_unlock_ability', 'reboot'}
assert all(c.startswith('getvar:') or c in allowed for c in commands), commands
session = json.loads((root / 'session.json').read_text())
host = json.loads((root / 'fwupd-ram-transfer-session.json').read_text())
assert session['stock_return_verified'] and session['before_properties'] == session['after_properties']
assert host['service_restored'] and host['usb_quirks']['restored']
report = {'scope': 'Host USB transcript verification; no device-side RAM hash/readback available',
          'transfers': transfers, 'wire_commands': commands, 'all_usb_completions_successful': True,
          'stock_return_verified': True, 'host_settings_restored': True}
(root / 'wire-verification.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
