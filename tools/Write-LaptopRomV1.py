#!/usr/bin/env python3
"""A6L rom-v1 EDL worker (agent flash, 24 Sep 2026). ATTENDED ONLY. Started by Run-LaptopRomInstall-v1.py /
Run-LaptopRomRestore-v1.py, never by hand. Exact spare (Sahara serial/hwid/pkhash) and GPT checks precede any write.
Every Firehose XML is checked against the fixed plan (reads of known regions, programs of exactly the planned ranges,
power off/reset only after all readbacks). No retry after any failure.
  --mode install   backup + write boot/dtbo/vendor/system + zero userdata head, readback, power off
  --mode restore   write back the install backup (see docs/flash-20260924.md), readback, reset
"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import pwd
import signal
import socket
import sys
import xml.etree.ElementTree as ET

HOME = Path(__file__).resolve().parent
sys.path.insert(0, str(HOME))
import RomFlashLayoutV1 as L
import RomFlashEngineV1 as E

LOADER_HASH = '6003242582a610712c6b32c8f09475fb78a166e4bb3b018e9f01a7b9bb083642'
PKHASH = '06a0604b3069cca35fd538f8ca5a8fc5d07c90a6e755f8cfacc1426cb9d75d22'
IDENTITY = {'serial': 'f17933a2', 'hwid': '0008c0e100430000', 'pkhash': PKHASH}
CONFIGURATION_PROBE = (1, 1)   # sector 1 x 1 (the library's "read first storage sector"), as (start, sectors)
DEADLINE_S = 5400
ROM = HOME / 'images'          # staged layout: ~/A6L-usb-20260915/rom-v1/{tools, images/, captures}


def allowed_reads(extra=()):
    regions = {L.GPT_PRIMARY, L.GPT_TAIL, CONFIGURATION_PROBE, (0, 1)}
    regions |= set(L.backup_regions().values())
    regions |= set(L.PARTITIONS.values())
    regions |= set(extra)
    return regions


class Guard:
    """Firehose XML allowlist for one run. The plan's program ranges are registered before any write."""
    def __init__(self):
        self.reads = allowed_reads()
        self.programs = set()
        self.power = None

    def check(self, data):
        root = ET.fromstring(data)
        if root.tag != 'data' or len(root) != 1:
            raise ValueError('Expected exactly one Firehose operation')
        node = root[0]
        tag = node.tag
        if tag in ('nop', 'configure', 'getstorageinfo'):
            if tag == 'configure' and node.get('MemoryName', '').lower() != 'emmc':
                raise ValueError('Only eMMC configuration is allowed')
            return tag
        if tag in ('read', 'program'):
            if node.get('SECTOR_SIZE_IN_BYTES') != '512' or node.get('physical_partition_number') != '0' or len(node):
                raise ValueError('Unexpected sector size / LUN')
            geometry = (int(node.get('start_sector', '-1')), int(node.get('num_partition_sectors', '-1')))
            if tag == 'read' and geometry not in self.reads:
                raise ValueError('Read outside the planned regions: %s' % (geometry,))
            if tag == 'program':
                if geometry not in self.programs:
                    raise ValueError('Program outside the registered plan: %s' % (geometry,))
                if set(node.attrib) != {'SECTOR_SIZE_IN_BYTES', 'num_partition_sectors', 'physical_partition_number', 'start_sector'}:
                    raise ValueError('Unexpected program attributes')
            return tag
        if tag == 'power':
            if node.attrib != {'value': self.power} or len(node) or self.power is None:
                raise ValueError('Power action not permitted now')
            return tag
        raise ValueError('Persistent or unsupported Firehose operation: ' + tag)


class FirehoseDevice:
    def __init__(self, fh, guard, last_response, report, save):
        self.fh, self.guard, self.last, self.report, self.save = fh, guard, last_response, report, save
        self.chunk = fh.cfg.MaxPayloadSizeToTargetInBytes
        if type(self.chunk) is not int or not 512 <= self.chunk <= 1048576 or self.chunk % 512:
            raise E.StopRun('Unexpected negotiated transfer size')

    def read(self, start, sectors, out):
        self.last.clear()
        if not self.fh.cmd_read(0, start, sectors, str(out), display=False):
            raise E.StopRun('Read not acknowledged: %d+%d' % (start, sectors))
        if self.last.get('value') != 'ACK' or self.last.get('rawmode') != 'false':
            raise E.StopRun('Final read acknowledgment missing: %d+%d' % (start, sectors))

    def program(self, start, sectors, stream):
        self.guard.programs = {(start, sectors)}
        cdc = self.fh.cdc

        def strict(data):
            written = cdc.EP_OUT.write(data, timeout=5000)
            if type(written) is not int or written != len(data):
                raise IOError('Short USB write: expected %d, got %r' % (len(data), written))

        response = self.fh.xmlsend(L.program_xml(start, sectors))
        attributes = response.data if isinstance(response.data, dict) else self.fh.xml.getresponse(response.data)
        if not response.resp or attributes.get('value') != 'ACK' or attributes.get('rawmode') != 'true':
            raise E.StopRun('Programming raw mode was not explicitly acknowledged')
        sent = 0
        pending = b''
        for block in stream:
            pending += block
            while len(pending) >= self.chunk:
                strict(pending[:self.chunk]); strict(b''); sent += self.chunk
                pending = pending[self.chunk:]
        if pending:
            strict(pending); strict(b''); sent += len(pending)
        if sent != sectors * L.SECTOR:
            raise E.StopRun('Program length mismatch')
        final = self.fh.xml.getresponse(self.fh.wait_for_data())
        if final.get('value') != 'ACK' or final.get('rawmode') != 'false':
            raise E.StopRun('Final programming acknowledgment is missing or unsuccessful')
        self.guard.programs = set()

    def power(self, value):
        self.guard.power = value
        response = self.fh.xmlsend('<?xml version="1.0" ?><data><power value="%s"/></data>' % value)
        attributes = response.data if isinstance(response.data, dict) else self.fh.xml.getresponse(response.data)
        self.guard.power = None
        if not (bool(response.resp) and attributes.get('value') == 'ACK'):
            raise E.StopRun('Power action was not explicitly acknowledged')


def load_images():
    pins = json.loads((ROM / 'rom-v1-pins.json').read_text())
    images = {}
    for name in ('boot', 'dtbo', 'vendor', 'system'):
        p = pins['images'][name]
        images[name] = (ROM / p['file'], p['bytes'], p['sha256'])
    return images, pins


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('install', 'restore'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--backup', type=Path, help='restore: the install capture edl directory')
    parser.add_argument('--userdata', choices=('zero', 'original'), default='zero')
    parser.add_argument('--allow-nonstock', action='store_true')
    args = parser.parse_args()
    if os.geteuid() != pwd.getpwnam('pierrelouis').pw_uid or socket.gethostname().split('.')[0] != 'system76-pc':
        parser.error('This bounded worker requires the configured user on the established laptop')
    capture = 'capture-rom-v1-install' if args.mode == 'install' else 'capture-rom-v1-restore'
    if args.output.resolve().parent.name != capture or args.output.name != 'edl':
        parser.error('Output must be <capture>/edl of the fixed capture directory')
    kit = HOME.parent / 'a6l-recovery-kit'
    os.umask(0o022)
    args.output.mkdir(mode=0o700, exist_ok=False)
    report = {'scope': 'rom-v1 ' + args.mode, 'started_utc': datetime.now(timezone.utc).isoformat(), 'operations': 0}
    path = args.output / 'report.json'

    def save():
        tmp = path.with_suffix('.json.tmp'); tmp.write_text(json.dumps(report, indent=2) + '\n'); tmp.replace(path)

    def deadline(signum, frame):
        raise TimeoutError('EDL worker reached its %d-second bound' % DEADLINE_S)

    signal.signal(signal.SIGALRM, deadline)
    signal.alarm(DEADLINE_S)
    app = None
    try:
        images, pins = load_images()
        save()
        manifest = json.loads((kit / 'manifest.json').read_text())
        for name, digest in manifest['files'].items():
            if hashlib.sha256((kit / name).read_bytes()).hexdigest() != digest:
                raise ValueError('Recovery kit input differs: ' + name)
        if hashlib.sha256((kit / 'programmer.elf').read_bytes()).hexdigest() != LOADER_HASH:
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
                if identity != IDENTITY:
                    raise ValueError('Hardware identity differs; no programmer will be uploaded')
                return super().upload_loader(version=version)

        module.sahara = ExactSpare
        from edlclient.Library.firehose import firehose
        from edlclient.Library.xmlparser import xmlparser
        guard = Guard()
        original_xml = firehose.xmlsend
        original_parser = xmlparser.getresponse
        last = {}

        def capture_response(self, data):
            result = original_parser(self, data)
            last.clear(); last.update(result)
            return result

        xmlparser.getresponse = capture_response

        def guarded_xml(self, data, *a, **k):
            try:
                tag = guard.check(data)
            except ValueError:
                report['blocked_xml_not_sent'] = data
                save()
                raise
            report['operations'] += 1
            report['last_xml'] = data
            return original_xml(self, data, *a, **k)

        firehose.xmlsend = guarded_xml
        os.chdir(args.output)
        app = module.main(module.args)
        rc = app.run()
        if rc != 0 or app.fh is None or not app.fh.connected:
            raise RuntimeError('Firehose connection failed')
        fh = app.fh.firehose
        if fh.cfg.SECTOR_SIZE_IN_BYTES != 512 or fh.cfg.MemoryName.lower() != 'emmc':
            raise ValueError('Unexpected storage geometry')
        dev = FirehoseDevice(fh, guard, last, report, save)
        # readbacks re-read exactly the programmed ranges: allow those reads too
        guard.reads |= {(st, n) for _, st, n in L.install_writes({k: v[1] for k, v in images.items()})}
        guard.reads |= {(st, n) for _, st, n in L.restore_writes(set(L.ROM_MAY_WRITE))}
        if args.mode == 'install':
            E.run_install(dev, args.output, images, pins['stock'] | {'rom-' + k: v['sha256_partition'] for k, v in pins['images'].items() if 'sha256_partition' in v},
                          kit_expected, report, save, allow_nonstock=args.allow_nonstock)
        else:
            if args.backup is None or args.backup.resolve().parent.name != 'capture-rom-v1-install':
                raise ValueError('--backup must be capture-rom-v1-install/edl')
            E.run_restore(dev, args.output, args.backup, kit_expected, report, save, userdata=args.userdata)
    except (Exception, SystemExit) as error:
        report['error'] = repr(error)
        print('rom-v1 operation stopped: ' + repr(error), flush=True)
    finally:
        signal.alarm(0)
        if app is not None and getattr(app, 'cdc', None) is not None and app.cdc.connected:
            app.cdc.close()
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
    print(json.dumps({k: report.get(k) for k in ('scope', 'error', 'readback_verified', 'power', 'predecessor', 'rom_changed')}, indent=2))
    return 0 if report.get('readback_verified') and report.get('power') and not report.get('error') else 1


if __name__ == '__main__':
    raise SystemExit(main())
