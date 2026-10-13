"""Verify wrong-device/identity/hash/mount rejection precedes all staging or actuation."""
import contextlib,hashlib,importlib.util,io,json,subprocess,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
spec=importlib.util.spec_from_file_location('runner',Path(__file__).with_name('Run-ControlsV45.py'));runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
class Guards(unittest.TestCase):
    def reject(self,identity=None,marker='v45\x00',mounts='tmpfs /tmp tmpfs rw 0 0\n',corrupt=False):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);payload=root/'controls-v45/payload';payload.mkdir(parents=True)
            (payload/'sample').write_bytes(b'reviewed')
            (payload.parent/'manifest.json').write_text(json.dumps({'files':{'sample':{'sha256':hashlib.sha256(b'wrong' if corrupt else b'reviewed').hexdigest()}}}))
            calls=[]
            good='v38\n7.2.3-a6l-probe+\n1\nuid=0(root) gid=0(root)\n'
            def fake(argv,**kwargs):
                calls.append(argv)
                command=' '.join(argv)
                if 'getprop ro.a6l.ramdiag; uname' in command:output=identity if identity is not None else good
                elif 'hisense,a6l-controls' in command:output=marker
                elif '/proc/mounts' in command:output=mounts
                elif command.endswith('getprop ro.a6l.ramdiag'):output='v38\n'
                else:output=''
                return subprocess.CompletedProcess(argv,0,output,'')
            with patch.object(runner,'ROOT',root),patch.object(runner,'PACKAGE',payload.parent),patch.object(runner.subprocess,'run',side_effect=fake),patch('sys.argv',['Run-ControlsV45.py','vibration']),contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(runner.main(),1)
            self.assertFalse(any(any(x in ' '.join(cmd) for x in [' push ','chmod','insmod','--pulse','backlight_probe','mkdir']) for cmd in calls),calls)
    def test_stock_android_rejected(self):self.reject(identity='\n4.4.153\n1\nuid=2000(shell)\n')
    def test_insecure_adb_rejected(self):self.reject(identity='v38\n7.2.3-a6l-probe+\n0\nuid=0(root)\n')
    def test_unprivileged_rejected(self):self.reject(identity='v38\n7.2.3-a6l-probe+\n1\nuid=2000(shell)\n')
    def test_previous_diagnostic_rejected(self):self.reject(marker='v38\x00')
    def test_mounted_userdata_rejected(self):self.reject(mounts='tmpfs /tmp tmpfs rw 0 0\n/dev/mmcblk0p59 /data ext4 rw 0 0\n')
    def test_missing_ram_mount_rejected(self):self.reject(mounts='proc /proc proc rw 0 0\n')
    def test_corrupted_payload_rejected(self):self.reject(corrupt=True)
if __name__=='__main__':unittest.main(verbosity=2)
