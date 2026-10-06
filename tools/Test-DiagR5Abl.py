#!/usr/bin/env python3
"""diag-r5 (30 Sep 2026): run tools/Test-RomV2Abl.py (captured ABL routines, NORMAL boot path: boot + dtbo partitions) on a
Prepare-DiagR5Boot.py output dir. Only change: the expected /chosen hisense,a6l-image marker is read from report.json
(rom-v2 for the dt-r6b variants, v74 for r5p-dtv74). Run with /home/a6l/venv-abl/bin/python.
usage: Test-DiagR5Abl.py <dir>"""
import json, sys
from pathlib import Path
ROOT = Path('/mnt/c/Users/Pierre/Desktop/A6L')
marker = json.loads((Path(sys.argv[1]) / 'report.json').read_text())['dt_image_marker']
src = (ROOT / 'tools/Test-RomV2Abl.py').read_text().replace('\r', '')
old = "read_fdt(merged)['/chosen']['hisense,a6l-image'] == b'rom-v2\\0'"
assert src.count(old) == 1
src = src.replace(old, "read_fdt(merged)['/chosen']['hisense,a6l-image'] == %r" % (marker.encode() + b'\0'))
sys.argv[0] = str(ROOT / 'tools/Test-RomV2Abl.py')
g = {'__name__': '__main__', '__file__': sys.argv[0]}
exec(compile(src, sys.argv[0], 'exec'), g)
