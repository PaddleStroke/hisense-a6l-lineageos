"""Launch the "preserve userdata" update coordinator ONLY while Pierre is present (agent update-keepdata, 29 Sep 2026).
usage: python3 Launch-RomUpdateV1.py --mode backup-only|update|rollback --transport adb-recovery|edl [coordinator options]
A dry run of the worker must pass first (it is repeated inside the coordinator)."""
import json, os, subprocess, sys
from pathlib import Path
root = Path(__file__).resolve().parent
args = sys.argv[1:]
if '--dry-run' in args:
    sys.exit('use Run-LaptopRomUpdate-v1.py --dry-run directly (offline)')
mode = args[args.index('--mode') + 1] if '--mode' in args else None
if mode not in ('backup-only', 'update', 'rollback'):
    sys.exit('--mode backup-only|update|rollback required')
subprocess.run(['/usr/bin/python3', str(root / 'Run-LaptopRomUpdate-v1.py')] + args + ['--dry-run'], check=True)
env = dict(os.environ, DISPLAY=':0', XDG_RUNTIME_DIR='/run/user/1000', DBUS_SESSION_BUS_ADDRESS='unix:path=/run/user/1000/bus')
log_path = root / f'rom-update-{mode}-launch.log'
if log_path.exists():
    os.replace(log_path, log_path.with_suffix('.log.%d' % int(log_path.stat().st_mtime)))
with log_path.open('x') as log:
    p = subprocess.Popen(['/usr/bin/gnome-session-inhibit', '--app-id', 'A6L-rom-update', '--reason', 'A6L rom update ' + mode,
                          '--inhibit', 'suspend:idle', '/usr/bin/python3', str(root / 'Run-LaptopRomUpdate-v1.py')] + args,
                         cwd=root, env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
print(json.dumps({'launched_pid': p.pid, 'log': log_path.name}))
