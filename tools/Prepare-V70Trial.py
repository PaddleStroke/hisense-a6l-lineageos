"""Generate isolated V68 install/restore/inspect tools from the hash-pinned V46 tools. Offline only."""
import hashlib,json,py_compile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];T=ROOT/'tools'
IMAGE=ROOT/'firmware/extracted/recovery-v70-candidate-20260921'
V46='4dc4861ebcbd30b5f38ab236b3bdb37e57140803dca1b993d4749808c2815eec'  # here: the V68 tools' candidate
V45='2448b101eb780b1630cc6fd7181315da50211de2504b08cd6dad9e34d44a7520'  # here: the V68 tools' predecessor
report=json.loads((IMAGE/'report.json').read_text());V68=report['candidate_sha256']  # = V69 hash in this generator
assert hashlib.sha256((IMAGE/'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()==V68 and report['previous_sha256']==V46
assert json.loads((IMAGE/'captured-abl-validation.json').read_text())['passed']
pins=json.loads((T/'diagnostic-user-v69-tools.json').read_text())['files']
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
manifest={'candidate_sha256':V68,'previous_sha256':V46,'files':{},'sources':{}}
def source(name):
    p=T/name;assert digest(p)==pins[name],('pinned V46 tool changed',name);manifest['sources'][name]=pins[name];return p.read_text()
def v68(text):
    # order matters: the V46 tools name V45 as "previous" and V46 as "candidate"
    text=text.replace(V45,'@@PREV@@').replace(V46,V68).replace('@@PREV@@',V46)
    text=text.replace('verified-v68','@@LABEL@@').replace('verified V68 predecessor','@@PRED@@')
    text=text.replace('recovery-v69-candidate-20260921','recovery-@@C@@-candidate-20260921')
    return text.replace('V69','V70').replace('v69','v70').replace('@@C@@','v70').replace('@@LABEL@@','verified-v69').replace('@@PRED@@','verified V69 predecessor')
def put(name,text):
    p=T/name;p.write_text(text);py_compile.compile(str(p),doraise=True);manifest['files'][name]=digest(p)
for old in [n for n in pins if n!='diagnostic-user-v69-tools.json']:
    put(old.replace('V69','V70').replace('v69','v70'),v68(source(old)))
put('diagnostic-user-v70-tools.json',json.dumps(manifest,indent=2)+'\n')
leftovers={n:[w for w in ('v45','V45','controls-v46-prep') if w in (T/n).read_text()] for n in manifest['files'] if n.endswith('.py')}
print(json.dumps({'prepared':len(manifest['files']),'candidate':V68,'previous':V46,'review_leftovers':{k:v for k,v in leftovers.items() if v}},indent=1))
