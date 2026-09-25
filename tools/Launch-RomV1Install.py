"""Launch the rom-v1 install ONLY when explicitly invoked while Pierre is present (agent flash)."""
import json, os, subprocess, sys
from pathlib import Path
root = Path(__file__).resolve().parent
assert not (root / 'capture-rom-v1-install').exists(), 'capture dir exists: inspect it, do not re-run blindly'
subprocess.run(['/usr/bin/python3', str(root / 'Verify-RomV1Stage.py')] + (['--restore'] if 'Install' == 'Restore' else []), check=True)
env = dict(os.environ, DISPLAY=':0', XDG_RUNTIME_DIR='/run/user/1000', DBUS_SESSION_BUS_ADDRESS='unix:path=/run/user/1000/bus')
with (root / 'rom-v1-install-launch.log').open('x') as log:
    p = subprocess.Popen(['/usr/bin/gnome-session-inhibit', '--app-id', 'A6L-rom-v1', '--reason', 'A6L rom-v1 install',
                          '--inhibit', 'suspend:idle', '/usr/bin/python3', str(root / 'Run-LaptopRomInstall-v1.py')] + sys.argv[1:],
                         cwd=root, env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
print(json.dumps({'launched_pid': p.pid, 'log': 'rom-v1-install-launch.log'}))
