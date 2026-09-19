"""Bounded, receive-only V12 USB observer. No reboot, flash or fastboot commands."""
import json
import subprocess
import sys
import time
import serial
from serial.tools import list_ports
import WindowsRecoveryReadOnly as base

OUT = base.ROOT / 'captures/windows-v12-probe'
PNP_QUERY = "Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match '^USB\\\\VID_(18D1|109B|05C6|1D6B)' } | Select-Object Status,Class,FriendlyName,InstanceId | ConvertTo-Json -Compress"


def pnp_snapshot():
    result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', PNP_QUERY],
                            capture_output=True, text=True, timeout=8,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode:
        return {'error': result.stderr.strip()}
    return json.loads(result.stdout) if result.stdout.strip() else []


def collect():
    install = json.loads((base.ROOT / 'captures/windows-v12-install-diagnostic/session.json').read_text())
    assert install['independent_desktop_verification'] and install['worker_exit'] == 0
    OUT.mkdir(exist_ok=False)
    report = {'started_utc': base.utc(), 'scope': 'Receive-only USB observation for 300 seconds',
              'bytes': 0, 'port_events': [], 'pnp_events': [], 'ready': False}
    def save():
        base.save(OUT / 'report.json', report)
    save()
    report['pnp_events'].append({'utc': base.utc(), 'devices': pnp_snapshot()})
    report['ready'] = True
    save()
    connection = None
    tried = set()
    previous = None
    next_pnp = time.monotonic() + 2
    deadline = time.monotonic() + 300
    try:
        with (OUT / 'console.bin').open('xb') as log:
            while time.monotonic() < deadline:
                ports = list(list_ports.comports())
                relevant = [p for p in ports if p.vid in (0x1d6b, 0x05c6, 0x18d1, 0x109b)]
                state = [{'port': p.device, 'vid': p.vid, 'pid': p.pid, 'serial': p.serial_number} for p in relevant]
                if state != previous:
                    report['port_events'].append({'utc': base.utc(), 'ports': state})
                    previous = state
                    save()
                matches = [p for p in relevant if (p.vid, p.pid, p.serial_number) == (0x1d6b, 0x0104, 'HLTE730T-PROBE')]
                if connection is None and len(matches) == 1 and matches[0].device not in tried:
                    port = matches[0].device
                    tried.add(port)
                    try:
                        connection = serial.Serial(port, 115200, timeout=.2, write_timeout=1)
                        report['opened_port'] = port
                        save()
                    except serial.SerialException as error:
                        report.setdefault('errors', []).append(repr(error))
                        save()
                if connection is not None:
                    try:
                        data = connection.read(min(max(connection.in_waiting, 1), 65536))
                        if data:
                            log.write(data)
                            log.flush()
                            report['bytes'] += len(data)
                            save()
                    except serial.SerialException as error:
                        report.setdefault('errors', []).append(repr(error))
                        connection.close()
                        connection = None
                        save()
                if time.monotonic() >= next_pnp:
                    try:
                        state = pnp_snapshot()
                    except (subprocess.SubprocessError, ValueError) as error:
                        state = {'error': repr(error)}
                    if state != report['pnp_events'][-1]['devices']:
                        report['pnp_events'].append({'utc': base.utc(), 'devices': state})
                        save()
                    next_pnp = time.monotonic() + 2
                time.sleep(.1)
    finally:
        if connection is not None:
            connection.close()
        report['finished_utc'] = base.utc()
        save()


if __name__ == '__main__':
    if sys.argv[1:] != ['--collect']:
        raise SystemExit('Use --collect after verified diagnostic installation')
    collect()
