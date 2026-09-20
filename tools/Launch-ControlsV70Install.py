"""Launch the checked V70 install only when explicitly invoked while attended."""
import json,os,subprocess
from pathlib import Path
root=Path(__file__).resolve().parent
assert not (root/'capture-diagnostic-install-user-v70').exists()
subprocess.run(['/usr/bin/python3',str(root/'Verify-ControlsV70Stage.py')],check=True)
env=dict(os.environ,DISPLAY=':0',XDG_RUNTIME_DIR='/run/user/1000',DBUS_SESSION_BUS_ADDRESS='unix:path=/run/user/1000/bus')
with (root/'controls-v70-install-launch.log').open('x') as log:
    p=subprocess.Popen(['/usr/bin/gnome-session-inhibit','--app-id','A6L-v70-install','--reason','A6L diagnostic installation','--inhibit','suspend:idle','/usr/bin/python3',str(root/'Run-LaptopDiagnosticInstall-user-v70.py')],cwd=root,env=env,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
print(json.dumps({'launched_pid':p.pid}))
