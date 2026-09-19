#!/usr/bin/env python3
"""Read-only Android verification after the approved A6L unlock and data reset."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import pwd
import socket
import subprocess

SERIAL = '1e529013'
FINGERPRINT = 'Hisense/HLTE730T/HLTE730T:9/PKQ1.190723.001/L1632.6.01.04:user/release-keys'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    assert pwd.getpwuid(__import__('os').getuid()).pw_name == 'pierrelouis'
    assert socket.gethostname().split('.')[0] == 'system76-pc'
    report = {'scope': 'Read-only post-reset Android verification; no reboot or write to phone',
              'started_utc': datetime.now(timezone.utc).isoformat(), 'commands': [],
              'android_return_verified': False, 'ordinary_unlock_persisted': False,
              'critical_unlock_after_reboot': 'Not directly queried by this Android-only check'}

    def run(command):
        entry = {'argv': command}
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=8)
            entry.update(exit=result.returncode, stdout=result.stdout, stderr=result.stderr)
        except subprocess.TimeoutExpired:
            entry.update(exit=None, stdout='', stderr='Timed out after eight seconds')
        report['commands'].append(entry)
        return entry

    # Refuse to overwrite earlier evidence even if the check fails.
    with args.output.open('x', encoding='utf-8') as output:
        state = run(['/usr/bin/adb', '-s', SERIAL, 'get-state'])
        if state['exit'] == 0 and state['stdout'].strip() == 'device':
            props = {}
            for prop in ['ro.build.fingerprint', 'sys.boot_completed', 'ro.boot.flash.locked',
                         'ro.boot.verifiedbootstate', 'ro.boot.vbmeta.device_state',
                         'ro.crypto.state', 'ro.crypto.type', 'ro.bootmode']:
                result = run(['/usr/bin/adb', '-s', SERIAL, 'shell', 'getprop', prop])
                props[prop] = result['stdout'].strip() if result['exit'] == 0 else None
            report['properties'] = props
            report['android_return_verified'] = (props['ro.build.fingerprint'] == FINGERPRINT
                                                  and props['sys.boot_completed'] == '1')
            report['ordinary_unlock_persisted'] = props['ro.boot.flash.locked'] == '0'
        else:
            report['requires_usb_debugging_or_setup'] = True
        host = run(['/usr/bin/systemctl', 'show', 'fwupd.service', '-p', 'LoadState', '-p', 'ActiveState'])
        quirks = Path('/sys/module/usbcore/parameters/quirks').read_text().strip()
        report['host'] = {'usb_quirks': quirks, 'fwupd_loaded_and_active':
                          host['exit'] == 0 and 'LoadState=loaded' in host['stdout']
                          and 'ActiveState=active' in host['stdout']}
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        output.write(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0 if report['android_return_verified'] and report['ordinary_unlock_persisted'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
