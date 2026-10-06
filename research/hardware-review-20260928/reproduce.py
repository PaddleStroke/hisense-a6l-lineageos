"""Run with WSL python3. Offline only; all mutable state is in a temporary dir.

Charger checks (F1/F2) were converted to the corrected expectations with the r5 review fixes (28 Sep 2026);
the sensor checks still assert the originally reproduced behavior.
Production source is never modified. Results and source hashes stay beside this file.
"""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
GUARD = ROOT / 'device/hisense/a6l/power/rom/a6l-chg-guard.sh'
HAL = ROOT / 'device/hisense/a6l/hals/sensors/stk3338'
sources = [GUARD, *(HAL / n for n in ('sensors_a6l.c', 'a6l_motion.c', 'a6l_motion.h', 'a6l_magcal.c'))]
hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
output = []
def record(s):
    output.append(s)
    print(s, flush=True)

with tempfile.TemporaryDirectory(prefix='a6l-hardware-review-') as tmp:
    work = Path(tmp)
    guard = work / 'guard.sh'
    guard.write_text(GUARD.read_text())
    bat, chg = work / 'bat', work / 'chg'
    bat.mkdir(); chg.mkdir()
    def setup(temp, status='1'):
        for p, v in {bat/'temp':temp, bat/'voltage_now':'3900000',
                     chg/'online':'1', chg/'usb_type':'[SDP] DCP CDP',
                     chg/'current_max':'500000', chg/'status':status}.items():
            p.write_text(str(v))
    env = dict(os.environ, BAT=str(bat), CHG=str(chg), ONESHOT='1')
    for temp, expected in ((50,500000), (460,500000), (250,500000)):  # r5 F1: min(source, thermal)
        setup(temp)
        subprocess.run(['sh',str(guard)],env=env,check=True,capture_output=True)
        actual = int((chg/'current_max').read_text())
        record(f'SDP T={temp}: input_limit={actual} source_budget=500000')
        assert actual == expected
    setup(250, '0')
    subprocess.run(['sh',str(guard)],env=env,check=True,capture_output=True)
    record('RESTART normal, previously suspended: status=' + (chg/'status').read_text().strip())
    assert (chg/'status').read_text().strip() == '1'   # r5 F2: desired state applied explicitly at startup

    # First suspend write fails because status is a directory. At the first
    # sleep boundary the fake node recovers. A second hot iteration must retry.
    setup(560)
    (chg/'status').unlink(); (chg/'status').mkdir()
    wrapper = work / 'fault.sh'
    wrapper.write_text('''n=0
sleep() {
 n=$((n+1))
 if [ "$n" = 1 ]; then rmdir "$CHG/status"; echo 1 > "$CHG/status"; else exit 0; fi
}
. "$GUARD"
''')
    result = subprocess.run(['sh',str(wrapper)], env=dict(env,ONESHOT='0',GUARD=str(guard)),
                            capture_output=True,text=True,check=True)
    assert 'ERROR: write' in result.stdout and 'applied after' in result.stdout   # r5 F2: failure logged, retried
    actual = (chg/'status').read_text().strip()
    record('SUSPEND WRITE FAILURE then node recovery: status=' + actual + ' (0 means suspend retried and applied)')
    assert actual == '0'

    binary = work / 'sensor-review'
    subprocess.run(['gcc','-std=gnu11','-O1','-g','-DA6L_HOST_TEST',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                    '-I'+str(HAL/'tests/include'),'-I'+str(HAL),
                    str(HERE/'sensor_review.c'),str(HAL/'a6l_motion.c'),str(HAL/'a6l_magcal.c'),
                    '-lm','-lpthread','-o',str(binary)],check=True)
    empty = work / 'empty'; empty.mkdir()
    result = subprocess.run([str(binary)],env=dict(os.environ,A6L_SYSROOT=str(empty),
                            A6L_DEVROOT=str(empty),A6L_DATADIR=str(empty)),
                            capture_output=True,text=True,timeout=10,check=True)
    record(result.stdout.strip())
    record('Sensor harness completed with ASan/UBSan; no sanitizer diagnostics.')

assert all(hashlib.sha256(p.read_bytes()).hexdigest() == hashes[str(p.relative_to(ROOT))] for p in sources)
(HERE/'source-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
(HERE/'results.txt').write_text('\n'.join(output)+'\n')
