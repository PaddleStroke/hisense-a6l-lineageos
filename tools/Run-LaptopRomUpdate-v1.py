#!/usr/bin/env python3
"""A6L "preserve userdata" update coordinator (agent update-keepdata, 29 Sep 2026). ATTENDED ONLY for a real run: start it
through Launch-RomUpdateV1.py. --dry-run is offline (no adb, no USB) and may run any time.

usage (in the kit dir on the laptop):
  python3 Run-LaptopRomUpdate-v1.py --mode backup-only --transport adb-recovery --dry-run    # offline, prints the plan
  python3 Launch-RomUpdateV1.py --mode backup-only --transport adb-recovery [--enter-recovery]  # rehearsal, no write
  python3 Launch-RomUpdateV1.py --mode update --transport adb-recovery [--enter-recovery]
  python3 Launch-RomUpdateV1.py --mode rollback --transport adb-recovery --update-capture capture-rom-update-update-<t>/edl
  --transport edl [--already-in-edl]: EDL/Firehose (entry from LineageOS via `adb reboot edl` is UNPROVEN on our kernel;
                                      --already-in-edl = Pierre put the phone in EDL by another way)
Steps: worker --dry-run (tool/image/compat hashes, compat rules, plan allowlist) -> phone state (LineageOS fingerprint
= the installed build, battery) -> transport entry -> worker (backup, [write boot/dtbo/vendor/system only], readback,
protected regions re-hashed) -> host resume. Capture: capture-rom-update-<mode>-<UTC>/ (never reused, never deleted).

wipe-data (user-build-20261012/wipe-mode; test-keys <-> release-keys switch, ERASES /data):
  python3 Launch-RomUpdateV1.py --mode update --wipe-data --confirm-wipe <LineageOS serial> --transport adb-recovery \
          --backup-policy hash-only --enter-recovery
  Extra coordinator checks: --confirm-wipe must equal the expected LineageOS serial; the run must START in LineageOS
  (exactly one adb device, that serial, state 'device', running fingerprint/date = the kit's predecessor, battery >= 50 %)
  with no diagnostic recovery already on adb, so the recovery that appears is that phone; in the recovery, the
  battery capacity (when the recovery kernel exposes it) must be >= 50 % and androidboot.serialno (when present on its
  cmdline) must equal the same serial. The worker then runs with --wipe-data.
  Resume after a wipe run that stopped with the phone still in the recovery (images read back, zeroing not finished):
  ... --wipe-data --confirm-wipe <serial> --reflash --from-recovery (instead of --enter-recovery). Then exactly one adb
  device must be connected, the diagnostic recovery, and its cmdline serialno and battery capacity become REQUIRED
  (present, = serial, >= 50 %); the worker's --reflash only accepts partitions holding the previous or the new images.
"""
import argparse
from datetime import datetime, timezone
import json, os, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
LINEAGE_SERIAL = '1e529013'          # ro.serialno from the ABL (same as stock; confirm at the first attended run)
RECOVERY_SERIAL = 'HLTE730T-PROBE'   # V74 diagnostic recovery adbd
ADB = '/usr/bin/adb'


def usb(vid, pid):
    found = []
    for item in Path('/sys/bus/usb/devices').glob('*'):
        try:
            if (item / 'idVendor').read_text().strip() == vid and (item / 'idProduct').read_text().strip() == pid:
                found.append(item.name)
        except OSError:
            pass
    return found


def worker_argv(a, output=None, dry=False):
    argv = ['/usr/bin/python3', str(ROOT / 'Write-LaptopRomUpdateV1.py'), '--mode', a.mode, '--transport', a.transport]
    if a.backup_policy:
        argv += ['--backup-policy', a.backup_policy]
    if a.wipe_data:
        argv.append('--wipe-data')
    if dry:
        return argv + ['--dry-run']
    argv += ['--output', str(output)]
    if a.update_capture:
        argv += ['--update-capture', str(a.update_capture)]
    if a.userdata_full:
        argv.append('--userdata-full')
    if a.reflash:
        argv.append('--reflash')
    return argv


def wipe_args_error(a):
    """None, or why the wipe-data arguments are refused (checked before anything else, dry run included)."""
    if not a.wipe_data:
        return '--confirm-wipe/--from-recovery without --wipe-data' if a.confirm_wipe or a.from_recovery else None
    if a.transport != 'adb-recovery' or a.mode not in ('update', 'backup-only'):
        return '--wipe-data needs --transport adb-recovery and --mode update|backup-only'
    if a.mode == 'update':
        if a.confirm_wipe != LINEAGE_SERIAL:
            return '--wipe-data --mode update erases /data: pass --confirm-wipe %s (the expected phone serial)' % LINEAGE_SERIAL
        if a.from_recovery and (a.enter_recovery or not a.reflash):
            return '--from-recovery is the resume of a stopped wipe: with --reflash, without --enter-recovery'
        if not a.enter_recovery and not a.from_recovery:
            return '--wipe-data --mode update must start from LineageOS: pass --enter-recovery'
        if a.backup_policy not in (None, 'hash-only') or a.userdata_full:
            return '--wipe-data updates are hash-only'
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mode', choices=('update', 'backup-only', 'rollback'), required=True)
    ap.add_argument('--transport', choices=('edl', 'adb-recovery'), required=True)
    ap.add_argument('--already-in-edl', action='store_true')
    ap.add_argument('--enter-recovery', action='store_true', help='adb-recovery: `adb reboot recovery` from LineageOS first')
    ap.add_argument('--update-capture', type=Path)
    ap.add_argument('--userdata-full', action='store_true')
    ap.add_argument('--reflash', action='store_true')
    ap.add_argument('--backup-policy', choices=('hash-only', 'snapshot'),
                    help='update defaults to hashes without development snapshots; backup-only retains snapshots')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--wipe-data', action='store_true', help='ERASES /data: see the docstring (wipe-data)')
    ap.add_argument('--confirm-wipe', metavar='SERIAL', help='with --wipe-data --mode update: the expected phone serial')
    ap.add_argument('--from-recovery', action='store_true', help='with --wipe-data --reflash: resume a stopped wipe run')
    a = ap.parse_args()
    err = wipe_args_error(a)
    if err:
        ap.error(err)
    wipe = a.wipe_data and a.mode == 'update'
    if a.dry_run:
        r = subprocess.run(worker_argv(a, dry=True), capture_output=True, text=True)
        print(r.stdout, end='')
        print(json.dumps({'would': ['check LineageOS state (%s, fingerprint = installed build, battery >= 50 %%)' % LINEAGE_SERIAL,
                                    'enter ' + ('EDL' if a.transport == 'edl' else 'the V74 diagnostic recovery (%s)' % RECOVERY_SERIAL),
                                    'run the worker: ' + ' '.join(worker_argv(a, 'capture-rom-update-%s-<utc>/edl' % a.mode)[1:])]
                          + (['WIPE: zero the first MiB of metadata and userdata of %s after the images are read back' % LINEAGE_SERIAL]
                             if wipe else [])}, indent=2))
        return r.returncode
    import pwd, socket
    assert os.geteuid() != 0 and pwd.getpwuid(os.getuid()).pw_name == 'pierrelouis'
    assert socket.gethostname().split('.')[0] == 'system76-pc'
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    capture = ROOT / f'capture-rom-update-{a.mode}-{stamp}'
    capture.mkdir(mode=0o700, exist_ok=False)
    report = {'scope': f'rom-update {a.mode} via {a.transport}', 'started_utc': datetime.now(timezone.utc).isoformat(),
              'commands': [], 'services_restored': False}
    paused = False
    code = 1

    def save():
        t = capture / 'session.json.tmp'; t.write_text(json.dumps(report, indent=2) + '\n'); t.replace(capture / 'session.json')

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

    def adb_serials():
        out = run([ADB, 'devices'], required=False)['stdout']
        # adb lists the V74/V75 diagnostic recovery as state 'recovery', LineageOS as 'device' (fix 30 Sep: 'recovery' was ignored)
        return [l.split()[0] for l in out.splitlines()[1:] if len(l.split()) >= 2 and l.split()[1] in ('device', 'recovery')]

    try:
        if not all(w in run(['/usr/bin/gnome-session-inhibit', '--list'])['stdout'] for w in ['A6L-rom-update', 'suspend', 'idle']):
            raise RuntimeError('requires the desktop idle/suspend inhibitor (use Launch-RomUpdateV1.py)')
        dry = run(worker_argv(a, dry=True), required=False, timeout=900)
        report['dry_run'] = json.loads(dry['stdout']) if dry['stdout'].strip().startswith('{') else dry['stdout']
        if dry['exit'] != 0:
            raise RuntimeError('worker dry run failed: see session.json dry_run')
        need = 12 + (120 if a.userdata_full else 0)
        st = os.statvfs(ROOT); free = st.f_bavail * st.f_frsize / 1e9
        report['free_gb'] = free
        if free < need:
            raise RuntimeError(f'need >= {need} GB free for the backup')
        compat = {k: json.loads((ROOT / 'update' / f).read_text()) for k, f in (('prev', 'prev-rom-update-compat.json'), ('new', 'rom-update-compat.json'))}
        serials = adb_serials()
        report['adb_serials'] = serials
        in_lineage = LINEAGE_SERIAL in serials
        if wipe:
            report['wipe_data'] = True
            listed = [l.split() for l in run([ADB, 'devices'], required=False)['stdout'].splitlines()[1:] if l.strip()]
            report['adb_devices_all'] = listed
            want = [[RECOVERY_SERIAL, 'recovery']] if a.from_recovery else [[LINEAGE_SERIAL, 'device']]
            if listed != want:
                raise RuntimeError('wipe-data: exactly one adb device must be connected: %s (got %s)' % (want, listed))
        if in_lineage:
            fp = run([ADB, '-s', LINEAGE_SERIAL, 'shell', 'getprop', 'ro.system.build.fingerprint'], timeout=5)['stdout'].strip()
            report['running_fingerprint'] = fp
            date = run([ADB, '-s', LINEAGE_SERIAL, 'shell', 'getprop', 'ro.system.build.date.utc'], timeout=5)['stdout'].strip()
            report['running_build_date_utc'] = date
            # eng builds share one fingerprint (eng.root): the build date tells r5 from r6. The partition hashes read by
            # the worker are the real identity check; this only stops early with the phone untouched.
            if a.mode != 'rollback' and (fp, date) != (compat['prev']['system_fingerprint'], compat['prev']['build_date_utc']):
                raise RuntimeError('running build %r/%s is not the installed build the kit expects (%r/%s)' % (
                    fp, date, compat['prev']['system_fingerprint'], compat['prev']['build_date_utc']))
            bat = run([ADB, '-s', LINEAGE_SERIAL, 'shell', 'dumpsys', 'battery'])['stdout']
            m = re.search(r'^\s*level:\s*(\d+)\s*$', bat, re.M)
            if not m or int(m.group(1)) < 50:
                raise RuntimeError('battery must be >= 50 %')
        if a.transport == 'edl':
            if not a.already_in_edl:
                if not in_lineage:
                    raise RuntimeError('phone not in LineageOS with adb (%s); use --already-in-edl if it is in EDL' % LINEAGE_SERIAL)
                if (Path('/sys/bus/usb/devices/3-2') / 'serial').read_text().strip() != LINEAGE_SERIAL:
                    raise RuntimeError('phone is not on the established physical port 3-2')
            state = json.loads(run(['/usr/bin/sudo', '-n', '/usr/local/sbin/a6l-host-control', 'status'])['stdout'])
            if state['owned_pause'] is not None:
                raise RuntimeError('an earlier host pause needs inspection/cleanup')
            paused = True
            run(['/usr/bin/sudo', '-n', '/usr/local/sbin/a6l-host-control', 'pause'])
            if not a.already_in_edl:
                run([ADB, '-s', LINEAGE_SERIAL, 'reboot', 'edl'], required=False)
                t0 = time.monotonic()
                while not usb('05c6', '9008') and time.monotonic() - t0 < 45:
                    time.sleep(.25)
            if usb('05c6', '9008') != ['3-2']:
                raise RuntimeError('no EDL device on port 3-2 (EDL entry from LineageOS is unproven: the kernel may have '
                                   'rebooted normally). Use --transport adb-recovery, or --already-in-edl after a hardware EDL entry')
        else:
            if RECOVERY_SERIAL not in serials:
                if not (a.enter_recovery and in_lineage):
                    raise RuntimeError('phone not in the V74 diagnostic recovery (%s); boot it (fastboot menu -> Recovery) '
                                       'or pass --enter-recovery from LineageOS' % RECOVERY_SERIAL)
                run([ADB, '-s', LINEAGE_SERIAL, 'reboot', 'recovery'], required=False)
                t0 = time.monotonic()
                while RECOVERY_SERIAL not in adb_serials() and time.monotonic() - t0 < 150:
                    time.sleep(2)
                if RECOVERY_SERIAL not in adb_serials():
                    raise RuntimeError('V74 recovery adb did not appear within 150 s')
            img = run([ADB, '-s', RECOVERY_SERIAL, 'shell', "/system/bin/toybox cat /proc/device-tree/chosen/hisense,a6l-image"],
                      required=False)['stdout'].strip('\x00\r\n ')
            report['recovery_image'] = img
            if wipe:
                if adb_serials() != [RECOVERY_SERIAL]:
                    raise RuntimeError('wipe-data: expected only %s on adb after the reboot' % RECOVERY_SERIAL)
                cmdline = run([ADB, '-s', RECOVERY_SERIAL, 'shell', '/system/bin/toybox cat /proc/cmdline'])['stdout']
                m = re.search(r'(?:^|\s)androidboot\.serialno=(\S+)', cmdline)
                report['recovery_serialno'] = m.group(1) if m else None
                if a.from_recovery and not m:
                    raise RuntimeError('wipe-data --from-recovery: no androidboot.serialno on the recovery cmdline')
                if m and m.group(1) != LINEAGE_SERIAL:
                    raise RuntimeError('wipe-data: recovery androidboot.serialno %s != %s' % (m.group(1), LINEAGE_SERIAL))
                cap = run([ADB, '-s', RECOVERY_SERIAL, 'shell', '/system/bin/toybox cat /sys/class/power_supply/battery/capacity'],
                          required=False)['stdout'].strip()
                report['recovery_battery'] = int(cap) if cap.isdigit() else None
                if a.from_recovery and not cap.isdigit():
                    raise RuntimeError('wipe-data --from-recovery: battery capacity not readable in the recovery')
                if cap.isdigit() and int(cap) < 50:
                    raise RuntimeError('wipe-data: battery %s %% in recovery (needs >= 50 %%)' % cap)
                save()
        bound = 10900 if a.userdata_full else 5500
        with (capture / 'worker.log').open('x') as log:
            r = subprocess.run(['/usr/bin/timeout', '--signal=TERM', '--kill-after=5s', f'{bound}s'] + worker_argv(a, capture / 'edl'),
                               stdout=log, stderr=subprocess.STDOUT, timeout=bound + 20)
        report['worker_exit'] = r.returncode; save()
        if r.returncode:
            raise RuntimeError(f'{a.mode} stopped: keep USB connected, do not retry; inspect {capture.name}/edl/report.json')
        print({'backup-only': 'Rehearsal passed, nothing written. Phone reset.',
               'update': ('Update written and read back, metadata/userdata heads zeroed and read back, other protected regions '
                          'unchanged. Phone off: press Power; the first boot formats /metadata and /data (longer boot).')
                         if wipe else 'Update written and read back, protected regions unchanged. Phone off: press Power.',
               'rollback': 'Previous build written back and read back. Phone reset.'}[a.mode], flush=True)
        code = 0
    except (Exception, KeyboardInterrupt) as e:
        report['error'] = str(e); print(str(e), flush=True)
    finally:
        restored = not paused
        if paused:
            t0 = time.monotonic()
            while usb('05c6', '9008') and time.monotonic() - t0 < 30:
                time.sleep(.25)
            try:
                run(['/usr/bin/sudo', '-n', '/usr/local/sbin/a6l-host-control', 'resume'])
                restored = json.loads(run(['/usr/bin/sudo', '-n', '/usr/local/sbin/a6l-host-control', 'status'])['stdout'])['owned_pause'] is None
            except Exception as e:   # noqa: BLE001
                report.setdefault('cleanup_errors', []).append(str(e))
        report['services_restored'] = restored
        report['finished_utc'] = datetime.now(timezone.utc).isoformat(); save()
    print(json.dumps({k: report.get(k) for k in ('error', 'worker_exit', 'services_restored')}, indent=2))
    return 0 if code == 0 and report['services_restored'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
