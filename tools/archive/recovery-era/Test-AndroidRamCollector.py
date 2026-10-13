"""V38 acceptance checks: Android services AND complete read-only hash evidence."""
import ast
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'tools/Collect-ProbeSerial-v38.py').read_text()
node = next(n for n in ast.parse(source).body if isinstance(n, ast.FunctionDef)
            and n.name == 'storage_window_complete')
scope = {'re': re}
exec(compile(ast.Module(body=[node], type_ignores=[]), '<collector predicate>', 'exec'), scope)
check = scope['storage_window_complete']
manifest = json.loads((ROOT / 'firmware/extracted/storage-read-v37-20260917/read-manifest.json').read_text())
total = sum(r['bytes'] for r in manifest['ranges']) * 2
hashes = [f"A6L_STORAGE_READ_HASH pass={p} name={r['name']} sha256={r['sha256']} match=1"
          for p in (1, 2) for r in manifest['ranges']]
android = ['A6L_ANDROID_SERVICE_START pid=79 parent=1 init=/system/bin/init',
           'A6L_ANDROID_PROPERTY ramdiag=v38 selinux_time=123',
           'A6L_ANDROID_ADB_FUNCTION linked=1', 'A6L_ANDROID_ADBD state=running']
lines = [*android, 'A6L_STORAGE_READ_FORK_BEGIN seconds=32', *hashes,
         f'A6L_STORAGE_READ_PASS regions=5 passes=2 bytes={total} duration_ms=2000',
         'A6L_STORAGE_READ_CHILD_EXIT status=0', 'A6L_RAM_PROBE_ALIVE seconds=40']
payload = ('\n'.join(lines) + '\n').encode()
results = {}
def expect(name, data, accepted):
    results[name] = bool(check(data)) == accepted
    assert results[name], name
expect('complete LF', payload, True)
expect('complete CRLF', payload.replace(b'\n', b'\r\n'), True)
for i, line in enumerate(hashes + android):
    expect(f'missing required marker {i}', payload.replace((line + '\n').encode(), b''), False)
for name, data in {
    'wrong hash': payload.replace(b'match=1', b'match=0', 1),
    'child failure': payload.replace(b'status=0\n', b'status=14\n'),
    'early heartbeat': payload.replace(b'ALIVE seconds=40', b'ALIVE seconds=38'),
    'read failed': payload + b'A6L_STORAGE_READ_FAILED\n',
    'panic': payload + b'Kernel panic\n',
    'wrong byte count': payload.replace(f'bytes={total} '.encode(), b'bytes=0 '),
    'wrong Android version': payload.replace(b'ramdiag=v38', b'ramdiag=v37'),
}.items():
    expect(name, data, False)
old = (ROOT / 'captures/capture-probe-serial-user-v37/serial/console.bin').read_bytes()
expect('actual V37 lacks Android', old, False)
# Exercises the exact CRLF capture that exposed V37's overly narrow LF predicate.
expect('actual V37 with synthetic Android markers', ('\r\n'.join(android)+'\r\n').encode()+old, True)
assert 'time.monotonic() + 75' in source
out = ROOT / 'firmware/extracted/recovery-probe-android-ram-v38-20260917/collector-tests.json'
out.write_text(json.dumps(dict(passed=True, cases=results,
    scope='Synthetic acceptance tests and actual V37 capture regression; not physical V38 proof'), indent=2)+'\n')
print(f'V38 collector: {len(results)} cases passed.')
