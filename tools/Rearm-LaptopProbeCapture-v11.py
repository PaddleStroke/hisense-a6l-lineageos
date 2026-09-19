#!/usr/bin/env python3
"""Rearm only the read-only logger after V11's network-interrupted window."""
import hashlib
import json
import os
from pathlib import Path
import pwd
import socket
import subprocess
import time

ROOT = Path('/home/pierrelouis/A6L-usb-20260915')
OUT = ROOT / 'capture-probe-serial-v11-rearm-2'
COLLECTOR = ROOT / 'Collect-ProbeSerial-v1.py'

def check():
    assert pwd.getpwuid(os.getuid()).pw_name == 'pierrelouis'
    assert socket.gethostname().split('.')[0] == 'system76-pc'
    assert hashlib.sha256(COLLECTOR.read_bytes()).hexdigest() == '3f62455fec2edff76e1c3186e859743d382fdad69d9471f61ce665335e479076'
    old = json.loads((ROOT / 'capture-probe-serial-v11/session.json').read_text())
    assert old['phase'] == 'finished' and old['worker_exit'] == 1
    assert not old['services_restored']
    serial = json.loads((ROOT / 'capture-probe-serial-v11/serial/report.json').read_text())
    assert serial['finished_utc'] and serial['bytes'] == 0
    install = json.loads((ROOT / 'capture-diagnostic-install-v11/edl/report.json').read_text())
    assert install['readback_verified'] and not install.get('error')
    assert install['target_sha256'] == '54ec8ad057bbb4bd52c63ad2c82ee68efd0a4828ff674cfdc51acbb41a12259a'
    usb = Path('/sys/bus/usb/devices/3-2')
    assert [(usb / n).read_text().strip() for n in ['idVendor', 'idProduct', 'serial']] == ['18d1', 'd00d', '1e529013']
    for unit in ['fwupd.service', 'ModemManager.service']:
        p = Path('/run/systemd/system') / unit
        assert p.is_symlink() and os.readlink(p) == '/dev/null'
        state = subprocess.run(['systemctl', 'show', unit, '-p', 'LoadState', '-p', 'ActiveState'], capture_output=True, text=True, check=True).stdout
        assert 'LoadState=masked' in state and 'ActiveState=inactive' in state
    for p in Path('/proc').glob('[0-9]*/cmdline'):
        try:
            args = p.read_bytes().split(b'\0')
        except OSError:
            continue
        assert not any(a.endswith((b'/Run-LaptopProbeCapture-v11.py', b'/Collect-ProbeSerial-v1.py')) for a in args), 'An earlier capture is still running'

check()
assert not OUT.exists()
print('Enter the laptop password to arm the read-only logger. Leave the phone in fastboot.', flush=True)
subprocess.run(['sudo', '-v'], check=True)
check()
OUT.mkdir(mode=0o700)
# Authorize on the current TTY first, then detach the bounded root observer.
# Detaching sudo itself loses Ubuntu's TTY-scoped timestamp.
argv = ['/usr/bin/timeout', '--signal=TERM', '--kill-after=3s', '610s', '/usr/bin/python3', str(COLLECTOR), str(OUT / 'serial'), '--seconds', '600']
launcher = ('import subprocess\n'
            + 'with open(' + repr(str(OUT / 'worker.log')) + ', "x") as log:\n'
            + ' p = subprocess.Popen(' + repr(argv) + ', stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)\n'
            + 'print(p.pid)\n')
launched = subprocess.run(['sudo', '-n', '/usr/bin/python3', '-c', launcher], capture_output=True, text=True, check=True)
worker_pid = int(launched.stdout.strip())
(OUT / 'launch.json').write_text(json.dumps({'pid': worker_pid, 'scope': 'Read-only USB logger; no reboot, image writes or service changes', 'services_left_masked': True}, indent=2) + '\n')
time.sleep(1)
assert Path('/proc', str(worker_pid)).exists(), (OUT / 'worker.log').read_text()
report = json.loads((OUT / 'serial/report.json').read_text())
assert report['usb_observations'][-1]['state']['idProduct'] == 'd00d'
print('Logger armed. Wait for Codex instructions before selecting Recovery.', flush=True)
