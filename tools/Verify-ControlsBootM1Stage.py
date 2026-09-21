"""Verify BootM1 staged files only; never open USB or invoke a phone operation."""
import hashlib, json
from pathlib import Path
ROOT = Path(__file__).resolve().parent
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
pins = json.loads((ROOT / 'bootm1-pins.json').read_text())
for name, digest in pins.items():
    assert sha(ROOT / name) == digest, name
manifest = json.loads((ROOT / 'diagnostic-user-bootm1-tools.json').read_text())
assert manifest['candidate_sha256'] == '259c43e3f4c32ab95db02467d1bd34f5f0b525414f074583a715acc131f8237e' and manifest['previous_sha256'] == '55ad4747ea8d83a32edb09eafb01772a3cb170378331cdea3d45cd5a8cb34dbc'
assert sha(ROOT / 'ram-staging/boot-magisk-staged-bootm1.img') == manifest['candidate_sha256']
assert sha(ROOT / 'stock-boot/restore-stock-boot.img') == manifest['previous_sha256']
print(json.dumps({'passed': True, 'pin_count': len(pins), 'scope': 'Offline staged-file verification; no USB access or phone actions'}, indent=2))
