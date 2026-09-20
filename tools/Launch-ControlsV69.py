"""Launch one verified V69 capture under the laptop's desktop sleep inhibitor."""
import json,os,subprocess
from pathlib import Path
root=Path(__file__).resolve().parent
assert not (root/'capture-controls-user-v69').exists()
subprocess.run(['/usr/bin/python3',str(root/'Verify-ControlsV69Stage.py')],check=True)
env=dict(os.environ,DISPLAY=':0',XDG_RUNTIME_DIR='/run/user/1000',DBUS_SESSION_BUS_ADDRESS='unix:path=/run/user/1000/bus')
with (root/'controls-v69-launch.log').open('x') as log:
    p=subprocess.Popen(['/usr/bin/gnome-session-inhibit','--app-id','A6L-v69-capture',
                        '--reason','A6L combined controls test','--inhibit','suspend:idle',
                        '/usr/bin/python3',str(root/'Run-LaptopControlsCapture-v69.py')],
                       cwd=root,env=env,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,
                       start_new_session=True)
print(json.dumps({'launched_pid':p.pid}))
