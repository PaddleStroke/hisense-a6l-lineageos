#!/usr/bin/env python3
"""A6L rom-v1 RESTORE coordinator (agent flash). ATTENDED ONLY — start it through Launch-RomV1Restore.py.
Returns the phone to the exact pre-install state recorded in capture-rom-v1-install/edl (boot = Magisk stock boot,
dtbo, system, vendor, and misc/modemst1/modemst2/fsg/fsc/persist if the ROM changed them). userdata: head + stock FDE
footer zeroed (default) -> stock Android asks for a data wipe on its first boot (see docs/flash-20260924.md §6).
The phone must already be in EDL (9008) on port 3-2 — LineageOS/our kernel has no 'reboot edl' (see doc: how to enter EDL).
If adb from the ROM or the V74 recovery is available, this tool first tries 'adb reboot edl' (harmless if unsupported).
"""
from datetime import datetime, timezone
import json, os, pwd, socket, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CAPTURE = ROOT / 'capture-rom-v1-restore'
BACKUP = ROOT / 'capture-rom-v1-install' / 'edl'
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
    userdata = 'original' if '--userdata=original' in sys.argv else 'zero'
    if not (BACKUP / 'backup-manifest.json').exists():
        raise SystemExit('no install backup at ' + str(BACKUP))
    CAPTURE.mkdir(mode=0o700, exist_ok=False)
    report = {'scope': 'rom-v1 restore (userdata=%s)' % userdata, 'started_utc': datetime.now(timezone.utc).isoformat(), 'commands': []}
    paused = False
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
            raise RuntimeError('requires the desktop idle/suspend inhibitor (use Launch-RomV1Restore.py)')
        run(['/usr/bin/python3', str(ROOT / 'Verify-RomV1Stage.py'), '--restore'], timeout=900)
        state = json.loads(run(['/usr/bin/sudo', '-n', '/usr/local/sbin/a6l-host-control', 'status'])['stdout'])
        if state['owned_pause'] is not None:
            raise RuntimeError('an earlier host pause needs inspection/cleanup')
        paused = True
        run(['/usr/bin/sudo', '-n', '/usr/local/sbin/a6l-host-control', 'pause'])
        if not usb('05c6', '9008'):
            run(['/usr/bin/adb', 'reboot', 'edl'], required=False, timeout=10)
            print('Waiting up to 180 s for the phone in EDL (9008) on port 3-2 (see doc: entering EDL)...', flush=True)
            deadline = time.monotonic() + 180
            while not usb('05c6', '9008') and time.monotonic() < deadline:
                time.sleep(.5)
        if usb('05c6', '9008') != ['3-2']:
            raise RuntimeError('EDL device not present on the established port')
        with (CAPTURE / 'worker.log').open('x') as log:
            r = subprocess.run(['/usr/bin/timeout', '--signal=TERM', '--kill-after=5s', str(WORKER_TIMEOUT) + 's', '/usr/bin/python3',
                                str(ROOT / 'Write-LaptopRomV1.py'), '--mode', 'restore', '--backup', str(BACKUP), '--userdata', userdata,
                                '--output', str(CAPTURE / 'edl')], stdout=log, stderr=subprocess.STDOUT, timeout=WORKER_TIMEOUT + 20)
        report['worker_exit'] = r.returncode; save()
        if r.returncode:
            raise RuntimeError('restore stopped: keep USB connected; inspect capture-rom-v1-restore/edl/report.json')
        print('Restore written and read back; phone reset (stock boots; expect the data-wipe prompt, see doc).', flush=True)
        code = 0
    except (Exception, KeyboardInterrupt) as e:
        report['error'] = str(e); print(str(e), flush=True)
    finally:
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
