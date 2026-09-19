"""Launch one verified V47 RAM capture under the laptop's desktop sleep inhibitor."""
import json,os,subprocess
from pathlib import Path
root=Path(__file__).resolve().parent
assert not (root/'capture-input-user-v47').exists()
subprocess.run(['/usr/bin/python3',str(root/'Verify-InputV47Stage.py')],check=True)
env=dict(os.environ,DISPLAY=':0',XDG_RUNTIME_DIR='/run/user/1000',DBUS_SESSION_BUS_ADDRESS='unix:path=/run/user/1000/bus')
with (root/'input-v47-launch.log').open('x') as log:
    p=subprocess.Popen(['/usr/bin/gnome-session-inhibit','--app-id','A6L-v47-capture',
                        '--reason','A6L Android input test','--inhibit','suspend:idle',
                        '/usr/bin/python3',str(root/'Run-LaptopInputCapture-v47.py')],
                       cwd=root,env=env,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,
                       start_new_session=True)
print(json.dumps({'launched_pid':p.pid}))
