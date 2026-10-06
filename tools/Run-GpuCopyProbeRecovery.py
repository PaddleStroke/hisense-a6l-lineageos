#!/usr/bin/env python3
"""Laptop-only bounded RAM GPU comparison on the attended A6L recovery.
No flash, block writes, modem startup, or module unloading. Start live kmsg
before loading the verified modules; preserve full outputs on the laptop.
"""
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

SERIAL = 'HLTE730T-PROBE'
T = '/system/bin/toybox'
RAM = '/tmp/gpu-copy-probe-20261002'
KIT = Path.home() / 'A6L-usb-20260915/rom-r6f'
LOG = KIT / ('logs/gpu-copy-probe-recovery-20261002-' + time.strftime('%H%M%S', time.gmtime()))
LOG.mkdir(exist_ok=False)
archive = KIT / 'extra/gpu-copy-probe-20261002.tar.gz'
recovery_modules = KIT.parent / 'v71/bundle/gpu/modules'
recovery_pins = json.loads((KIT / 'extra/recovery-module-pins.json').read_text(encoding='utf-8-sig'))
expected = 'f74b7a2458d659d2af9ff66c98be9d5364487fd94944858c2b17d5853fa981ba'
h = hashlib.sha256(archive.read_bytes()).hexdigest()
assert h == expected, (h, expected)

def adb(args, name, timeout=25, check=True):
    r = subprocess.run(['adb','-s',SERIAL] + args, capture_output=True, timeout=timeout)
    (LOG / (name + '.txt')).write_bytes(r.stdout + r.stderr)
    if check and r.returncode:
        raise RuntimeError((name, r.returncode, (r.stdout+r.stderr).decode(errors='replace')[-2500:]))
    return r

def shell(command, name, timeout=25, check=True):
    return adb(['shell', command], name, timeout, check)

guard = shell(f'{T} uname -r; {T} tr -d "\\000" < /proc/device-tree/chosen/hisense,a6l-image; echo; {T} cat /proc/modules', 'guard')
assert b'7.2.3-a6l-probe+' in guard.stdout and b'v74' in guard.stdout, 'unexpected recovery'
if b'\nmsm ' in guard.stdout:
    assert '--reuse-loaded-gpu' in sys.argv, 'GPU already loaded; inspect first'
    separate = shell(f'{T} cat /sys/module/msm/parameters/separate_gpu_kms', 'existing-msm-parameter')
    assert any(l in (b'Y',b'1') for l in separate.stdout.splitlines()), 'combined GPU/KMS not allowed'
previous = shell(f'{T} printf "A6L_FW_PATH=%s\\n" "$({T} cat /sys/module/firmware_class/parameters/path)"', 'firmware-path-before')
# Strip the transport CR only; linker warnings are stderr, not firmware path.
old_path = next(line.split('=',1)[1] for line in previous.stdout.decode().splitlines()
                if line.startswith('A6L_FW_PATH='))
assert old_path == '', ('unexpected firmware search path',old_path)
stream = (LOG / 'live-kmsg.txt').open('wb')
reader = subprocess.Popen(['adb','-s',SERIAL,'exec-out',T,'cat','/dev/kmsg'],
                           stdout=stream, stderr=subprocess.STDOUT)
(LOG / 'reader-pid.txt').write_text(str(reader.pid)+'\n')
results = []
try:
    adb(['push',str(archive),'/tmp/gpu-copy-probe-20261002.tar.gz'], 'push',timeout=60)
    shell(f'set -e; [ "$({T} sha256sum /tmp/gpu-copy-probe-20261002.tar.gz | {T} cut -d " " -f 1)" = {expected} ]; '
          f'{T} gzip -dc /tmp/gpu-copy-probe-20261002.tar.gz | {T} tar -xf - -C /tmp; '
          f'cd {RAM}; {T} sha256sum -c SHA256SUMS; {T} chmod 755 gpu-copy-probe runtime/linker64', 'payload-check')
    shell(f'echo {RAM}/firmware > /sys/module/firmware_class/parameters/path; '
          'echo A6L_GPU_COPY_PROBE_BEGIN > /dev/kmsg', 'firmware-path-ram')
    names = (['drm_exec','drm_gpuvm','gpu-sched','mdt_loader','ocmem','ubwc_config',
              'llcc-qcom','qcom_aoss','cec','drm_display_helper','drm_dp_aux_bus','msm'])
    for name in names:
        source = recovery_modules / (name+'.ko')
        actual = hashlib.sha256(source.read_bytes()).hexdigest()
        assert actual == recovery_pins[source.name], (name,actual)
    adb(['push',str(recovery_modules),RAM+'/recovery-modules'],'push-recovery-modules')
    (LOG / 'recovery-module-pins.json').write_text(json.dumps(recovery_pins,indent=2)+'\n')
    for name in names:
        normalized = name.replace('-','_')
        arg = ' separate_gpu_kms=1' if name=='msm' else ''
        shell(f'set -e; [ "$({T} sha256sum {RAM}/recovery-modules/{name}.ko | {T} cut -d " " -f 1)" = {recovery_pins[name+".ko"]} ]; '
              f'{T} grep -q "^{normalized} " /proc/modules || '
              f'{T} timeout 15 {T} insmod {RAM}/recovery-modules/{name}.ko{arg}', 'load-'+name)
        print('MODULE',name,'OK',flush=True)
    nodes = shell(f'{T} mkdir -p /dev/dri; for n in /sys/class/drm/card[0-9]* /sys/class/drm/renderD[0-9]*; do '
                  f'[ -f "$n/dev" ] || continue; mm=$({T} cat "$n/dev"); nm=${{n##*/}}; '
                  f'[ -e /dev/dri/$nm ] || {T} mknod /dev/dri/$nm c ${{mm%:*}} ${{mm#*:}}; '
                  'echo "$nm $mm"; done', 'drm-nodes')
    assert b'renderD' in nodes.stdout, 'no GPU render node'
    for flags in ['sysmem,notile','sysmem']:
        for driver in ['old','new']:
            name = driver+'-'+flags.replace(',','-')
            command = (f'FD_MESA_DEBUG={flags} EGL_LOG_LEVEL=debug MESA_SHADER_CACHE_DISABLE=true '
                       f'LD_LIBRARY_PATH={RAM}/{driver}:{RAM}/runtime '
                       f'{T} timeout 45 {RAM}/runtime/linker64 '
                       f'{RAM}/gpu-copy-probe')
            try:
                r = shell(command,name,timeout=55,check=False)
            except subprocess.TimeoutExpired:
                results.append({'driver':driver,'flags':flags,'host_timeout':True})
                raise
            s = r.stdout.decode(errors='replace')
            summaries = [l for l in s.splitlines() if l.startswith(('GL renderer=','RESULT','PROBE_FATAL'))]
            results.append({'driver':driver,'flags':flags,'exit_code':r.returncode,'summary':summaries})
            print(name,r.returncode,summaries,flush=True)
            if r.returncode not in (0,1) or not any(l.startswith('RESULT') for l in summaries):
                raise RuntimeError('GPU probe failed to execute; do not call this a rendering pass')
            shell(f'{T} cat /proc/uptime',name+'-alive',timeout=8)
finally:
    (LOG / 'results.json').write_text(json.dumps(results,indent=2)+'\n')
    try:
        shell('echo > /sys/module/firmware_class/parameters/path', 'restore-firmware-path',timeout=8)
        shell(f'{T} dmesg', 'kernel-after',timeout=8)
    except Exception as error:
        (LOG / 'cleanup-error.txt').write_text(str(error)+'\n')
    reader.terminate()
    try: reader.wait(timeout=5)
    except subprocess.TimeoutExpired: reader.kill();reader.wait()
    stream.close()
print('GPU_COPY_PROBE_COMPLETE',LOG,flush=True)
