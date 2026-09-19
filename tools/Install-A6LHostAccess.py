#!/usr/bin/python3 -I
"""One-time, user-authorized installation of scoped A6L USB/service access."""
import hashlib
import json
import os
from pathlib import Path
import pwd
import socket
import subprocess
import tempfile

ROOT = Path('/home/pierrelouis/A6L-usb-20260915')
SOURCE = ROOT / 'host-access-v1'
REPORT = ROOT / 'host-access-install-v2.json'
MANIFEST_HASH = '7331738640facceb7e10f71c519aab2d165015bcd57fe5e59a737efc4264ec12'
TARGETS = {
    'a6l-host-control.py': (Path('/usr/local/sbin/a6l-host-control'), 0o755),
    '99-a6l-access.rules': (Path('/etc/udev/rules.d/99-a6l-access.rules'), 0o644),
    'a6l-host-control.sudoers': (Path('/etc/sudoers.d/a6l-host-control'), 0o440),
}


def run(argv):
    r = subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30,
                       env={'PATH': '/usr/sbin:/usr/bin:/sbin:/bin', 'LC_ALL': 'C'})
    if r.returncode:
        raise RuntimeError(repr(argv) + ': ' + r.stdout + r.stderr)
    return r.stdout.strip()


def main():
    assert os.geteuid() == 0 and os.environ.get('SUDO_UID') == '1000'
    assert socket.gethostname() == 'system76-pc' and pwd.getpwuid(1000).pw_name == 'pierrelouis'
    assert not REPORT.exists(), 'Existing install report; inspect before retrying'
    raw = (SOURCE / 'manifest.json').read_bytes()
    assert hashlib.sha256(raw).hexdigest() == MANIFEST_HASH
    manifest = json.loads(raw)
    data = {name: (SOURCE / name).read_bytes() for name in TARGETS}
    for name, (target, _) in TARGETS.items():
        assert hashlib.sha256(data[name]).hexdigest() == manifest['files'][name], name
        assert not target.exists() and not target.is_symlink(), target
    assert (Path('/sys/bus/usb/devices/3-2') / 'serial').read_text().strip() == '1e529013'
    with tempfile.TemporaryDirectory(prefix='a6l-access-check-', dir='/run') as temp:
        test = Path(temp) / 'sudoers'
        test.write_bytes(data['a6l-host-control.sudoers'])
        test.chmod(0o440)
        run(['/usr/sbin/visudo', '-cf', str(test)])
    report = {'installed': False, 'files': {}, 'scope': 'A6L USB owner permissions and three fixed host-service helper actions only'}
    created = []
    try:
        for name, (target, mode) in TARGETS.items():
            with os.fdopen(os.open(target, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, mode), 'wb') as f:
                created.append(target)
                f.write(data[name])
            target.chmod(mode)
            info = target.stat()
            assert info.st_uid == 0 and info.st_gid == 0 and info.st_mode & 0o777 == mode
            assert hashlib.sha256(target.read_bytes()).hexdigest() == manifest['files'][name]
            report['files'][str(target)] = {'sha256': manifest['files'][name], 'mode': oct(mode), 'uid': info.st_uid}
        run(['/usr/sbin/visudo', '-c'])
        run(['/usr/bin/udevadm', 'control', '--reload-rules'])
        run(['/usr/bin/udevadm', 'trigger', '--action=change', '/sys/bus/usb/devices/3-2'])
        run(['/usr/bin/udevadm', 'settle', '--timeout=10'])
        usb = Path('/sys/bus/usb/devices/3-2')
        node = Path('/dev/bus/usb') / f'{int((usb / "busnum").read_text()):03d}' / f'{int((usb / "devnum").read_text()):03d}'
        info = node.stat()
        # Ubuntu's earlier Android rule may lock in plugdev group access.
        # Require direct owner read/write and no world access; do not weaken
        # that existing rule simply to force a narrower group mode here.
        assert info.st_uid == 1000 and info.st_mode & 0o600 == 0o600 and info.st_mode & 0o007 == 0, (info.st_uid, oct(info.st_mode & 0o777))
        report['current_usb_node'] = {'path': str(node), 'uid': info.st_uid, 'mode': oct(info.st_mode & 0o777)}
        report['installed'] = True
        print('A6L scoped access installed. No password saved; no phone reboot or flash performed.', flush=True)
    except BaseException as error:
        report['error'] = repr(error)
        for target in reversed(created):
            target.unlink()
        run(['/usr/bin/udevadm', 'control', '--reload-rules'])
        raise
    finally:
        # The report contains configuration hashes and status only.
        with os.fdopen(os.open(REPORT, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600), 'w') as f:
            json.dump(report, f, indent=2)
            f.write('\n')
        os.chown(REPORT, 1000, pwd.getpwuid(1000).pw_gid)


if __name__ == '__main__':
    main()
