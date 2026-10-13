"""Generate the guarded V37 workflow, accepting only stock or exact V36 predecessor."""
import hashlib
import json
from pathlib import Path
import py_compile

ROOT = Path(__file__).resolve().parents[1]
T = ROOT / 'tools'
OUT = ROOT / 'firmware/extracted/recovery-probe-storage-read-v37-20260917'
package = json.loads((OUT / 'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT / 'captured-abl-validation.json').read_text())['passed']
candidate = hashlib.sha256((OUT / 'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate == package['candidate_sha256']
old = 'fa6319e61da7347b28f76527f8ff75dfeba2a6b3c7067de564ca85fe9123018e'
old_previous = '129621db2ac07c8c955426a5a974a4ffc8ba587eb883d0e6c76e6c01a2f76c00'
previous = json.loads((T / 'diagnostic-user-v36-tools.json').read_text())['files']
manifest = dict(candidate_sha256=candidate, sources=previous, files={})
read_manifest = json.loads((ROOT / 'firmware/extracted/storage-read-v37-20260917/read-manifest.json').read_text())


def write(name, s):
    p = T / name
    assert not p.exists(), p
    p.write_bytes(s.encode())
    py_compile.compile(str(p), doraise=True)
    return hashlib.sha256(p.read_bytes()).hexdigest()


def version(s):
    return s.replace('v36', 'v37').replace('V36', 'V37')


for name, h in previous.items():
    raw = (T / name).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == h, name
    s = version(raw.decode()).replace('recovery-probe-kernel72-v37-20260917', 'recovery-probe-storage-read-v37-20260917')
    if name == 'RecoveryTransitionV36.py':
        assert old_previous in s
        s = s.replace(old_previous, old).replace('verified-v35', 'verified-v36').replace('verified V35', 'verified V36')
    else:
        s = s.replace(old, candidate)
    if name.startswith('Inspect-'):
        anchor = 'for mode, filename in ['
        assert s.count(anchor) == 1
        s = s.replace(anchor, anchor + "('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v36.img'), ")
    if name.startswith('Collect-'):
        start = s.index('def storage_window_complete(data):')
        end = s.index('\ndef save():', start)
        expected = {r['name']: r['sha256'] for r in read_manifest['ranges']}
        total = sum(r['bytes'] for r in read_manifest['ranges']) * 2
        predicate = '''def storage_window_complete(data):
    data = data.replace(b'\\r\\n', b'\\n')
    expected = EXPECTED
    forks = [int(v) for v in re.findall(rb'A6L_STORAGE_READ_FORK_BEGIN seconds=([0-9]+)', data)]
    alive = [int(v) for v in re.findall(rb'A6L_RAM_PROBE_ALIVE seconds=([0-9]+)', data)]
    hashes_ok = all(f'A6L_STORAGE_READ_HASH pass={p} name={name} sha256={digest} match=1'.encode() in data
                    for p in (1, 2) for name, digest in expected.items())
    return bool(hashes_ok and b'A6L_STORAGE_READ_PASS regions=5 passes=2 bytes=TOTAL ' in data
                and b'A6L_STORAGE_READ_CHILD_EXIT status=0\\n' in data
                and b'A6L_STORAGE_READ_FAIL' not in data and b'Kernel panic' not in data
                and forks and alive and max(alive) >= forks[0] + 8)

'''.replace('EXPECTED', repr(expected)).replace('TOTAL', str(total))
        s = s[:start] + predicate + s[end:]
        s = s.replace('time.monotonic() + 60', 'time.monotonic() + 75')
        s = s.replace('report["post_selection_capture_seconds"] = 60', 'report["post_selection_capture_seconds"] = 75')
    manifest['files'][version(name)] = write(version(name), s)

write('Test-RecoveryTransitionV37.py', version((T / 'Test-RecoveryTransitionV36.py').read_text()))
write('Verify-StorageReadReadbacks.py', version((T / 'Verify-Kernel72Readbacks.py').read_text()).replace(old, candidate))
(T / 'diagnostic-user-v37-tools.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
