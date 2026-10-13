#!/usr/bin/env python3
"""Prepare a task-specific SSH key and a reviewed public-key installer."""
import base64
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
ACCESS = ROOT / 'logs/laptop-access'
ACCESS.mkdir(parents=True, exist_ok=True)
KEY = ACCESS / 'a6l_ed25519'
if not KEY.exists():
    subprocess.run(['C:/Windows/System32/OpenSSH/ssh-keygen.exe', '-q', '-t', 'ed25519',
                    '-N', '', '-C', 'a6l-port-desktop', '-f', str(KEY)], check=True)
public = KEY.with_suffix('.pub').read_text().strip()
if not public.startswith('ssh-ed25519 ') or len(public.split()) != 3:
    raise SystemExit('Unexpected public key format')
line = 'from="192.168.1.11",no-agent-forwarding,no-port-forwarding,no-X11-forwarding ' + public
remote = '''import fcntl, os, pathlib, pwd, socket
assert pwd.getpwuid(os.getuid()).pw_name == "pierrelouis", "Unexpected remote user"
assert socket.gethostname().split(".")[0] == "system76-pc", "Unexpected laptop hostname"
line = PUBLIC_KEY_LINE
directory = pathlib.Path.home() / ".ssh"
assert not directory.is_symlink(), "Refusing a symlinked .ssh directory"
directory.mkdir(mode=0o700, exist_ok=True)
assert directory.stat().st_uid == os.getuid(), "Unexpected .ssh owner"
target = directory / "authorized_keys"
assert not target.is_symlink(), "Refusing symlinked authorized_keys"
if target.exists():
    assert target.stat().st_uid == os.getuid(), "Unexpected authorized_keys owner"
fd = os.open(str(target), os.O_RDWR | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW, 0o600)
with os.fdopen(fd, "a+") as handle:
    fcntl.flock(handle, fcntl.LOCK_EX)
    handle.seek(0)
    existing = handle.read()
    if line not in existing.splitlines():
        handle.write(("\\n" if existing and not existing.endswith("\\n") else "") + line + "\\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.fchmod(handle.fileno(), 0o600)
directory.chmod(0o700)
print("A6L_ACCESS_KEY_INSTALLED")
'''.replace('PUBLIC_KEY_LINE', repr(line))
compile(remote, '<remote-key-installer>', 'exec')
(ACCESS / 'install-public-key.py').write_text(remote)
encoded = base64.b64encode(remote.encode()).decode()
(ACCESS / 'remote-command.txt').write_text(
    'printf %s ' + encoded + ' | base64 -d | python3')
subprocess.run(['C:/Windows/System32/OpenSSH/ssh-keygen.exe', '-lf', str(KEY.with_suffix('.pub'))], check=True)
print('Prepared public-key installer for pierrelouis@192.168.1.22; no password stored.')
