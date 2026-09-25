#!/usr/bin/env python3
"""A6L rom-v1 INSTALL coordinator (agent flash). ATTENDED ONLY — start it through Launch-RomV1Install.py.
From stock Android (Magisk-rooted stock, V74 diagnostic recovery): checks, host pause, adb reboot edl, EDL worker
(backup everything it writes + ROM-writable regions, write boot/dtbo/vendor/system, zero userdata head, full readback,
power off), host resume. Does NOT boot the phone: Pierre presses Power afterwards.
"""
from datetime import datetime, timezone
import json, os, pwd, re, socket, subprocess, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CAPTURE = ROOT / 'capture-rom-v1-install'
SERIAL = '1e529013'
EXPECTED = {'ro.build.fingerprint': 'Hisense/HLTE730T/HLTE730T:9/PKQ1.190723.001/L1632.6.01.04:user/release-keys',
            'sys.boot_completed': '1', 'ro.boot.flash.locked': '0', 'ro.boot.verifiedbootstate': 'orange'}
WORKER_TIMEOUT = 5500


def usb(vid, pid):
    found = []
    for item in Path('/sys/bus/usb/devices').glob('*'):
        try:
            if (item / 'idVendor').read_text().strip() == vid and (item / 'idProduct').read_text().strip() == pid:
                found.append(item.name)
        except OSError:
            pass
    return found


def main():
    assert os.geteuid() != 0 and pwd.getpwuid(os.getuid()).pw_name == 'pierrelouis'
    assert socket.gethostname().split('.')[0] == 'system76-pc'
    CAPTURE.mkdir(mode=0o700, exist_ok=False)
    report = {'scope': 'rom-v1 install: backup, write boot/dtbo/vendor/system + userdata head zero, readback, poweroff',
              'started_utc': datetime.now(timezone.utc).isoformat(), 'commands': [], 'services_restored': False}
    paused = edl_requested = False
    code = 1

    def save():
        t = CAPTURE / 'session.json.tmp'; t.write_text(json.dumps(report, indent=2) + '\n'); t.replace(CAPTURE / 'session.json')

    def run(argv, required=True, timeout=10):
        e = {'argv': argv}; report['commands'].append(e); save()
        try:
            r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
            e.update(exit=r.returncode, stdout=r.stdout[-4000:], stderr=r.stderr[-4000:])
        except subprocess.TimeoutExpired:
            e.update(exit=None, stdout='', stderr='timeout')
        save()
        if required and e['exit'] != 0:
            raise RuntimeError('command failed: ' + ' '.join(argv))
        return e

    try:
        if not all(w in run(['/usr/bin/gnome-session-inhibit', '--list'])['stdout'] for w in ['A6L-rom-v1', 'suspend', 'idle']):
            raise RuntimeError('requires the desktop idle/suspend inhibitor (use Launch-RomV1Install.py)')
        run(['/usr/bin/python3', str(ROOT / 'Verify-RomV1Stage.py')], timeout=900)
        free = os.statvfs(ROOT); free_gb = free.f_bavail * free.f_frsize / 1e9
        report['free_gb'] = free_gb
        if free_gb < 12:
            raise RuntimeError('need >= 12 GB free for the backup (system 6 GiB + vendor 1.1 GiB + small regions)')
        values = {}
        for prop in EXPECTED:
            r = run(['/usr/bin/adb', '-s', SERIAL, 'shell', 'getprop', prop], required=False, timeout=4)
            values[prop] = r['stdout'].strip() if r['exit'] == 0 else None
        report['before'] = values
        if values != EXPECTED:
            raise RuntimeError('phone is not in the verified stock Android state')
        if (Path('/sys/bus/usb/devices/3-2') / 'serial').read_text().strip() != SERIAL:
            raise RuntimeError('phone is not on the established physical port 3-2')
        battery = run(['/usr/bin/adb', '-s', SERIAL, 'shell', 'dumpsys', 'battery'])['stdout']
        level = re.search(r'^\s*level:\s*(\d+)\s*$', battery, re.M)
        if not level or int(level.group(1)) < 60:
            raise RuntimeError('battery must be >= 60 % (long EDL session)')
        state = json.loads(run(['/usr/bin/sudo', '-n', '/usr/local/sbin/a6l-host-control', 'status'])['stdout'])
        if state['owned_pause'] is not None:
            raise RuntimeError('an earlier host pause needs inspection/cleanup (stop fwupd etc. as on 23 Sep)')
        paused = True
        run(['/usr/bin/sudo', '-n', '/usr/local/sbin/a6l-host-control', 'pause'])
        print('Host services paused; rebooting to EDL. Expect about 10-25 min (backup ~7.7 GB, write ~1.5 GB, readback).', flush=True)
        edl_requested = True
        run(['/usr/bin/adb', '-s', SERIAL, 'reboot', 'edl'])
        deadline = time.monotonic() + 30
        while not usb('05c6', '9008') and time.monotonic() < deadline:
            time.sleep(.25)
        if usb('05c6', '9008') != ['3-2']:
            raise RuntimeError('EDL device did not appear on the established port')
        with (CAPTURE / 'worker.log').open('x') as log:
            r = subprocess.run(['/usr/bin/timeout', '--signal=TERM', '--kill-after=5s', str(WORKER_TIMEOUT) + 's', '/usr/bin/python3',
                                str(ROOT / 'Write-LaptopRomV1.py'), '--mode', 'install', '--output', str(CAPTURE / 'edl')],
                               stdout=log, stderr=subprocess.STDOUT, timeout=WORKER_TIMEOUT + 20)
        report['worker_exit'] = r.returncode; save()
        if r.returncode:
            raise RuntimeError('install stopped: keep USB connected, do not retry; inspect capture-rom-v1-install/edl/report.json')
        print('rom-v1 written and read back. Phone powered off. Press Power to boot LineageOS.', flush=True)
        code = 0
    except (Exception, KeyboardInterrupt) as e:
        report['error'] = str(e); print(str(e), flush=True)
    finally:
        if edl_requested:
            t0 = time.monotonic()
            while usb('05c6', '9008') and time.monotonic() - t0 < 30:
                time.sleep(.25)
            report['edl_gone'] = not usb('05c6', '9008')
        restored = not paused
        if paused:
            try:
                run(['/usr/bin/sudo', '-n', '/usr/local/sbin/a6l-host-control', 'resume'])
                restored = json.loads(run(['/usr/bin/sudo', '-n', '/usr/local/sbin/a6l-host-control', 'status'])['stdout'])['owned_pause'] is None
            except Exception as e:
                report.setdefault('cleanup_errors', []).append(str(e))
        report['services_restored'] = restored
        report['finished_utc'] = datetime.now(timezone.utc).isoformat(); save()
    print(json.dumps({k: report.get(k) for k in ('error', 'worker_exit', 'services_restored')}, indent=2))
    return 0 if code == 0 and report['services_restored'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
