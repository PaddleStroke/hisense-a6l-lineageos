#!/usr/bin/env python3
"""A6L "preserve userdata" update worker (agent update-keepdata, 29 Sep 2026). ATTENDED ONLY, except --dry-run.
Started by Run-LaptopRomUpdate-v1.py (never by hand for a real run). Kit layout (laptop ~/A6L-usb-20260915/<kit>/):
  <kit>/*.py (rom-v1 tools + these), <kit>/images/ (new build + rom-v1-pins.json), <kit>/update/ (rom-update-compat.json of
  the new build, prev-rom-v1-pins.json + prev-rom-update-compat.json of the INSTALLED build, rom-update-tools.json, SHA256SUMS).
  --mode backup-only   rehearsal: all checks + full backup of boot/dtbo/vendor/system, no write, reset (--userdata-full
                       also copies the whole 107 GiB userdata: ~1.5-2 h, needs ~120 GB free)
  --mode update        hash-check installed images, write changed boot/dtbo/vendor/system ONLY, readback,
                       protected regions re-hashed, power off; no old partition snapshots by default
  --backup-policy snapshot  explicitly retain old partitions for capture rollback; backup-only uses this policy
  --mode rollback      write back the partitions saved by an update capture (--update-capture), keeps data, reset
  --transport edl | adb-recovery    (EDL entry from LineageOS is unproven; adb-recovery = V74 diagnostic recovery)
  --dry-run            offline: tool/image/compat hashes, compat rules, plan allowlist, guard self-test. No USB, no adb.
Never written in any mode: userdata, metadata, persist, modemst1/2, fsg, fsc, misc, recovery, vbmeta, devinfo, GPT.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import sys

HOME = Path(__file__).resolve().parent
sys.path.insert(0, str(HOME))
import RomFlashLayoutV1 as L
import RomFlashEngineV1 as E
import RomUpdateLayoutV1 as U
import RomUpdateEngineV1 as UE

ROM = HOME / 'images'
UPD = HOME / 'update'
ADB_RECOVERY_SERIAL = 'HLTE730T-PROBE'


def load_worker():
    spec = importlib.util.spec_from_file_location('a6l_rom_worker_v1', HOME / 'Write-LaptopRomV1.py')
    w = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(w)
    return w


W = load_worker()


def labelled(ranges):
    out = []
    for start, sectors in ranges:
        hit = U.partition_at(start, sectors)
        out.append((hit[0] if len(hit) == 1 else '?', start, sectors))
    return out


class UpdateGuard(W.Guard):
    """rom-v1 Firehose allowlist + the update allowlist: a plan (and every program XML) may only name ranges that start
    at boot/dtbo/vendor/system and stay inside them. Self-contained plan handling: kits staged before bug hunt round 2
    (e.g. kit-r5) carry a Write-LaptopRomV1.Guard without set_plan and with a self-registering program allowlist, so the
    plan is fixed and enforced HERE whatever the base version is."""
    def __init__(self):
        super().__init__()
        self.plan = None
        self.update_plan = None

    def set_plan(self, ranges):
        if self.update_plan is not None:
            raise ValueError('Program plan already fixed for this run')
        ranges = frozenset((int(a), int(b)) for a, b in ranges)
        U.check_update_plan(labelled(ranges))
        if hasattr(L, 'check_plan'):
            L.check_plan([('plan', a, b) for a, b in ranges])
        self.update_plan = ranges
        self.plan = ranges          # the fixed base Guard (bug hunt round 2) checks this too

    def check(self, data):
        tag = super().check(data)
        if tag == 'program':
            import xml.etree.ElementTree as ET
            n = ET.fromstring(data)[0]
            geometry = (int(n.get('start_sector')), int(n.get('num_partition_sectors')))
            if self.update_plan is None or geometry not in self.update_plan:
                raise ValueError('Program outside the fixed update plan: %s' % (geometry,))
            U.check_update_plan(labelled([geometry]))
        return tag


def sha_big(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 22), b''):
            h.update(b)
    return h.hexdigest()


def load_kit(verify_files=True):
    """-> images, prev_images, prev_compat, new_compat. Verifies tool pins and every data file hash."""
    if verify_files:
        tools = json.loads((UPD / 'rom-update-tools.json').read_text())
        for name, digest in tools['files'].items():
            if sha_big(HOME / name) != digest:
                raise E.StopRun('tool differs from rom-update-tools.json: ' + name)
        for line in (UPD / 'SHA256SUMS').read_text().splitlines():
            digest, name = line.split(None, 1)
            if sha_big(UPD / name.strip()) != digest:
                raise E.StopRun('update data differs: ' + name)
    pins = json.loads((ROM / 'rom-v1-pins.json').read_text())
    prev = json.loads((UPD / 'prev-rom-v1-pins.json').read_text())
    images = {n: (ROM / pins['images'][n]['file'], pins['images'][n]['bytes'], pins['images'][n]['sha256']) for n in U.UPDATE_WRITABLE}
    prev_images = {n: (None, prev['images'][n]['bytes'], prev['images'][n]['sha256']) for n in U.UPDATE_WRITABLE}
    new_compat = json.loads((UPD / 'rom-update-compat.json').read_text())
    prev_compat = json.loads((UPD / 'prev-rom-update-compat.json').read_text())
    for c, p in ((new_compat, pins), (prev_compat, prev)):
        if {n: c.get('images', {}).get(n) for n in U.UPDATE_WRITABLE} != {n: p['images'][n]['sha256'] for n in U.UPDATE_WRITABLE}:
            raise E.StopRun('compat file does not describe the pinned images (%s)' % c.get('system_fingerprint'))
    return images, prev_images, prev_compat, new_compat


def dry_run(mode, backup_policy=None):
    backup_policy = backup_policy or ('hash-only' if mode == 'update' else 'snapshot')
    result = {'dry_run': True, 'mode': mode, 'backup_policy': backup_policy, 'passed': False}
    try:
        if backup_policy == 'hash-only' and mode != 'update':
            raise E.StopRun('hash-only is for development updates')
        images, prev_images, prev_compat, new_compat = load_kit()
        UE.load_payloads(images)
        result['compat_problems'] = U.compat_problems(prev_compat, new_compat)
        result['prev'] = prev_compat.get('system_fingerprint')
        result['new'] = new_compat.get('system_fingerprint')
        writes = U.update_writes({k: v[1] for k, v in images.items()})
        result['plan'] = [list(w) for w in writes] if mode == 'update' else []
        result['plan_if_update'] = [list(w) for w in writes]
        result['rollback_plan'] = [list(w) for w in U.rollback_writes()]
        g = UpdateGuard()
        g.set_plan({(s, n) for _, s, n in writes})
        for name in U.NEVER_WRITE:
            try:
                UpdateGuard().set_plan({tuple(L.PARTITIONS[name])})
            except ValueError:
                continue
            raise AssertionError('update guard accepted a plan writing ' + name)
        g.programs = {tuple(L.PARTITIONS['userdata'])}
        try:
            g.check(L.program_xml(*L.PARTITIONS['userdata'])); raise AssertionError('guard accepted a userdata program')
        except ValueError:
            pass
        if result['compat_problems'] and mode != 'rollback':
            raise E.StopRun('new build cannot boot the existing /data: ' + '; '.join(result['compat_problems']))
        result['passed'] = True
    except Exception as e:   # noqa: BLE001 - reported
        result['error'] = repr(e)
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


def edl_device(report, save, extra_reads):
    """Same bring-up as Write-LaptopRomV1.main: kit hashes, one 9008 on port 3-2, exact Sahara identity before the loader."""
    kit = HOME.parent / 'a6l-recovery-kit'
    manifest = json.loads((kit / 'manifest.json').read_text())
    for name, digest in manifest['files'].items():
        if hashlib.sha256((kit / name).read_bytes()).hexdigest() != digest:
            raise ValueError('Recovery kit input differs: ' + name)
    if hashlib.sha256((kit / 'programmer.elf').read_bytes()).hexdigest() != W.LOADER_HASH:
        raise ValueError('Programmer hash differs')
    kit_expected = {'primary': (kit / 'expected-primary.bin').read_bytes(), 'tail': (kit / 'expected-tail.bin').read_bytes()}
    devices = []
    for item in Path('/sys/bus/usb/devices').glob('*'):
        try:
            if (item / 'idVendor').read_text().strip() == '05c6' and (item / 'idProduct').read_text().strip() == '9008':
                devices.append(item.name)
        except OSError:
            pass
    if devices != ['3-2']:
        raise ValueError('Expected exactly one EDL device, on the established physical port')
    sys.path[:0] = [str(kit / 'deps'), str(kit / 'edl')]
    sys.argv = ['edl.py', 'getstorageinfo', '--loader=' + str(kit / 'programmer.elf'), '--memory=eMMC', '--vid=05c6', '--pid=9008']
    spec = importlib.util.spec_from_file_location('a6l_edl', kit / 'edl/edl.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    original_sahara = module.sahara

    class ExactSpare(original_sahara):
        def upload_loader(self, version):
            identity = {'serial': self.serials, 'hwid': self.hwidstr, 'pkhash': self.pkhash}
            report['sahara'] = identity
            save()
            if identity != W.IDENTITY:
                raise ValueError('Hardware identity differs; no programmer will be uploaded')
            return super().upload_loader(version=version)

    module.sahara = ExactSpare
    from edlclient.Library.firehose import firehose
    from edlclient.Library.xmlparser import xmlparser
    guard = UpdateGuard()
    guard.reads |= set(extra_reads)
    original_xml, original_parser, last = firehose.xmlsend, xmlparser.getresponse, {}

    def capture_response(self, data):
        result = original_parser(self, data)
        last.clear(); last.update(result)
        return result

    xmlparser.getresponse = capture_response

    def guarded_xml(self, data, *a, **k):
        try:
            guard.check(data)
        except ValueError:
            report['blocked_xml_not_sent'] = data
            save()
            raise
        report['operations'] = report.get('operations', 0) + 1
        report['last_xml'] = data
        return original_xml(self, data, *a, **k)

    firehose.xmlsend = guarded_xml
    app = module.main(module.args)
    rc = app.run()
    if rc != 0 or app.fh is None or not app.fh.connected:
        raise RuntimeError('Firehose connection failed')
    fh = app.fh.firehose
    if fh.cfg.SECTOR_SIZE_IN_BYTES != 512 or fh.cfg.MemoryName.lower() != 'emmc':
        raise ValueError('Unexpected storage geometry')
    return W.FirehoseDevice(fh, guard, last, report, save), guard, kit_expected, app


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mode', choices=('update', 'backup-only', 'rollback'), required=True)
    ap.add_argument('--transport', choices=('edl', 'adb-recovery'), default='edl')
    ap.add_argument('--output', type=Path)
    ap.add_argument('--update-capture', type=Path, help='rollback: capture-rom-update-update-*/edl')
    ap.add_argument('--userdata-full', action='store_true')
    ap.add_argument('--reflash', action='store_true')
    ap.add_argument('--backup-policy', choices=('hash-only', 'snapshot'),
                    help='development updates default to hashes, without old partition snapshots')
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()
    policy = args.backup_policy or ('hash-only' if args.mode == 'update' and not args.userdata_full else 'snapshot')
    if args.dry_run:
        if policy == 'hash-only' and args.userdata_full:
            ap.error('--userdata-full requires snapshot policy')
        return dry_run(args.mode, policy)
    import pwd, socket
    try:
        uid = pwd.getpwnam('pierrelouis').pw_uid
    except KeyError:
        uid = None
    if os.geteuid() != uid or socket.gethostname().split('.')[0] != 'system76-pc':
        ap.error('This bounded worker requires the configured user on the established laptop')
    if args.output is None or not args.output.resolve().parent.name.startswith('capture-rom-update-' + args.mode + '-') \
            or args.output.name != 'edl':
        ap.error('Output must be capture-rom-update-<mode>-<stamp>/edl')
    if args.userdata_full and args.mode == 'rollback':
        ap.error('--userdata-full is for backup-only/update')
    os.umask(0o022)
    args.output.mkdir(mode=0o700, exist_ok=False)
    report = {'scope': 'rom-update ' + args.mode + ' via ' + args.transport, 'started_utc': datetime.now(timezone.utc).isoformat()}
    path = args.output / 'report.json'

    def save():
        tmp = path.with_suffix('.json.tmp'); tmp.write_text(json.dumps(report, indent=2) + '\n'); tmp.replace(path)

    bound = 10800 if args.userdata_full else 5400

    def deadline(signum, frame):
        raise TimeoutError('update worker reached its %d-second bound' % bound)

    signal.signal(signal.SIGALRM, deadline)
    signal.alarm(bound)
    app = None
    try:
        images, prev_images, prev_compat, new_compat = load_kit()
        save()
        writes = U.update_writes({k: v[1] for k, v in images.items()})
        plan = set() if args.mode == 'backup-only' else (
            {(s, n) for _, s, n in writes} if args.mode == 'update' else {(s, n) for _, s, n in U.rollback_writes()})
        report['program_plan'] = sorted(plan)
        save()
        if args.transport == 'edl':
            extra = {(s, n) for _, s, n in writes} | set(U.invariant_regions().values()) | {U.userdata_full()}
            dev, guard, kit_expected, app = edl_device(report, save, extra)
            if plan:
                guard.set_plan(plan)
        else:
            import RomUpdateAdbV1 as A
            kit = HOME.parent / 'a6l-recovery-kit'
            kit_expected = {'primary': (kit / 'expected-primary.bin').read_bytes(), 'tail': (kit / 'expected-tail.bin').read_bytes()}
            dev = A.AdbRecoveryDevice(ADB_RECOVERY_SERIAL)
            report['adb_disk'] = dev.preflight()
            save()
        if args.mode == 'rollback':
            if args.update_capture is None or not args.update_capture.resolve().parent.name.startswith('capture-rom-update-update-'):
                raise ValueError('--update-capture must be capture-rom-update-update-*/edl')
            UE.run_rollback(dev, args.output, args.update_capture, kit_expected, report, save)
        else:
            UE.run_update(dev, args.output, images, prev_images, prev_compat, new_compat, kit_expected, report, save,
                          mode=args.mode, userdata_full=args.userdata_full, reflash=args.reflash, backup_policy=policy)
    except (Exception, SystemExit) as error:
        report['error'] = repr(error)
        print('rom-update stopped: ' + repr(error), flush=True)
    finally:
        signal.alarm(0)
        if app is not None and getattr(app, 'cdc', None) is not None and app.cdc.connected:
            app.cdc.close()
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
    keys = ('scope', 'error', 'predecessor', 'compat', 'readback_verified', 'invariants_unchanged', 'rehearsal_passed', 'power')
    print(json.dumps({k: report.get(k) for k in keys}, indent=2))
    ok = report.get('power') and not report.get('error') and (report.get('readback_verified') or report.get('rehearsal_passed'))
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
