#!/usr/bin/env python3
"""Close the interrupted capture's host-service state, then use guarded restore."""
import hashlib
import json
import os
from pathlib import Path
import pwd
import socket
import subprocess
import sys
import time
from datetime import datetime, timezone

ROOT = Path('/home/pierrelouis/A6L-usb-20260915')
OUT = ROOT / 'capture-probe-serial-v11-rearm-2'
REPORT = OUT / 'cleanup.json'
EXPECTED = {'ro.build.fingerprint': 'Hisense/HLTE730T/HLTE730T:9/PKQ1.190723.001/L1632.6.01.04:user/release-keys',
            'sys.boot_completed': '1', 'ro.boot.flash.locked': '0', 'ro.boot.verifiedbootstate': 'orange'}
assert os.geteuid() != 0 and pwd.getpwuid(os.getuid()).pw_name == 'pierrelouis'
assert socket.gethostname().split('.')[0] == 'system76-pc'
assert not REPORT.exists() and not (ROOT / 'capture-diagnostic-restore-v11').exists()
restore = ROOT / 'Run-LaptopDiagnosticRestore-v11.py'
assert hashlib.sha256(restore.read_bytes()).hexdigest() == 'cc5e46b525c3b0f37dc7b712a315cf20f5d1b0d2959e96d6ede57cc5ee3b4db4'
print('Waiting for the bounded read-only logger to finish; no phone action is being sent.', flush=True)
deadline = time.monotonic() + 180
while not json.loads((OUT / 'serial/report.json').read_text()).get('finished_utc'):
    assert time.monotonic() < deadline, 'Observer has not completed; inspect before retrying'
    time.sleep(2)
for p in Path('/proc').glob('[0-9]*/cmdline'):
    try:
        args = p.read_bytes().split(b'\0')
    except OSError:
        continue
    assert not any(a.endswith(b'/Collect-ProbeSerial-v1.py') for a in args), 'Collector remains running'
values = {p: subprocess.run(['/usr/bin/adb', '-s', '1e529013', 'shell', 'getprop', p], capture_output=True, text=True, check=True, timeout=5).stdout.strip() for p in EXPECTED}
assert values == EXPECTED
usb = Path('/sys/bus/usb/devices/3-2')
assert (usb / 'serial').read_text().strip() == '1e529013'
assert (usb / 'idVendor').read_text().strip() == '109b'
original = json.loads((ROOT / 'capture-probe-serial-v11/session.json').read_text())
assert original['phase'] == 'finished'
units = ['fwupd.service', 'ModemManager.service']
for unit in units:
    states = [c for c in original['commands'] if c['argv'] == ['/usr/bin/systemctl', 'show', unit, '-p', 'LoadState', '-p', 'ActiveState']]
    assert len(states) == 1 and states[0]['exit'] == 0
    assert 'LoadState=loaded' in states[0]['stdout'] and 'ActiveState=active' in states[0]['stdout']
    override = Path('/run/systemd/system') / unit
    assert override.is_symlink() and os.readlink(override) == '/dev/null'
print('Enter the laptop password if requested. This restores laptop services, then stock recovery with full readback.', flush=True)
subprocess.run(['sudo', '-v'], check=True)
report = {'started_utc': datetime.now(timezone.utc).isoformat(), 'android': values, 'commands': [], 'services_restored': False}
def save():
    temp = REPORT.with_suffix('.json.tmp')
    temp.write_text(json.dumps(report, indent=2) + '\n')
    temp.replace(REPORT)
save()
for unit in units:
    for argv in [['sudo', '-n', 'systemctl', 'unmask', '--runtime', unit], ['sudo', '-n', 'systemctl', 'start', unit], ['systemctl', 'show', unit, '-p', 'LoadState', '-p', 'ActiveState']]:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=30)
        report['commands'].append({'argv': argv, 'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr})
        save()
        assert result.returncode == 0
    assert 'LoadState=loaded' in result.stdout and 'ActiveState=active' in result.stdout
report['services_restored'] = True
report['finished_utc'] = datetime.now(timezone.utc).isoformat()
save()
print('Capture cleanup verified. Starting the unchanged guarded stock recovery restore.', flush=True)
os.execv(sys.executable, [sys.executable, str(restore)])
