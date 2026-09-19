#!/usr/bin/env python3
"""Native Windows EDL qualification: fixed reads, no program/erase/patch."""
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import multiprocessing
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'captures/windows-edl-readonly-v1'
EDL = ROOT / 'tools/edl'
LOADER = ROOT / 'tools/a6l-recovery-kit/programmer.elf'
ADB = ROOT / 'tools/platform-tools/adb.exe'
MANIFEST_HASH = '93979cd0f15a36a0df9b88fe15f9d7d6d2fddbff05c3cf4ba25a718dd472497f'
IDENTITY = {'serial': 'f17933a2', 'hwid': '0008c0e100430000', 'pkhash': '06a0604b3069cca35fd538f8ca5a8fc5d07c90a6e755f8cfacc1426cb9d75d22'}
REGIONS = {
    'primary': (0, 1048576, '0a170df9e55b8c1e2c7ad971f44c2b05ddfa78efc1b47588ebdbefadfdd396b0'),
    'tail': (125074122752, 22528, '14a0aa4ecef49ffe6b477cf43a08da1d457624d6a687e8be2f4d7015b9359946'),
    'recovery': (469762048, 67108864, '9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621'),
    'devinfo': (253755392, 4096, '7d6a4855f19d498a092ff0ffb44a69e114cd915d4d49acb2640035cf70e854ed'),
    'misc-bcb': (8757706752, 4096, 'ad7facb2586fc6e966c004d7d1d16b024f5805ff7cb47c7a85dabd8b48892ca7'),
    'vbmeta': (291811328, 65536, 'e48632a03179e68b3c059905f613d321b3b37aa789ba72a02b322dffde4f7350'),
}
EXPECTED = {'ro.build.fingerprint': 'Hisense/HLTE730T/HLTE730T:9/PKQ1.190723.001/L1632.6.01.04:user/release-keys',
            'sys.boot_completed': '1', 'ro.boot.flash.locked': '0', 'ro.boot.verifiedbootstate': 'orange'}

def utc():
    return datetime.now(timezone.utc).isoformat()

def save(path, data):
    temp = path.with_suffix('.json.tmp')
    temp.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    temp.replace(path)

def verify_inputs():
    raw = (ROOT / 'tools/windows-edl-readonly-manifest-v1.json').read_bytes()
    assert hashlib.sha256(raw).hexdigest() == MANIFEST_HASH
    manifest = json.loads(raw)
    for name, digest in manifest['files'].items():
        assert hashlib.sha256((ROOT / name).read_bytes()).hexdigest() == digest, name
    return len(manifest['files'])

def check_identity(identity):
    if identity != IDENTITY:
        raise ValueError('Wrong Sahara identity; no programmer upload allowed')

class ReadGate:
    def __init__(self):
        self.pending = False
        self.ambiguous = False
        self.verified = set()
        self.reset_sent = False

    def begin(self, data):
        root = ET.fromstring(data)
        if root.tag != 'data' or root.attrib or len(root) != 1 or (root.text or '').strip():
            raise ValueError('Expected one Firehose operation')
        node = root[0]
        if len(node) or (node.text or '').strip() or (node.tail or '').strip():
            raise ValueError('Unexpected nested command or text')
        if self.pending or self.ambiguous or self.reset_sent:
            raise ValueError('Unfinished/ambiguous connection or reset already sent')
        tag = node.tag
        if tag == 'read':
            if node.get('SECTOR_SIZE_IN_BYTES') != '512' or node.get('physical_partition_number') != '0':
                raise ValueError('Unexpected read geometry')
            geometry = (int(node.get('start_sector', '-1')) * 512, int(node.get('num_partition_sectors', '-1')) * 512)
            if geometry not in [(v[0], v[1]) for v in REGIONS.values()] + [(512, 512)]:
                raise ValueError('Read outside fixed qualification regions')
            self.pending = True
        elif tag == 'configure':
            if node.get('MemoryName', '').lower() != 'emmc':
                raise ValueError('Only eMMC configuration allowed')
        elif tag in ('nop', 'getstorageinfo'):
            pass
        elif tag == 'power':
            if node.attrib != {'value': 'reset'} or self.verified != set(REGIONS):
                raise ValueError('Reset requires all six independently verified reads')
            self.reset_sent = True
        else:
            raise ValueError('Persistent/unsupported command rejected: ' + tag)
        return tag

    def response(self, attributes):
        if attributes.get('value') == 'ACK' and attributes.get('rawmode') == 'false':
            self.pending = False

    def verify_region(self, name, data):
        _, size, digest = REGIONS[name]
        if self.pending or self.ambiguous or len(data) != size or hashlib.sha256(data).hexdigest() != digest:
            raise ValueError('Region read differs or is unfinished: ' + name)
        self.verified.add(name)

def edl_module(port):
    assert re.fullmatch(r'COM[1-9][0-9]*', port)
    sys.path.insert(0, str(EDL))
    sys.argv = ['edl.py', 'getstorageinfo', '--loader=' + str(LOADER), '--memory=eMMC', '--serial', '--portname=' + port]
    spec = importlib.util.spec_from_file_location('a6l_windows_edl', EDL / 'edl.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def matching_ports():
    from serial.tools import list_ports
    return [p for p in list_ports.comports() if p.vid == 0x05c6 and p.pid == 0x9008]

def worker(port):
    path = OUT / 'edl'
    path.mkdir(exist_ok=False)
    report = {'scope': 'Fixed Windows EDL reads only; no program/erase/patch', 'started_utc': utc(),
              'operations': [], 'regions': {}, 'readback_verified': False, 'reset_acknowledged': False}
    log = (path / 'worker.log').open('w', encoding='utf-8', buffering=1)
    sys.stdout = sys.stderr = log
    app = None
    gate = ReadGate()
    def persist():
        save(path / 'report.json', report)
    persist()
    try:
        report['pinned_files_verified'] = verify_inputs()
        ports = matching_ports()
        assert len(ports) == 1 and ports[0].device == port, 'EDL port changed or multiple devices'
        module = edl_module(port)
        original_sahara = module.sahara
        class ExactSpare(original_sahara):
            def cmd_reset(self, *args, **kwargs):
                raise RuntimeError('Automatic Sahara reset prohibited; inspect failed handshake')

            def streaminginfo(self, *args, **kwargs):
                raise RuntimeError('Streaming fallback prohibited')

            def upload_loader(self, version):
                identity = {'serial': self.serials, 'hwid': self.hwidstr, 'pkhash': self.pkhash}
                report['sahara'] = identity
                persist()
                check_identity(identity)
                return super().upload_loader(version=version)
        module.sahara = ExactSpare
        from edlclient.Library.firehose import firehose
        from edlclient.Library.xmlparser import xmlparser
        original_xml = firehose.xmlsend
        original_response = xmlparser.getresponse
        last_response = {}
        def response(self, data):
            result = original_response(self, data)
            last_response.clear()
            last_response.update(result)
            gate.response(result)
            return result
        xmlparser.getresponse = response
        def guarded_xml(self, data, *args, **kwargs):
            tag = gate.begin(data)
            entry = {'tag': tag, 'xml': data}
            report['operations'].append(entry)
            persist()
            try:
                result = original_xml(self, data, *args, **kwargs)
            except BaseException:
                gate.ambiguous = True
                raise
            entry['acknowledged'] = bool(result.resp)
            persist()
            return result
        firehose.xmlsend = guarded_xml
        os.chdir(path)
        app = module.main(module.args)
        assert app.imported is True
        result = app.run()
        assert result == 0 and app.fh is not None and app.fh.connected
        fh = app.fh.firehose
        assert fh.cfg.SECTOR_SIZE_IN_BYTES == 512 and fh.cfg.MemoryName.lower() == 'emmc'
        for name, (offset, size, digest) in REGIONS.items():
            output = path / (name + '.bin')
            last_response.clear()
            assert fh.cmd_read(0, offset // 512, size // 512, str(output), display=False)
            assert last_response.get('value') == 'ACK' and last_response.get('rawmode') == 'false'
            data = output.read_bytes()
            gate.verify_region(name, data)
            report['regions'][name] = {'offset': offset, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
            persist()
            print('Verified fixed read: ' + name, flush=True)
        report['readback_verified'] = True
        persist()
        result = fh.xmlsend('<?xml version="1.0" ?><data><power value="reset"/></data>')
        attributes = result.data if isinstance(result.data, dict) else fh.xml.getresponse(result.data)
        report['reset_acknowledged'] = bool(result.resp) and attributes.get('value') == 'ACK'
        assert report['reset_acknowledged']
    except BaseException as error:
        report['error'] = repr(error)
        print('Qualification stopped: ' + repr(error), flush=True)
    finally:
        # No retry or reset after any stopped check. The parent enforces timeout.
        if app is not None and app.cdc is not None and app.cdc.connected:
            try:
                app.cdc.close()
            except Exception as error:
                report['close_error'] = repr(error)
        report['finished_utc'] = utc()
        persist()
        log.flush()
    raise SystemExit(0 if report['readback_verified'] and report['reset_acknowledged'] and not report.get('error') else 1)

def android():
    return {p: subprocess.run([str(ADB), '-s', '1e529013', 'shell', 'getprop', p], capture_output=True, text=True, check=True, timeout=5).stdout.strip() for p in EXPECTED}

def execute():
    assert sys.platform == 'win32' and socket.gethostname().lower() == 'desktop-3hcgn2h'
    verify_inputs()
    preflight = json.loads((ROOT / 'firmware/extracted/windows-edl-readonly-preflight-v1.json').read_text())
    assert preflight['passed'] and preflight['manifest_sha256'] == MANIFEST_HASH
    for name, digest in preflight['tested_tools'].items():
        assert hashlib.sha256((ROOT / 'tools' / name).read_bytes()).hexdigest() == digest, name
    assert not OUT.exists()
    devices = subprocess.check_output([str(ADB), 'devices'], text=True, timeout=10).splitlines()[1:]
    assert [s.split() for s in devices if s.strip()] == [['1e529013', 'device']]
    assert android() == EXPECTED
    assert not matching_ports(), 'An EDL device already exists; inspect before proceeding'
    OUT.mkdir(exist_ok=False)
    report = {'scope': 'Native Windows read-only EDL qualification then normal Android return', 'started_utc': utc(), 'before': EXPECTED, 'worker_started': False, 'android_return_verified': False}
    def persist():
        save(OUT / 'session.json', report)
    persist()
    subprocess.run([str(ADB), '-s', '1e529013', 'reboot', 'edl'], check=True, timeout=10)
    report['edl_requested'] = True
    persist()
    deadline = time.monotonic() + 30
    while True:
        ports = matching_ports()
        if len(ports) == 1:
            break
        assert not ports, 'More than one EDL device; do not choose'
        assert time.monotonic() < deadline, 'EDL port absent; no retry or reset'
        time.sleep(0.25)
    report['edl_port'] = ports[0].device
    proc = multiprocessing.get_context('spawn').Process(target=worker, args=(ports[0].device,))
    proc.start()
    report.update(worker_started=True, worker_pid=proc.pid)
    persist()
    proc.join(120)
    if proc.is_alive():
        proc.kill()
        proc.join(5)
        report['worker_timed_out'] = True
        report['finished_utc'] = utc()
        persist()
        raise RuntimeError('Read worker exceeded120seconds; no reset or retry issued')
    report['worker_exit'] = proc.exitcode
    persist()
    assert proc.exitcode == 0, 'Read-only worker stopped; inspect before any phone action'
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        try:
            if android() == EXPECTED:
                report['android_return_verified'] = True
                break
        except (subprocess.SubprocessError, OSError):
            pass
        time.sleep(2)
    report['finished_utc'] = utc()
    persist()
    assert report['android_return_verified'], 'Awaiting Android return; no automatic second reset'
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    if sys.argv[1:] != ['--execute']:
        raise SystemExit('Use the offline preflight first; explicit --execute required for the supervised read-only test.')
    execute()
