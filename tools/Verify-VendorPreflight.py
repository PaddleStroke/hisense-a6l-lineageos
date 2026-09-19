#!/usr/bin/env python3
"""Verify saved preflight/unlock USB captures offline, without accessing a phone."""
import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--unlock-capture', action='store_true', help='Verify the completed approved unlock capture instead of the earlier preflight')
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
case = 'vendor-unlock' if args.unlock_capture else 'vendor-query'
capture = root / ('captures/fastboot-laptop-' + case + '-20260915')
setup = json.loads((capture / (case + '-usb-setup/report.json')).read_text())
session = json.loads((capture / 'session.json').read_text())
host = json.loads((capture / ('fwupd-' + case + '-session.json')).read_text())
assert not setup['errors']
addresses = [int(addr) for addr, info in setup['device_snapshots'].items()
             if info['idVendor'] == '18d1' and info['idProduct'] == 'd00d'
             and info['serial'] == '1e529013' and info['quirks'] == '0x400']
assert len(addresses) == 1
commands, replies, pending, unsupported_queries = [], [], {}, []
completions = 0
for line in (capture / (case + '-usb-setup/usbmon-setup.txt')).read_text().splitlines():
    fields = line.split()
    tag, event, pipe = fields[0], fields[2], fields[3].split(':')
    if pipe[0] not in ('Bo', 'Bi') or int(pipe[1]) != setup['bus'] or int(pipe[2]) != addresses[0]:
        continue
    payload = bytes.fromhex(''.join(fields[fields.index('=') + 1:])) if '=' in fields else b''
    length = int(fields[5])
    if event == 'S':
        assert tag not in pending
        pending[tag] = (pipe[0], length)
        if pipe[0] == 'Bo':
            command = payload.decode('ascii')
            if len(payload) != length:
                # usbmon text retains at most 32 payload bytes. Keep this
                # limitation explicit instead of inventing the absent suffix.
                assert args.unlock_capture and len(payload) == 32 and length == 36
                assert command == 'getvar:partition-type:avb_custom'
                command += '[truncated; submitted 36 bytes]'
            commands.append(command)
    elif event == 'C':
        assert fields[4] == '0', line
        submitted_pipe, submitted_length = pending.pop(tag)
        assert submitted_pipe == pipe[0]
        assert length <= submitted_length
        if pipe[0] == 'Bo':
            assert length == submitted_length
        else:
            if payload.startswith(b'FAIL'):
                assert args.unlock_capture and payload == b'FAILGetVar Variable Not found'
                assert commands[-1] in ('getvar:has-slot:avb_custom_key',
                    'getvar:partition-type:avb_custom[truncated; submitted 36 bytes]')
                unsupported_queries.append(commands[-1])
            else:
                assert payload.startswith((b'INFO', b'OKAY')), payload
            replies.append(payload[:4].decode('ascii'))
        completions += 1
    else:
        raise AssertionError(line)
assert not pending
expected = ['getvar:product'] * 4 + [
    'getvar:unlocked', 'getvar:secure', 'oem device-info',
    'flashing get_unlock_ability']
if args.unlock_capture:
    expected += ['Hisense unlock', 'oem device-info',
                 'getvar:has-slot:avb_custom_key',
                 'getvar:partition-type:avb_custom[truncated; submitted 36 bytes]',
                 'erase:avb_custom_key']
expected += ['reboot']
assert commands == expected
assert replies.count('OKAY') + replies.count('FAIL') == len(commands)
assert len(unsupported_queries) == (2 if args.unlock_capture else 0)
assert session['preflight_passed'] and not session.get('error')
assert host['service_restored'] and host['usb_quirks']['restored']
if args.unlock_capture:
    assert session['runtime_unlock_verified'] and session['persistence_acknowledged']
    assert session['reboot']['ok']
else:
    assert session['android_return_verified'] and host['test_exit'] == 0
    assert not session['runtime_unlock_attempted'] and not session['persistence_attempted']
    assert session['after_properties']['ro.boot.flash.locked'] == '1'
    assert session['after_properties']['ro.boot.verifiedbootstate'] == 'green'
report = {'scope': 'Offline verification of the ' + ('approved unlock' if args.unlock_capture else 'read-only preflight') + ' capture',
          'wire_commands': commands, 'okay_replies': replies.count('OKAY'),
          'successful_bulk_completions': completions,
          'stock_return_verified': session['android_return_verified'],
          'host_settings_restored': True, 'unlock_command_sent': args.unlock_capture}
if args.unlock_capture:
    report['limitation'] = 'Command acknowledgment is verified; persistence across reboot requires a separate post-reset check'
    report['unsupported_optional_client_queries'] = unsupported_queries
(capture / 'wire-verification.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
