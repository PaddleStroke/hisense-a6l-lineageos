#!/usr/bin/env python3
"""Install one fixed diagnostic recovery or restore the fixed stock recovery.

Exact spare and GPT checks precede one bounded write. The full target and other
fixed regions are read back before poweroff (install) or normal reset (restore).
Restore accepts any pre-existing recovery bytes, preserving them in the capture.
"""
import argparse
from RecoveryTransitionV68 import verify_transition, RECOVERIES
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
from DiagnosticRecoveryProtocolV68 import check_program_xml, program_recovery, verify_payload, TARGETS

REGIONS = {
    'primary': (0, 1048576), 'tail': (125074122752, 22528),
    'recovery': (469762048, 67108864), 'devinfo': (253755392, 4096),
    'misc-bcb': (8757706752, 4096), 'vbmeta': (291811328, 65536),
}
RECOVERY_HASH = '9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621'
LOADER_HASH = '6003242582a610712c6b32c8f09475fb78a166e4bb3b018e9f01a7b9bb083642'
PKHASH = '06a0604b3069cca35fd538f8ca5a8fc5d07c90a6e755f8cfacc1426cb9d75d22'
CONFIGURATION_PROBE = (512, 512)


def edl_arguments(kit):
    # Imported mode returns after connection instead of dispatching this nominal
    # read-only command. Its documented grammar accepts all required options.
    return ['edl.py', 'getstorageinfo', '--loader=' + str(kit / 'programmer.elf'),
            '--memory=eMMC', '--vid=05c6', '--pid=9008']


def create_edl_app(module):
    # This library uses an EMPTY imported argument to select imported mode.
    return module.main(module.args)


def allowed_xml(data):
    root = ET.fromstring(data)
    if root.tag != 'data' or len(root) != 1:
        raise ValueError('Expected exactly one Firehose operation')
    node = root[0]
    if node.tag == 'program':
        return check_program_xml(data)
    if node.tag in ('nop', 'configure', 'getstorageinfo'):
        if node.tag == 'configure' and node.get('MemoryName', '').lower() != 'emmc':
            raise ValueError('Only eMMC configuration is allowed')
    elif node.tag == 'power':
        if node.attrib not in ({'value': 'reset'}, {'value': 'off'}) or len(node):
            raise ValueError('Only a normal reset or poweroff is allowed')
    elif node.tag == 'read':
        geometry = (int(node.get('start_sector', '-1')) * 512,
                    int(node.get('num_partition_sectors', '-1')) * 512)
        if (node.get('SECTOR_SIZE_IN_BYTES') != '512'
                or node.get('physical_partition_number') != '0'
                or geometry not in [*REGIONS.values(), CONFIGURATION_PROBE]):
            raise ValueError('Read outside the fixed recovery-check regions')
    else:
        raise ValueError('Persistent or unsupported Firehose operation: ' + node.tag)
    return node.tag


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--mode', choices=tuple(TARGETS), required=True)
    args = parser.parse_args()
    if os.geteuid() != pwd.getpwnam('pierrelouis').pw_uid or socket.gethostname().split('.')[0] != 'system76-pc':
        parser.error('This bounded worker requires the configured user on the established laptop')
    home = Path(__file__).resolve().parent
    kit = home / 'a6l-recovery-kit'
    capture_name = 'capture-diagnostic-install-user-v68' if args.mode == 'install-diagnostic' else 'capture-diagnostic-restore-user-v68'
    if args.output.resolve().parent != home / capture_name or args.output.name != 'edl':
        parser.error('Output must be the fixed recovery-restore capture directory')
    os.umask(0o022)
    args.output.mkdir(mode=0o700, exist_ok=False)
    report = {'scope': 'One fixed recovery write, independent readback, then selected power action',
              'started_utc': datetime.now(timezone.utc).isoformat(), 'operations': [],
              'mode': args.mode, 'pre_write_readback_verified': False, 'write': {},
              'readback_verified': False, 'power_acknowledged': False}
    path = args.output / 'report.json'

    def save():
        temporary = path.with_suffix('.json.tmp')
        temporary.write_text(json.dumps(report, indent=2) + '\n')
        temporary.replace(path)

    def deadline(signum, frame):
        raise TimeoutError('EDL worker reached its 120-second bound')

    signal.signal(signal.SIGALRM, deadline)
    signal.alarm(120)
    app = None
    wire_state = {'raw_read_pending': False, 'raw_write_pending': False, 'configured': False,
                  'ambiguous': False, 'permit_program': False, 'program_count': 0, 'permit_power': False}
    try:
        # Hash the exact immutable payload before touching USB or loading code.
        stock = (home / 'stock-recovery/restore-stock-recovery.img').read_bytes()
        report['restore_payload_sha256'] = verify_payload(stock, 'restore-stock')
        payload = ((home / 'ram-staging/recovery-diagnostic-staged-usb-v68.img').read_bytes()
                   if args.mode == 'install-diagnostic' else stock)
        report['target_sha256'] = verify_payload(payload, args.mode)
        save()
        manifest = json.loads((kit / 'manifest.json').read_text())
        for name, digest in manifest['files'].items():
            if hashlib.sha256((kit / name).read_bytes()).hexdigest() != digest:
                raise ValueError('Recovery kit input differs: ' + name)
        if hashlib.sha256((kit / 'programmer.elf').read_bytes()).hexdigest() != LOADER_HASH:
            raise ValueError('Programmer hash differs')
        devices = []
        for item in Path('/sys/bus/usb/devices').glob('*'):
            try:
                if (item / 'idVendor').read_text().strip() == '05c6' and (item / 'idProduct').read_text().strip() == '9008':
                    devices.append(item.name)
            except OSError:
                pass
        if devices != ['3-2']:
            raise ValueError('Expected exactly one EDL device, on the established physical port')
        port = Path('/sys/bus/usb/devices/3-2')
        bus = int((port / 'busnum').read_text())
        device = int((port / 'devnum').read_text())
        node = Path(f'/dev/bus/usb/{bus:03d}/{device:03d}')
        fd = os.open(node, os.O_RDWR | os.O_CLOEXEC)
        os.close(fd)
        report['unprivileged_edl_access'] = str(node)
        save()
        sys.path[:0] = [str(kit / 'deps'), str(kit / 'edl')]
        sys.argv = edl_arguments(kit)
        spec = importlib.util.spec_from_file_location('a6l_edl', kit / 'edl/edl.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        original_sahara = module.sahara

        class ExactSpare(original_sahara):
            def upload_loader(self, version):
                identity = {'serial': self.serials, 'hwid': self.hwidstr, 'pkhash': self.pkhash}
                report['sahara'] = identity
                save()
                if identity != {'serial': 'f17933a2', 'hwid': '0008c0e100430000', 'pkhash': PKHASH}:
                    raise ValueError('Hardware identity differs; no programmer will be uploaded')
                return super().upload_loader(version=version)

        module.sahara = ExactSpare
        from edlclient.Library.firehose import firehose
        from edlclient.Library.xmlparser import xmlparser
        original_xml = firehose.xmlsend
        original_parser_response = xmlparser.getresponse
        last_response = {}

        def capture_response(self, data):
            result = original_parser_response(self, data)
            last_response.clear()
            last_response.update(result)
            if result.get('value') == 'ACK' and result.get('rawmode') == 'false':
                wire_state['raw_read_pending'] = False
            return result

        xmlparser.getresponse = capture_response

        def guarded_xml(self, data, *positional, **keywords):
            try:
                tag = allowed_xml(data)
                if tag == 'power':
                    if not wire_state['permit_power'] or wire_state['raw_read_pending'] or wire_state['ambiguous']:
                        raise ValueError('Power action requires all independent readbacks')
                    expected = 'off' if args.mode == 'install-diagnostic' else 'reset'
                    if ET.fromstring(data)[0].get('value') != expected:
                        raise ValueError('Power action differs from the selected mode')
                    wire_state['permit_power'] = False
                if tag == 'program':
                    if (not wire_state['permit_program'] or wire_state['program_count'] != 0
                            or wire_state['raw_read_pending'] or wire_state['ambiguous']):
                        raise ValueError('Recovery programming is not enabled or has already been attempted')
                    wire_state['program_count'] += 1
                    wire_state['raw_write_pending'] = True
                    wire_state['permit_program'] = False
                elif wire_state['raw_write_pending']:
                    raise ValueError('No XML command may interrupt an unfinished write')
            except ValueError:
                report['blocked_xml_not_sent'] = data
                save()
                raise
            entry = {'tag': tag, 'xml': data}
            report['operations'].append(entry)
            save()
            if tag == 'read':
                wire_state['raw_read_pending'] = True
            try:
                result = original_xml(self, data, *positional, **keywords)
            except BaseException:
                wire_state['ambiguous'] = True
                raise
            entry['acknowledged'] = bool(result.resp)
            if tag == 'configure' and result.resp:
                wire_state['configured'] = True
            save()
            return result

        firehose.xmlsend = guarded_xml
        os.chdir(args.output)
        app = create_edl_app(module)
        app_result = app.run()
        report['connection_handoff'] = {'return_code': app_result, 'imported': app.imported,
                                         'firehose_connected': bool(app.fh and app.fh.connected)}
        save()
        if app_result != 0 or app.fh is None or not app.fh.connected:
            raise RuntimeError('Firehose connection failed')
        fh = app.fh.firehose
        if fh.cfg.SECTOR_SIZE_IN_BYTES != 512:
            raise ValueError('Unexpected device sector size')
        report['regions'] = {}
        for name, (offset, size) in REGIONS.items():
            output = args.output / (name + '.bin')
            if not fh.cmd_read(0, offset // 512, size // 512, str(output), display=False):
                raise RuntimeError('Read not acknowledged: ' + name)
            if last_response.get('value') != 'ACK' or last_response.get('rawmode') != 'false':
                raise RuntimeError('Final read acknowledgment missing: ' + name)
            if output.stat().st_size != size:
                raise ValueError('Read length differs: ' + name)
            digest = hashlib.sha256(output.read_bytes()).hexdigest()
            report['regions'][name] = {'offset': offset, 'bytes': size, 'sha256': digest}
            save()
            if name in ('primary', 'tail') and output.read_bytes() != (kit / ('expected-' + name + '.bin')).read_bytes():
                raise ValueError('GPT region differs from verified spare backup: ' + name)
            if name == 'recovery' and args.mode == 'install-diagnostic' and digest not in RECOVERIES:
                raise ValueError('Recovery is not an exact allowed predecessor')
            if name == 'vbmeta' and digest != 'e48632a03179e68b3c059905f613d321b3b37aa789ba72a02b322dffde4f7350':
                raise ValueError('Live vbmeta differs from the bootloader-tested fixture')
            print('Verified fixed read: ' + name, flush=True)
        if args.mode == 'install-diagnostic':
            report['transition'] = verify_transition(report['regions']['recovery']['sha256'],
                (args.output / 'misc-bcb.bin').read_bytes(), (args.output / 'devinfo.bin').read_bytes())
        report['pre_write_readback_verified'] = True
        info = (args.output / 'devinfo.bin').read_bytes()
        report['gpt_devinfo_flags'] = {'ordinary': info[13], 'critical': info[14]}
        report['misc_command_hex'] = (args.output / 'misc-bcb.bin').read_bytes()[:32].hex()
        save()
        # The known spare, both GPT regions and full existing recovery have
        # now been checked in this connection. Permit exactly one fixed program.
        wire_state['permit_program'] = True
        program_recovery(fh, payload, args.mode, report['write'], save)
        wire_state['raw_write_pending'] = False
        report['after_regions'] = {}
        for name, (offset, size) in REGIONS.items():
            output = args.output / (name + '-after-write.bin')
            last_response.clear()
            if not fh.cmd_read(0, offset // 512, size // 512, str(output), display=False):
                raise RuntimeError('Post-write read failed: ' + name)
            if last_response.get('value') != 'ACK' or last_response.get('rawmode') != 'false':
                raise RuntimeError('Post-write read acknowledgment missing: ' + name)
            if output.stat().st_size != size:
                raise ValueError('Post-write read length differs: ' + name)
            digest = hashlib.sha256(output.read_bytes()).hexdigest()
            report['after_regions'][name] = {'offset': offset, 'bytes': size, 'sha256': digest}
            save()
            expected = report['target_sha256'] if name == 'recovery' else report['regions'][name]['sha256']
            if digest != expected:
                raise ValueError('Post-write bytes differ from the required region hash: ' + name)
        report['write']['independent_readback_verified'] = True
        report['readback_verified'] = True
        save()
        # Permit one power action only after ALL post-write checks pass.
        wire_state['permit_power'] = True
        value = 'off' if args.mode == 'install-diagnostic' else 'reset'
        response = fh.xmlsend('<?xml version="1.0" ?><data><power value="' + value + '"/></data>')
        attributes = response.data if isinstance(response.data, dict) else fh.xml.getresponse(response.data)
        report['power_response'] = attributes
        report['power_action'] = value
        report['power_acknowledged'] = bool(response.resp) and attributes.get('value') == 'ACK'
        if not report['power_acknowledged']:
            raise RuntimeError('Power action was not explicitly acknowledged')
    except (Exception, SystemExit) as error:
        report['error'] = str(error)
        print('Recovery operation stopped: ' + str(error), flush=True)
    finally:
        # No automatic retry/reset after ANY stopped check or write. Preserve the
        # current connection/state for review, even if the write may be complete.
        signal.alarm(0)
        if app is not None and app.cdc is not None and app.cdc.connected:
            app.cdc.close()
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
    print(json.dumps(report, indent=2))
    return 0 if report['readback_verified'] and report['power_acknowledged'] and not report.get('error') else 1


if __name__ == '__main__':
    raise SystemExit(main())
