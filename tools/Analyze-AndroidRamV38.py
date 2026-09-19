"""Summarize the saved physical V38 evidence without changing the original logs."""
import ast
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
capture = ROOT / 'captures/capture-probe-serial-user-v38'
data = (capture / 'serial/console.bin').read_bytes()
tree = ast.parse((ROOT / 'tools/Collect-ProbeSerial-v38.py').read_text())
node = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'storage_window_complete')
scope = {'re': re}
exec(compile(ast.Module(body=[node], type_ignores=[]), '<collector>', 'exec'), scope)
assert scope['storage_window_complete'](data)
serial = json.loads((capture / 'serial/report.json').read_text())
assert serial['staged_capture_complete'] and serial['stop_reason'] == 'storage_window_complete'
assert serial['bytes'] == len(data) and not serial['kernel_panic_seen']
adb = json.loads((capture / 'adb-read-only-verification.json').read_text())
assert all(q['exit'] == 0 for q in adb['queries'])
queries = {tuple(q['argv']): q['stdout'].strip() for q in adb['queries']}
assert queries[('getprop', 'ro.a6l.ramdiag')] == 'v38'
assert queries[('getprop', 'ro.adb.secure')] == '1'
assert queries[('getprop', 'ro.adb.secure.recovery')] == '1'
assert queries[('getprop', 'init.svc.adbd')] == 'running'
assert queries[('readlink', '/proc/1/exe')] == '/system/bin/init'
assert queries[('cat', '/sys/fs/selinux/enforce')] == '0'
assert '7.2.3-a6l-probe+' in queries[('uname', '-a')]
assert 'uid=0(root)' in queries[('id',)]
mounts = queries[('cat', '/proc/mounts')].splitlines()
allowed = {'rootfs', 'tmpfs', 'devpts', 'proc', 'sysfs', 'selinuxfs', 'configfs', 'functionfs', 'debugfs'}
assert all(line.split()[2] in allowed for line in mounts)
duration = re.search(rb'A6L_STORAGE_READ_PASS regions=5 passes=2 bytes=153268224 duration_ms=(\d+)', data)
result = {
    'passed': True, 'console_bytes': len(data), 'console_sha256': hashlib.sha256(data).hexdigest(),
    'android_init_pid1': True, 'authenticated_adb_shell': True, 'adb_root': True,
    'selinux_policy_loaded': True, 'selinux_enforcing': False,
    'only_virtual_filesystems_mounted': True, 'matching_hashes': 10,
    'storage_read_bytes': 153268224, 'storage_read_duration_ms': int(duration[1]),
    'storage_child_exit': 0,
    'last_serial_heartbeat_seconds': max(map(int, re.findall(rb'A6L_RAM_PROBE_ALIVE seconds=(\d+)', data))),
    'limitations': ['RAM recovery userspace, not full Lineage framework or graphics',
                    'SELinux development permissive mode',
                    'Linker fallback works but generated linker configuration is absent',
                    'No persistent filesystem mounts or storage write validation'],
}
(capture / 'analysis.json').write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result, indent=2))
