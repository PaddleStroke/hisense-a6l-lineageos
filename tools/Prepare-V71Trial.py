"""Generate isolated V68 install/restore/inspect tools from the hash-pinned V46 tools. Offline only."""
import hashlib,json,py_compile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];T=ROOT/'tools'
IMAGE=ROOT/'firmware/extracted/recovery-v71-candidate-20260921'
V46='b93771e701d677c58e377feae794a25d3a9c4c1f5cd379bb904910e4661a4373'  # here: the V68 tools' candidate
V45='4dc4861ebcbd30b5f38ab236b3bdb37e57140803dca1b993d4749808c2815eec'  # here: the V68 tools' predecessor
report=json.loads((IMAGE/'report.json').read_text());V68=report['candidate_sha256']  # = V70 hash in this generator
assert hashlib.sha256((IMAGE/'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()==V68 and report['previous_sha256']==V46
assert json.loads((IMAGE/'captured-abl-validation.json').read_text())['passed']
pins=json.loads((T/'diagnostic-user-v70-tools.json').read_text())['files']
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
manifest={'candidate_sha256':V68,'previous_sha256':V46,'files':{},'sources':{}}
def source(name):
    p=T/name;assert digest(p)==pins[name],('pinned V46 tool changed',name);manifest['sources'][name]=pins[name];return p.read_text()
def v68(text):
    # order matters: the V46 tools name V45 as "previous" and V46 as "candidate"
    text=text.replace(V45,'@@PREV@@').replace(V46,V68).replace('@@PREV@@',V46)
    text=text.replace('verified-v69','@@LABEL@@').replace('verified V69 predecessor','@@PRED@@')
    text=text.replace('recovery-v70-candidate-20260921','recovery-@@C@@-candidate-20260921')
    return text.replace('V70','V71').replace('v70','v71').replace('@@C@@','v71').replace('@@LABEL@@','verified-v70').replace('@@PRED@@','verified V70 predecessor')
def put(name,text):
    p=T/name;p.write_text(text);py_compile.compile(str(p),doraise=True);manifest['files'][name]=digest(p)
for old in [n for n in pins if n!='diagnostic-user-v70-tools.json']:
    put(old.replace('V70','V71').replace('v70','v71'),v68(source(old)))
put('diagnostic-user-v71-tools.json',json.dumps(manifest,indent=2)+'\n')
leftovers={n:[w for w in ('v45','V45','controls-v46-prep') if w in (T/n).read_text()] for n in manifest['files'] if n.endswith('.py')}
print(json.dumps({'prepared':len(manifest['files']),'candidate':V68,'previous':V46,'review_leftovers':{k:v for k,v in leftovers.items() if v}},indent=1))
