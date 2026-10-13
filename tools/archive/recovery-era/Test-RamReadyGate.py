"""Reject stock, unauthenticated, and persistently mounted environments."""
import importlib.util,json
from pathlib import Path
root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('ready',root/'tools/Wait-AndroidRamReady.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
good='v38\n7.2.3-a6l-probe+\n1\n1\n1\nuid=0(root) gid=0(root)\nrootfs / rootfs rw 0 0\ntmpfs /tmp tmpfs rw 0 0\n'
checks={
 'diagnostic':m.valid_response(0,good),
 'adb_failure_rejected':not m.valid_response(1,good),
 'stock_kernel_rejected':not m.valid_response(0,good.replace('7.2.3-a6l-probe+','4.4.153')),
 'insecure_adb_rejected':not m.valid_response(0,good.replace('probe+\n1','probe+\n0')),
 'unprivileged_rejected':not m.valid_response(0,good.replace('uid=0(root)','uid=2000(shell)')),
 'persistent_mount_rejected':not m.valid_response(0,good+'/dev/block/mmcblk0p1 /system ext4 ro 0 0\n'),
 'missing_tmpfs_rejected':not m.valid_response(0,good.replace('tmpfs /tmp tmpfs rw 0 0\n','')),
 'truncated_mount_rejected':not m.valid_response(0,good+'malformed\n')}
out=root/'research/android-surface-v44-20260917';out.mkdir(parents=True,exist_ok=True)
(out/'readiness-checks.json').write_text(json.dumps(checks,indent=2)+'\n')
print(json.dumps(checks,indent=2));assert all(checks.values())
