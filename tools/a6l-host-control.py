#!/usr/bin/python3 -I
"""Root-owned fixed service controller. Never executes project code or phone commands."""
import fcntl
import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import sys

UNITS = ('fwupd.service', 'ModemManager.service')
RUNTIME = Path('/run/a6l-host-control')
ENV = {'PATH': '/usr/sbin:/usr/bin:/sbin:/bin', 'LC_ALL': 'C'}


def action_from(argv):
    if len(argv) != 1 or argv[0] not in ('status', 'pause', 'resume'):
        raise ValueError('Only status, pause or resume is allowed, with no additional arguments')
    return argv[0]


def run(argv):
    result = subprocess.run(argv, env=ENV, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError('Fixed host command failed: ' + repr(argv) + ': ' + result.stderr.strip())
    return result.stdout.strip()


def state(unit):
    text = run(['/usr/bin/systemctl', 'show', unit, '-p', 'LoadState', '-p', 'ActiveState'])
    return dict(line.split('=', 1) for line in text.splitlines() if '=' in line)


def devices():
    result = []
    for item in Path('/sys/bus/usb/devices').glob('*'):
        try:
            pair = ((item / 'idVendor').read_text().strip(), (item / 'idProduct').read_text().strip())
        except OSError:
            continue
        if pair in (('05c6', '9008'), ('18d1', 'd00d'), ('1d6b', '0104')):
            result.append(item.name)
    return result


def save(data):
    path = RUNTIME / 'state.json'
    temp = RUNTIME / 'state.tmp'
    temp.write_text(json.dumps(data) + '\n')
    temp.chmod(0o600)
    temp.replace(path)


def secure_runtime():
    RUNTIME.mkdir(mode=0o700, exist_ok=True)
    info = RUNTIME.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o077:
        raise RuntimeError('Unsafe runtime directory')


def main():
    action = action_from(sys.argv[1:])
    if os.geteuid() != 0 or os.environ.get('SUDO_UID') != '1000' or socket.gethostname() != 'system76-pc':
        raise RuntimeError('Requires the configured laptop sudo caller')
    os.umask(0o077)
    secure_runtime()
    lock = os.open(RUNTIME / 'lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(lock, 'w') as handle:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        path = RUNTIME / 'state.json'
        recorded = json.loads(path.read_text()) if path.exists() else None
        if action == 'status':
            print(json.dumps({'units': {u: state(u) for u in UNITS}, 'owned_pause': recorded,
                              'development_usb_devices': devices()}))
            return
        if action == 'pause':
            if recorded is not None:
                raise RuntimeError('A pause is already recorded; inspect or resume it first')
            usb = Path('/sys/bus/usb/devices/3-2')
            if (usb / 'serial').read_text().strip() != '1e529013' or (usb / 'idVendor').read_text().strip() != '109b':
                raise RuntimeError('Pause requires the spare in Android on port 3-2')
            if devices():
                raise RuntimeError('Development USB device already present')
            initial = {u: state(u) for u in UNITS}
            for unit, before in initial.items():
                override = Path('/run/systemd/system') / unit
                if (before.get('LoadState') != 'loaded' or before.get('ActiveState') not in ('active', 'inactive')
                        or override.exists() or override.is_symlink()):
                    raise RuntimeError('Unexpected service state: ' + unit)
            if initial['fwupd.service']['ActiveState'] == 'active':
                if run(['/usr/bin/busctl', '--system', 'get-property', 'org.freedesktop.fwupd', '/', 'org.freedesktop.fwupd', 'Status']) != 'u 1':
                    raise RuntimeError('fwupd is busy')
            if initial['ModemManager.service']['ActiveState'] == 'active':
                if 'No modems were found' not in run(['/usr/bin/mmcli', '-L']):
                    raise RuntimeError('ModemManager has an existing modem')
            recorded = {'initial': initial, 'owned': [], 'pending': None}
            save(recorded)
            for unit in UNITS:
                recorded['pending'] = unit
                save(recorded)
                run(['/usr/bin/systemctl', 'mask', '--runtime', '--now', unit])
                recorded['owned'].append(unit)
                recorded['pending'] = None
                save(recorded)
            print(json.dumps({'paused': True, 'state': recorded}))
            return
        # Only masks created by this helper are eligible for removal.
        if recorded is None:
            print(json.dumps({'resumed': True, 'nothing_to_restore': True}))
            return
        if devices():
            raise RuntimeError('Keep services paused while development USB is present')
        if set(recorded['initial']) != set(UNITS) or any(u not in UNITS for u in recorded['owned']):
            raise RuntimeError('Unexpected recorded state')
        pending = recorded.get('pending')
        if pending is not None:
            if pending not in UNITS:
                raise RuntimeError('Unexpected pending service')
            override = Path('/run/systemd/system') / pending
            if override.is_symlink() and os.readlink(override) == '/dev/null' and pending not in recorded['owned']:
                recorded['owned'].append(pending)
            recorded['pending'] = None
            save(recorded)
        for unit in reversed(recorded['owned'][:]):
            override = Path('/run/systemd/system') / unit
            already_unmasked = not override.exists() and not override.is_symlink() and recorded.get('restoring') == unit
            if not already_unmasked and (not override.is_symlink() or os.readlink(override) != '/dev/null'):
                raise RuntimeError('Owned runtime mask changed: ' + unit)
            recorded['restoring'] = unit
            save(recorded)
            if not already_unmasked:
                run(['/usr/bin/systemctl', 'unmask', '--runtime', unit])
            if recorded['initial'][unit]['ActiveState'] == 'active':
                run(['/usr/bin/systemctl', 'start', unit])
            if state(unit) != recorded['initial'][unit]:
                raise RuntimeError('Service restoration differs: ' + unit)
            recorded['owned'].remove(unit)
            recorded['restoring'] = None
            save(recorded)
        path.unlink()
        print(json.dumps({'resumed': True, 'units': {u: state(u) for u in UNITS}}))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
