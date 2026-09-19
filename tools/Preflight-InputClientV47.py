"""Syntax-only check using the latest emitted Soong compiler invocation."""
import os,shlex,subprocess
from pathlib import Path
root=Path('/home/a6l/android/a6l-lineage24')
log=Path('/home/a6l/logs/build-android-input-v47-r4.log').read_text()
lines=[s for s in log.splitlines() if s.startswith('PWD=') and s.endswith('diagnostic/input_client.cpp')]
assert len(lines)==1
args=shlex.split(lines[0])[1:]
clean=[];i=0
while i<len(args):
    if args[i] in ('-o','-MF'):i+=2;continue
    if args[i] in ('-c','-MD'):i+=1;continue
    clean.append(args[i]);i+=1
clean += ['-fsyntax-only','-Ipackages/modules/StatsD/lib/libstatssocket/include','-Iframeworks/native/services/inputflinger/reporter','-Iframeworks/native/services/batteryservice/include']
raise SystemExit(subprocess.run(clean,cwd=root).returncode)
