"""One fixed V12 recovery install/restore, supervised on the Windows desktop."""
import hashlib
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
import WindowsRecoveryReadOnly as base
from DiagnosticRecoveryProtocolV11 import TARGETS, verify_payload, check_program_xml
from WindowsRecoverySerialProtocol import program

ROOT = base.ROOT
MODES = ('install-diagnostic', 'restore-stock')
PAYLOADS = {
    'install-diagnostic': ROOT / 'firmware/extracted/recovery-probe-usb-only-20260916/recovery-diagnostic-unsigned.img',
    'restore-stock': ROOT / 'captures/windows-edl-readonly-v1/edl/recovery.bin',
}
TESTED_FILES = ('WindowsRecoveryTrial.py', 'WindowsRecoverySerialProtocol.py',
                'WindowsRecoveryReadOnly.py', 'DiagnosticRecoveryProtocolV11.py',
                'Test-WindowsRecoveryTrial.py')
BCB_BOOTONCE = '8ac9baa0ce2f52dda6debef8f8ffb4fcd8e85d44bf05882dbcb552dd06b46881'


def output(mode):
    if mode not in MODES:
        raise ValueError('Unrecognized fixed mode')
    return ROOT / ('captures/windows-v12-' + mode)


class TrialGate(base.ReadGate):
    def __init__(self, mode):
        super().__init__()
        if mode not in MODES:
            raise ValueError('Unrecognized fixed mode')
        self.mode = mode
        self.program_count = 0
        self.write_pending = False
        self.write_complete = False
        self.before = {}
        self.after = {}

    def begin(self, data):
        root = ET.fromstring(data)
        if root.tag != 'data' or root.attrib or len(root) != 1 or (root.text or '').strip():
            raise ValueError('Expected one operation')
        node = root[0]
        if len(node) or (node.text or '').strip() or (node.tail or '').strip():
            raise ValueError('Nested command/text prohibited')
        if self.pending or self.write_pending or self.ambiguous or self.reset_sent:
            raise ValueError('Incomplete or ambiguous operation')
        if node.tag == 'program':
            check_program_xml(data)
            if self.program_count or set(self.before) != set(base.REGIONS):
                raise ValueError('Program requires all verified pre-reads and only one attempt')
            self.program_count = 1
            self.write_pending = True
            return 'program'
        if node.tag == 'power':
            expected = 'off' if self.mode == 'install-diagnostic' else 'reset'
            if (node.attrib != {'value': expected} or not self.write_complete
                    or set(self.after) != set(base.REGIONS)):
                raise ValueError('Power requires all independent post-write comparisons')
            self.reset_sent = True
            return 'power'
        if node.tag == 'configure' and node.get('ZLPAwareHost') != '0':
            raise ValueError('COM transport must configure ZLPAwareHost=0')
        return super().begin(data)

    def complete_write(self, report):
        if (self.program_count != 1 or not self.write_pending or self.ambiguous
                or not report.get('write_acknowledged') or report.get('raw_mode_pending')
                or report.get('bytes_transferred') != 67108864):
            raise ValueError('Write completion not established')
        self.write_pending = False
        self.write_complete = True

    def verify_region(self, name, data):
        _, size, stock_digest = base.REGIONS[name]
        if self.pending or self.write_pending or self.ambiguous or len(data) != size:
            raise ValueError('Read incomplete or invalid size')
        digest = hashlib.sha256(data).hexdigest()
        if self.program_count:
            if not self.write_complete:
                raise ValueError('Write has not completed')
            expected = TARGETS[self.mode] if name == 'recovery' else self.before[name]
            if digest != expected:
                raise ValueError('Post-write mismatch: ' + name)
            self.after[name] = digest
        else:
            allowed = {stock_digest}
            if self.mode == 'restore-stock':
                if name == 'recovery':
                    allowed = {TARGETS['install-diagnostic']}
                elif name == 'misc-bcb':
                    allowed.add(BCB_BOOTONCE)
            if digest not in allowed:
                raise ValueError('Pre-write region differs: ' + name)
            self.before[name] = digest


def verify_inputs():
    base.verify_inputs()
    preflight = json.loads((ROOT / 'firmware/extracted/windows-recovery-trial-preflight.json').read_text())
    if not preflight['passed'] or set(preflight['tested_tools']) != set(TESTED_FILES):
        raise ValueError('Missing preflight')
    for name, digest in preflight['tested_tools'].items():
        if hashlib.sha256((ROOT / 'tools' / name).read_bytes()).hexdigest() != digest:
            raise ValueError('Tested tool changed: ' + name)
    for mode in MODES:
        verify_payload(PAYLOADS[mode].read_bytes(), mode)


def worker(mode, port):
    directory = output(mode) / 'edl'
    directory.mkdir(exist_ok=False)
    log = (directory / 'worker.log').open('x', encoding='utf-8', buffering=1)
    sys.stdout = sys.stderr = log
    report = {'mode': mode, 'started_utc': base.utc(), 'operations': [], 'regions': {},
              'after_regions': {}, 'write': {}, 'readback_verified': False, 'power_acknowledged': False}
    def save():
        base.save(directory / 'report.json', report)
    gate = TrialGate(mode)
    app = None
    save()
    try:
        verify_inputs()
        payload = PAYLOADS[mode].read_bytes()
        verify_payload(payload, mode)
        ports = base.matching_ports()
        assert len(ports) == 1 and ports[0].device == port
        module = base.edl_module(port)
        original_sahara = module.sahara
        class ExactSpare(original_sahara):
            def cmd_reset(self, *args, **kwargs):
                raise RuntimeError('Automatic Sahara reset prohibited')
            def streaminginfo(self, *args, **kwargs):
                raise RuntimeError('Streaming fallback prohibited')
            def upload_loader(self, version):
                identity = {'serial': self.serials, 'hwid': self.hwidstr, 'pkhash': self.pkhash}
                report['sahara'] = identity
                save()
                base.check_identity(identity)
                return super().upload_loader(version=version)
        module.sahara = ExactSpare
        from edlclient.Library.firehose import firehose
        from edlclient.Library.xmlparser import xmlparser
        original_xml, original_parse = firehose.xmlsend, xmlparser.getresponse
        original_configure = firehose.configure
        last_response = {}
        def configure(self, level):
            self.cfg.ZLPAwareHost = 0
            return original_configure(self, level)
        firehose.configure = configure
        def response(self, data):
            attributes = original_parse(self, data)
            last_response.clear()
            last_response.update(attributes)
            gate.response(attributes)
            return attributes
        xmlparser.getresponse = response
        def guarded(self, data, *args, **kwargs):
            tag = gate.begin(data)
            entry = {'tag': tag, 'xml': data}
            report['operations'].append(entry)
            save()
            try:
                result = original_xml(self, data, *args, **kwargs)
                if tag == 'read' and (not result.resp or last_response.get('value') != 'ACK'
                                      or last_response.get('rawmode') != 'true'):
                    raise RuntimeError('Read initial raw-mode ACK absent')
            except BaseException:
                gate.ambiguous = True
                raise
            entry['acknowledged'] = bool(result.resp)
            save()
            return result
        firehose.xmlsend = guarded
        os.chdir(directory)
        app = module.main(module.args)
        assert app.imported is True
        assert app.run() == 0 and app.fh is not None and app.fh.connected
        base.check_identity(report.get('sahara'))
        fh = app.fh.firehose
        assert fh.cdc.is_serial and fh.cfg.SECTOR_SIZE_IN_BYTES == 512
        def read_all(after=False):
            for name, (offset, size, _) in base.REGIONS.items():
                path = directory / (name + ('-after-write' if after else '') + '.bin')
                last_response.clear()
                assert fh.cmd_read(0, offset // 512, size // 512, str(path), display=False)
                assert last_response.get('value') == 'ACK' and last_response.get('rawmode') == 'false'
                data = path.read_bytes()
                gate.verify_region(name, data)
                report['after_regions' if after else 'regions'][name] = {
                    'offset': offset, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                save()
        read_all()
        report['pre_write_readback_verified'] = True
        save()
        program(fh, payload, mode, report['write'], save)
        gate.complete_write(report['write'])
        read_all(after=True)
        report['readback_verified'] = True
        save()
        value = 'off' if mode == 'install-diagnostic' else 'reset'
        result = fh.xmlsend('<data><power value="' + value + '"/></data>')
        attrs = result.data if isinstance(result.data, dict) else fh.xml.getresponse(result.data)
        report['power_acknowledged'] = bool(result.resp) and attrs.get('value') == 'ACK'
        assert report['power_acknowledged']
    except BaseException as error:
        report['error'] = repr(error)
        print('Stopped; no reset or retry: ' + repr(error), flush=True)
    finally:
        if app is not None and app.cdc is not None and app.cdc.connected:
            try:
                app.cdc.close()
            except Exception as error:
                report['close_error'] = repr(error)
        report['finished_utc'] = base.utc()
        save()
    raise SystemExit(0 if report['readback_verified'] and report['power_acknowledged'] and not report.get('error') else 1)


def execute(mode):
    assert sys.platform == 'win32' and socket.gethostname().lower() == 'desktop-3hcgn2h'
    verify_inputs()
    assert json.loads((ROOT / 'captures/windows-edl-readonly-v1/session.json').read_text())['android_return_verified']
    directory = output(mode)
    assert not directory.exists()
    assert base.android() == base.EXPECTED
    devices = subprocess.check_output([str(base.ADB), 'devices'], text=True, timeout=10).splitlines()[1:]
    assert [s.split() for s in devices if s.strip()] == [['1e529013', 'device']]
    battery = subprocess.check_output([str(base.ADB), '-s', '1e529013', 'shell', 'dumpsys', 'battery'], text=True, timeout=10)
    level = re.search(r'^\s*level:\s*(\d+)\s*$', battery, re.M)
    assert level and int(level.group(1)) >= 40
    assert not base.matching_ports()
    directory.mkdir(exist_ok=False)
    report = {'mode': mode, 'started_utc': base.utc(), 'battery_level': int(level.group(1)),
              'worker_started': False, 'independent_desktop_verification': False}
    def save():
        base.save(directory / 'session.json', report)
    save()
    try:
        subprocess.run([str(base.ADB), '-s', '1e529013', 'reboot', 'edl'], check=True, timeout=10)
        report['edl_requested'] = True
        save()
        deadline = time.monotonic() + 30
        while True:
            ports = base.matching_ports()
            if len(ports) == 1:
                break
            assert not ports and time.monotonic() < deadline, 'EDL port absent/ambiguous; stop'
            time.sleep(.25)
        proc = multiprocessing.get_context('spawn').Process(target=worker, args=(mode, ports[0].device))
        proc.start()
        report.update(worker_started=True, worker_pid=proc.pid, edl_port=ports[0].device)
        save()
        proc.join(120)
        if proc.is_alive():
            proc.kill()
            proc.join(5)
            report['worker_timed_out'] = True
            raise RuntimeError('Worker timed out; no automatic reset/retry')
        report['worker_exit'] = proc.exitcode
        save()
        assert proc.exitcode == 0, 'Worker failed; inspect before any phone action'
        result = json.loads((directory / 'edl/report.json').read_text())
        assert result['readback_verified'] and result['power_acknowledged']
        for key, suffix in [('regions', ''), ('after_regions', '-after-write')]:
            for name, entry in result[key].items():
                data = (directory / 'edl' / (name + suffix + '.bin')).read_bytes()
                assert len(data) == entry['bytes'] and hashlib.sha256(data).hexdigest() == entry['sha256']
        report['independent_desktop_verification'] = True
        deadline = time.monotonic() + 30
        while base.matching_ports() and time.monotonic() < deadline:
            time.sleep(.25)
        report['edl_disconnected'] = not base.matching_ports()
        assert report['edl_disconnected']
    except BaseException as error:
        report['error'] = repr(error)
        raise
    finally:
        report['finished_utc'] = base.utc()
        save()
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[1] != '--execute' or sys.argv[2] not in MODES:
        raise SystemExit('Expected --execute install-diagnostic|restore-stock after offline preflight')
    execute(sys.argv[2])
