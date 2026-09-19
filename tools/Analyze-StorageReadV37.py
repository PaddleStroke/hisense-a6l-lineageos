"""Audit the saved physical result and regress the collector's CRLF handling.

The original manifest-pinned collector and its report remain unchanged.
"""
import ast
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
capture = ROOT / 'captures/capture-probe-serial-user-v37'
data = (capture / 'serial/console.bin').read_bytes()
manifest = json.loads((ROOT / 'firmware/extracted/storage-read-v37-20260917/read-manifest.json').read_text())
expected = {r['name']: r['sha256'] for r in manifest['ranges']}
total = sum(r['bytes'] for r in manifest['ranges']) * 2
tree = ast.parse((ROOT / 'tools/Prepare-StorageReadTrial.py').read_text())
template = next(n.value for n in ast.walk(tree) if isinstance(n, ast.Constant)
                and isinstance(n.value, str) and n.value.startswith('def storage_window_complete(data):')
                and 'expected = EXPECTED' in n.value)
source = template.replace('EXPECTED', repr(expected)).replace('TOTAL', str(total))
scope = {'re': re}
exec(compile(source, '<generated collector predicate>', 'exec'), scope)
check = scope['storage_window_complete']
assert check(data), 'Physical capture must pass the corrected predicate'
assert check(data.replace(b'\r\n', b'\n')), 'LF capture must also pass'
for p in (1, 2):
    for name, digest in expected.items():
        marker = f'A6L_STORAGE_READ_HASH pass={p} name={name} sha256={digest} match=1'.encode()
        assert marker in data
        assert not check(data.replace(marker, b'MISSING_HASH'))
assert not check(data.replace(b'CHILD_EXIT status=0', b'CHILD_EXIT status=14'))
assert not check(data + b'Kernel panic\r\n')
assert not check(data + b'A6L_STORAGE_READ_FAIL\r\n')
old_tree = ast.parse((ROOT / 'tools/Collect-ProbeSerial-v37.py').read_text())
old_fn = next(n for n in old_tree.body if isinstance(n, ast.FunctionDef) and n.name == 'storage_window_complete')
old_scope = {'re': re}
exec(compile(ast.Module(body=[old_fn], type_ignores=[]), '<original predicate>', 'exec'), old_scope)
assert not old_scope['storage_window_complete'](data)
assert old_scope['storage_window_complete'](data.replace(b'\r\n', b'\n'))
duration = re.search(rb'A6L_STORAGE_READ_PASS regions=5 passes=2 bytes=153268224 duration_ms=(\d+)', data)
alive = [int(x) for x in re.findall(rb'A6L_RAM_PROBE_ALIVE seconds=(\d+)', data)]
report = {
    'storage_read_passed': True,
    'console_bytes': len(data), 'console_sha256': hashlib.sha256(data).hexdigest(),
    'matching_hashes': 10, 'read_bytes': total, 'duration_ms': int(duration[1]),
    'child_exit': 0, 'last_heartbeat_seconds': max(alive),
    'collector_false_negative': 'CRLF child exit line did not match LF-only predicate',
    'original_report_preserved': True, 'regression_checks_passed': 17,
    'scope': 'Two direct read-only passes over five pinned firmware/GPT regions; no write or Android boot validation',
}
(capture / 'analysis.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
