#!/usr/bin/env python3
"""Copy the stock A6L driver's exposed SPI region with a fixed-size helper.

Temporarily installs the reviewed helper in /data/local/tmp, executes only its
read path, and removes that helper. The stock read handler cycles EPD power.
No root, ioctls, calibration writes, or partition writes are requested.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--adb', type=Path, required=True)
p.add_argument('--serial', required=True)
p.add_argument('--helper', type=Path, required=True)
p.add_argument('--output', type=Path, required=True, help='New private capture directory')
a = p.parse_args()
adb = [str(a.adb.resolve()), '-s', a.serial]
remote = '/data/local/tmp/a6l-epd-read-analysis'


def command(*args, check=True):
    return subprocess.run([*adb, *args], check=check, capture_output=True, timeout=30)


device = command('shell', 'getprop', 'ro.product.device').stdout.strip()
fingerprint = command('shell', 'getprop', 'ro.build.fingerprint').stdout.strip()
if device != b'HLTE730T' or fingerprint != b'Hisense/HLTE730T/HLTE730T:9/PKQ1.190723.001/L1632.6.01.04:user/release-keys':
    raise SystemExit('This helper contract is only verified against the captured A6L build')
if not a.helper.read_bytes().startswith(b'\x7fELF'):
    raise SystemExit('Expected the compiled reader ELF')
a.output.mkdir(parents=True, exist_ok=False)
report = {'scope': 'Fixed 0x70080-byte region exposed by stock driver, not a full SPI-chip dump',
          'expected_bytes': 0x70080, 'fingerprint': fingerprint.decode(),
          'helper_sha256': hashlib.sha256(a.helper.read_bytes()).hexdigest(), 'reads': []}
try:
    # Refuse to overwrite any pre-existing file at the helper's fixed path.
    exists = command('shell', 'test', '-e', remote, check=False)
    if exists.returncode == 0:
        raise SystemExit('A helper already exists at the fixed path; inspect it first')
    if exists.returncode != 1:
        raise SystemExit('Could not establish whether the temporary helper path is available')
    command('push', str(a.helper.resolve()), remote)
    report['helper_pushed'] = True
    command('shell', 'chmod', '700', remote)
    for index in (1, 2):
        # -T uses the shell-v2 binary stream without a pseudo-terminal and keeps
        # remote stderr separate. exec-out's merged streams are unsuitable here.
        proc = command('shell', '-T', remote, check=False)
        (a.output / f'read-{index:02}.stderr.txt').write_bytes(proc.stderr)
        (a.output / f'read-{index:02}.bin').write_bytes(proc.stdout)
        entry = {'exit_code': proc.returncode, 'bytes': len(proc.stdout),
                 'sha256': hashlib.sha256(proc.stdout).hexdigest()}
        report['reads'].append(entry)
        if proc.returncode != 0 or len(proc.stdout) != 0x70080:
            print(f'Read {index} failed: exit={proc.returncode}, bytes={len(proc.stdout)}')
            print(proc.stderr.decode(errors='replace'))
            break
        print(f'Read {index}: {len(proc.stdout)} bytes, SHA-256 {entry["sha256"]}')
    report['two_matching_reads'] = (len(report['reads']) == 2 and
                                    all(x['exit_code'] == 0 and x['bytes'] == 0x70080
                                        for x in report['reads']) and
                                    report['reads'][0]['sha256'] == report['reads'][1]['sha256'])
finally:
    if report.get('helper_pushed'):
        cleanup = command('shell', 'rm', '--', remote, check=False)
        report['temporary_helper_removed'] = cleanup.returncode == 0
    (a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
if not report.get('two_matching_reads'):
    raise SystemExit('No verified SPI-region backup produced; inspect the saved report')
