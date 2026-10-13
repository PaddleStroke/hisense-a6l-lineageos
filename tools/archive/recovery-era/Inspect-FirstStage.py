#!/usr/bin/env python3
"""Preserve and inspect the completed first-stage init; never package or flash it."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('source', type=Path)
p.add_argument('completed_build_log', type=Path)
p.add_argument('output', type=Path, help='New private output directory')
a = p.parse_args()
if '#### build completed successfully' not in a.completed_build_log.read_text()[-16384:]:
    raise SystemExit('First-stage build has no final successful-build marker')
binary = a.source / 'out/target/product/a6l/ramdisk/init'
data = binary.read_bytes()
if data[:6] != b'\x7fELF\x02\x01':
    raise SystemExit('Expected little-endian ELF64')
header = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
elf_type, machine, _, entry, phoff, _, _, _, phsize, phnum, _, _, _ = header
if phsize != 56 or phoff + phnum * phsize > len(data):
    raise SystemExit('Invalid ELF program-header layout')
segments = [struct.unpack_from('<IIQQQQQQ', data, phoff + i * phsize)
            for i in range(phnum)]
checks = {
    'aarch64_executable': elf_type == 2 and machine == 183,
    'no_interpreter': not any(s[0] == 3 for s in segments),
    'no_dynamic_segment': not any(s[0] == 2 for s in segments),
    'entry_in_executable_load': any(s[0] == 1 and s[1] & 1 and
                                    s[3] <= entry < s[3] + s[5] for s in segments),
    'non_executable_stack': any(s[0] == 0x6474e551 and not s[1] & 1 for s in segments),
    'load_segments_in_file': all(s[2] + s[5] <= len(data) and s[5] <= s[6]
                                 for s in segments if s[0] == 1),
}
a.output.mkdir(parents=True, exist_ok=False)
saved = a.output / 'init'
shutil.copyfile(binary, saved)
checks['preserved_copy_matches'] = digest(binary) == digest(saved)
details = subprocess.run(['readelf', '-h', '-l', str(saved)],
                         capture_output=True, text=True, check=True)
(a.output / 'readelf.txt').write_text(details.stdout)
shutil.copyfile(a.completed_build_log, a.output / 'build-first-stage.log')
revision = subprocess.run(['git', '-C', str(a.source / 'system/core'),
                           'rev-parse', 'HEAD'], capture_output=True, text=True,
                          check=True).stdout.strip()
report = {
    'scope': 'Offline ELF inspection; no boot packaging, execution or phone access',
    'source_binary': str(binary), 'bytes': len(data), 'sha256': digest(saved),
    'system_core_revision': revision,
    'pinned_manifest_sha256': digest(a.source / 'a6l-source-revisions.xml'),
    'entry_point': hex(entry), 'checks': checks,
    'all_checks_passed': all(checks.values()),
    'limitations': ['Kernel syscall compatibility and early mounts remain untested',
                    'This standalone component is not an integrated boot image'],
}
(a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
raise SystemExit(0 if report['all_checks_passed'] else 1)
