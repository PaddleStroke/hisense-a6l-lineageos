"""Check V37 capture completion against missing, corrupt and premature results."""
import ast
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'tools/Collect-ProbeSerial-v37.py').read_text()
tree = ast.parse(source)
node = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'storage_window_complete')
scope = {'re': re}
exec(compile(ast.Module(body=[node], type_ignores=[]), '<collector predicate>', 'exec'), scope)
check = scope['storage_window_complete']
m = json.loads((ROOT / 'firmware/extracted/storage-read-v37-20260917/read-manifest.json').read_text())
total = sum(r['bytes'] for r in m['ranges']) * 2
hashes = [f"A6L_STORAGE_READ_HASH pass={p} name={r['name']} sha256={r['sha256']} match=1"
          for p in (1, 2) for r in m['ranges']]
lines = ['A6L_STORAGE_READ_FORK_BEGIN seconds=32', *hashes,
         f'A6L_STORAGE_READ_PASS regions=5 passes=2 bytes={total} duration_ms=2000',
         'A6L_STORAGE_READ_CHILD_EXIT status=0', 'A6L_RAM_PROBE_ALIVE seconds=40']
payload = ('\n'.join(lines) + '\n').encode()
assert check(payload)
for line in hashes:
    assert not check(payload.replace((line + '\n').encode(), b''))
assert not check(payload.replace(b'match=1', b'match=0', 1))
assert not check(payload.replace(b'status=0\n', b'status=14\n'))
assert not check(payload.replace(b'ALIVE seconds=40', b'ALIVE seconds=38'))
assert not check(payload + b'A6L_STORAGE_READ_FAILED\n')
assert not check(payload + b'Kernel panic\n')
assert not check(payload.replace(f'bytes={total} '.encode(), b'bytes=0 '))
old = (ROOT / 'captures/capture-probe-serial-user-v36/serial/console.bin').read_bytes()
assert not check(old), 'Enumeration alone is not a passing read test'
assert 'time.monotonic() + 75' in source
out = ROOT / 'firmware/extracted/storage-read-v37-20260917/collector-tests.json'
out.write_text(json.dumps(dict(passed=True, cases=19, scope='Strict hash/exit/heartbeat result acceptance; V36 enumeration rejected'), indent=2)+'\n')
print('V37 collector: all completion and failure cases passed.')
