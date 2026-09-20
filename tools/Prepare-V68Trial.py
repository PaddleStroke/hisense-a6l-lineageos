"""Generate isolated V68 install/restore/inspect tools from the hash-pinned V46 tools. Offline only."""
import hashlib,json,py_compile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];T=ROOT/'tools'
IMAGE=ROOT/'firmware/extracted/recovery-v68-candidate-20260920'
V46='ce3727dda592065becb883ef3fe663cb3579290edb841854e01f4fc49d02a3c6'
V45='aae86f0da6f15a0c1f6e425e88806edb165289c459462b232f0b8773f5e4ac14'
report=json.loads((IMAGE/'report.json').read_text());V68=report['candidate_sha256']
assert hashlib.sha256((IMAGE/'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()==V68 and report['previous_sha256']==V46
assert json.loads((IMAGE/'captured-abl-validation.json').read_text())['passed']
pins=json.loads((T/'diagnostic-user-v46-tools.json').read_text())['files']
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
manifest={'candidate_sha256':V68,'previous_sha256':V46,'files':{},'sources':{}}
def source(name):
    p=T/name;assert digest(p)==pins[name],('pinned V46 tool changed',name);manifest['sources'][name]=pins[name];return p.read_text()
def v68(text):
    # order matters: the V46 tools name V45 as "previous" and V46 as "candidate"
    text=text.replace(V45,'@@PREV@@').replace(V46,V68).replace('@@PREV@@',V46)
    text=text.replace('verified-v45','@@LABEL@@').replace('verified V45 predecessor','@@PRED@@')
    text=text.replace('recovery-controls-v46-20260918','recovery-v68-candidate-20260920')
    return text.replace('V46','V68').replace('v46','v68').replace('@@LABEL@@','verified-v46').replace('@@PRED@@','verified V46 predecessor')
def put(name,text):
    p=T/name;p.write_text(text);py_compile.compile(str(p),doraise=True);manifest['files'][name]=digest(p)
for old in pins:
    put(old.replace('V46','V68').replace('v46','v68'),v68(source(old)))
put('diagnostic-user-v68-tools.json',json.dumps(manifest,indent=2)+'\n')
leftovers={n:[w for w in ('v45','V45','controls-v46-prep') if w in (T/n).read_text()] for n in manifest['files'] if n.endswith('.py')}
print(json.dumps({'prepared':len(manifest['files']),'candidate':V68,'previous':V46,'review_leftovers':{k:v for k,v in leftovers.items() if v}},indent=1))
