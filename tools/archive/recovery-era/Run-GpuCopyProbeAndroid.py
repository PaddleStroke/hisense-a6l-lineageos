#!/usr/bin/env python3
"""Laptop: private GPU-copy A/B probe on installed Android, no ROM replacement.
Requires the fresh radio-off hardware UI comparison to be ready. Small test
files under /data/local/tmp are removed after execution; logs remain on host.
"""
import hashlib
import json
import subprocess
from pathlib import Path

KIT=Path.home()/'A6L-usb-20260915/rom-r6f'
LOG=sorted(p for p in (KIT/'logs').glob('gpu-copy-android-20261002-*') if p.is_dir())[-1]
READY=json.loads((LOG/'ready.json').read_text())
R='/data/local/tmp/gpu-copy-probe-20261002'
ARCHIVE=KIT/'extra/gpu-copy-probe-20261002.tar.gz'
DIGEST='f74b7a2458d659d2af9ff66c98be9d5364487fd94944858c2b17d5853fa981ba'
A=['adb','-s','1e529013']
assert hashlib.sha256(ARCHIVE.read_bytes()).hexdigest()==DIGEST

def shell(c,name,timeout=15,check=True):
    z=subprocess.run(A+['shell',c],capture_output=True,text=True,timeout=timeout)
    (LOG/(name+'.txt')).write_text(z.stdout+z.stderr)
    if check and z.returncode:raise RuntimeError((name,z.returncode,z.stdout,z.stderr))
    return z

z=shell('cat /proc/sys/kernel/random/boot_id; getprop persist.vendor.a6l.radio; '
        'getprop debug.mesa.fd.mesa.debug','probe-guard')
assert z.stdout.replace('\r','').splitlines()==[READY['boot_id'],'0','sysmem,noblit']
shell('id; ls -l /dev/dri; cat /proc/version','probe-kernel')
subprocess.run(A+['push',str(ARCHIVE),'/data/local/tmp/gpu-copy-probe-20261002.tar.gz'],check=True,capture_output=True,timeout=45)
shell(f'set -e; [ "$(sha256sum /data/local/tmp/gpu-copy-probe-20261002.tar.gz | cut -d " " -f 1)" = {DIGEST} ]; '
      f'[ ! -e {R} ]; toybox gzip -dc /data/local/tmp/gpu-copy-probe-20261002.tar.gz | toybox tar -xf - -C /data/local/tmp; '
      f'cd {R}; sha256sum -c SHA256SUMS; chmod 755 gpu-copy-probe runtime/linker64','probe-payload',timeout=45)
results=[]
try:
    for flags in ['sysmem','sysmem,noblit']:
        for driver in ['old','new']:
            name='probe-'+driver+'-'+flags.replace(',','-')
            c=(f'FD_MESA_DEBUG={flags} MESA_SHADER_CACHE_DISABLE=true '
               f'LD_LIBRARY_PATH={R}/{driver}:{R}/runtime toybox timeout 45 '
               f'{R}/runtime/linker64 {R}/gpu-copy-probe')
            z=shell(c,name,timeout=55,check=False)
            summary=[l for l in z.stdout.splitlines() if l.startswith(('GL renderer=','RESULT','PROBE_FATAL'))]
            results.append({'driver':driver,'flags':flags,'exit_code':z.returncode,'summary':summary})
            print(name,z.returncode,summary,flush=True)
            if not any(l.startswith('RESULT') for l in summary):raise RuntimeError('probe did not complete')
            shell('cat /proc/uptime',name+'-alive',timeout=8)
finally:
    (LOG/'offscreen-results.json').write_text(json.dumps(results,indent=2)+'\n')
    # Literal task-specific targets only; no parent or wildcard removal.
    shell(f'rm -rf {R}; rm -f /data/local/tmp/gpu-copy-probe-20261002.tar.gz','probe-cleanup',timeout=10)
shell('getprop debug.mesa.fd.mesa.debug; getprop persist.vendor.a6l.radio; '
      'dumpsys SurfaceFlinger','probe-ui-after',timeout=15)
print('ANDROID_GPU_COPY_COMPARISON_COMPLETE',LOG,flush=True)
