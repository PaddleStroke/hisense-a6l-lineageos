#!/usr/bin/env python3
"""Read only the diagnostic gadget's serial output into private host files."""
import argparse
import errno
import re
from datetime import datetime, timezone
import sys
import json
from pathlib import Path
import time
sys.path.insert(0, str(Path(__file__).resolve().parent / 'a6l-recovery-kit/deps'))
import serial
from serial.tools import list_ports

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--seconds', type=int, default=180, choices=range(10, 3601))
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
report = {'scope': 'Host serial log capture only; no command or data bytes sent',
          'expected_usb_identity': {'vid': '1d6b', 'pid': '0104', 'serial': 'HLTE730T-PROBE'},
          'ports_opened': [], 'bytes': 0, 'started_utc': datetime.now(timezone.utc).isoformat(), 'usb_observations': []}
deadline = time.monotonic() + args.seconds
connection = None
payload = bytearray()
last_usb = None
boot_deadline = None
permission_denied_since = None

def capture_pending(now, menu_deadline, boot_deadline):
    # Once Recovery departs, grant the entire diagnostic window.
    return now < (boot_deadline if boot_deadline is not None else menu_deadline)


def storage_window_complete(data):
    forks = [int(v) for v in re.findall(rb'A6L_STORAGE_MODULE_FORK_BEGIN seconds=([0-9]+)', data)]
    alive = [int(v) for v in re.findall(rb'A6L_RAM_PROBE_ALIVE seconds=([0-9]+)', data)]
    return bool(b'A6L_STORAGE_MODULE_LOAD_END result=0 errno=0' in data and b'A6L_STORAGE_SNAPSHOT_DONE' in data and forks and alive and max(alive) >= forks[0] + 20)

def save():
    temporary = args.output / 'report.json.tmp'
    temporary.write_text(json.dumps(report, indent=2) + '\n')
    temporary.replace(args.output / 'report.json')

save()
try:
    with (args.output / 'console.bin').open('wb') as log:
        while capture_pending(time.monotonic(), deadline, boot_deadline):
            state = {}
            for name in ['idVendor', 'idProduct', 'serial']:
                try:
                    state[name] = (Path('/sys/bus/usb/devices/3-2') / name).read_text().strip()
                except OSError:
                    pass
            if (boot_deadline is None and last_usb and last_usb.get("idVendor") == "18d1"
                    and last_usb.get("idProduct") == "d00d" and state != last_usb):
                boot_deadline = time.monotonic() + 60
                report["recovery_departure_utc"] = datetime.now(timezone.utc).isoformat()
                report["post_selection_capture_seconds"] = 60
            if state != last_usb:
                report['usb_observations'].append({'utc': datetime.now(timezone.utc).isoformat(), 'state': state})
                last_usb = state
                save()
            if connection is None:
                matches = [port for port in list_ports.comports()
                           if port.vid == 0x1d6b and port.pid == 0x0104 and
                           port.serial_number == 'HLTE730T-PROBE']
                if len(matches) > 1:
                    raise RuntimeError('More than one matching diagnostic gadget; refusing to choose')
                if not matches:
                    time.sleep(0.5)
                    continue
                physical = Path('/sys/class/tty') / Path(matches[0].device).name / 'device'
                if '3-2' not in physical.resolve().parts:
                    raise RuntimeError('Diagnostic gadget is not on the established physical USB port')
                try:
                    connection = serial.Serial(matches[0].device, baudrate=115200, timeout=0.5, exclusive=True)
                except (serial.SerialException, OSError) as error:
                    if getattr(error, 'errno', None) != errno.EACCES:
                        raise
                    now = time.monotonic()
                    if permission_denied_since is None:
                        permission_denied_since = now
                    if now - permission_denied_since >= 5:
                        raise
                    report['permission_retry_count'] = report.get('permission_retry_count', 0) + 1
                    save()
                    time.sleep(0.25)
                    continue
                permission_denied_since = None
                report['ports_opened'].append(matches[0].device)
                save()
                print(f'Diagnostic gadget connected on {matches[0].device}', flush=True)
            try:
                chunk = connection.read(16384)
            except (serial.SerialException, OSError) as error:
                report.setdefault('disconnects', []).append(str(error))
                connection.close()
                connection = None
                continue
            if chunk:
                payload.extend(chunk)
                log.write(chunk)
                log.flush()
                report['bytes'] = len(payload)
                report['ready_seen'] = b'A6L_RAM_PROBE_READY' in payload
                report['heartbeat_count'] = payload.count(b'A6L_RAM_PROBE_ALIVE')
                save()
                if (report['ready_seen'] and b'A6L_SERIAL_WRITE_END' in payload
                        and storage_window_complete(payload)):
                    break
                if len(payload) > 16 * 1024 * 1024:
                    raise RuntimeError('Diagnostic log exceeded the 16 MiB capture bound')
except Exception as error:
    report['error'] = str(error)
finally:
    if connection:
        connection.close()
    report['bytes'] = len(payload)
    report['ready_seen'] = b'A6L_RAM_PROBE_READY' in payload
    report['heartbeat_seen'] = b'A6L_RAM_PROBE_ALIVE' in payload
    report['kernel_panic_seen'] = b'Kernel panic' in payload
    report['staged_capture_complete'] = (b'A6L_SERIAL_WRITE_END' in payload and storage_window_complete(payload))
    (args.output / 'console.txt').write_text(payload.decode(errors='replace'))
    report['stop_reason'] = ('error' if report.get('error') else
                             'storage_window_complete' if report['staged_capture_complete'] else
                             'diagnostic_deadline' if boot_deadline is not None else 'menu_deadline')
    report['finished_utc'] = datetime.now(timezone.utc).isoformat()
    save()
print(json.dumps(report, indent=2))
raise SystemExit(0 if report['ready_seen'] and report['staged_capture_complete'] and not report.get('error') else 1)
