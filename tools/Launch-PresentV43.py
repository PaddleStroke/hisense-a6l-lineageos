"""Launch one verified V43 capture under the laptop's desktop sleep inhibitor."""
import json,os,subprocess
from pathlib import Path
root=Path(__file__).resolve().parent
assert not (root/'capture-present-user-v43').exists()
subprocess.run(['/usr/bin/python3',str(root/'Verify-PresentV43Stage.py')],check=True)
env=dict(os.environ,DISPLAY=':0',XDG_RUNTIME_DIR='/run/user/1000',DBUS_SESSION_BUS_ADDRESS='unix:path=/run/user/1000/bus')
with (root/'present-v43-launch.log').open('x') as log:
    p=subprocess.Popen(['/usr/bin/gnome-session-inhibit','--app-id','A6L-v43-capture',
                        '--reason','A6L RAM Android graphics test','--inhibit','suspend:idle',
                        '/usr/bin/python3',str(root/'Run-LaptopPresentCapture-v43.py')],
                       cwd=root,env=env,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,
                       start_new_session=True)
print(json.dumps({'launched_pid':p.pid}))
