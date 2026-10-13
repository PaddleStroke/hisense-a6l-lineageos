"""Generate isolated V68 install/restore/inspect tools from the hash-pinned V46 tools. Offline only."""
import hashlib,json,py_compile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];T=ROOT/'tools'
IMAGE=ROOT/'firmware/extracted/recovery-rootedstock-r2-20260921'
V46='6aa00cd02346a3f4312827bccb1db7df1a6ce3dd5147b721d2e43080ccc05ee4'  # here: the V68 tools' candidate
V45='417164b78c3bc7c43eba3039caab240194b5a69000b78737c50c3503f398bcf9'  # here: the V68 tools' predecessor
report=json.loads((IMAGE/'report.json').read_text());V68=report['candidate_sha256']  # = V72 hash in this generator
assert hashlib.sha256((IMAGE/'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()==V68 and report['previous_sha256']==V46
assert json.loads((IMAGE/'captured-abl-validation.json').read_text())['passed']
pins=json.loads((T/'diagnostic-user-v72-tools.json').read_text())['files']
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
manifest={'candidate_sha256':V68,'previous_sha256':V46,'files':{},'sources':{}}
def source(name):
    p=T/name;assert digest(p)==pins[name],('pinned V46 tool changed',name);manifest['sources'][name]=pins[name];return p.read_text()
def v68(text):
    # order matters: the V46 tools name V45 as "previous" and V46 as "candidate"
    text=text.replace(V45,'@@PREV@@').replace(V46,V68).replace('@@PREV@@',V46)
    text=text.replace('verified-v71','@@LABEL@@').replace('verified V71 predecessor','@@PRED@@')
    text=text.replace('recovery-rootedstock-20260921','recovery-rootedstock-r2-20260921')
    return text.replace('V72','V73').replace('v72','v73').replace('@@LABEL@@','verified-v72').replace('@@PRED@@','verified V72 predecessor')
def put(name,text):
    p=T/name;p.write_text(text);py_compile.compile(str(p),doraise=True);manifest['files'][name]=digest(p)
for old in [n for n in pins if n!='diagnostic-user-v72-tools.json']:
    put(old.replace('V72','V73').replace('v72','v73'),v68(source(old)))
put('diagnostic-user-v73-tools.json',json.dumps(manifest,indent=2)+'\n')
leftovers={n:[w for w in ('v45','V45','controls-v46-prep') if w in (T/n).read_text()] for n in manifest['files'] if n.endswith('.py')}
print(json.dumps({'prepared':len(manifest['files']),'candidate':V68,'previous':V46,'review_leftovers':{k:v for k,v in leftovers.items() if v}},indent=1))
