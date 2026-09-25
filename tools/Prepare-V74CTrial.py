"""Generate the hash-pinned install/restore/inspect tools for the V74 candidate (agent v74img, 23 Sep 2026). Offline only.

Tool suffix "v74c" (not "v74"): the laptop already holds capture-diagnostic-install-user-v74 from the 21 Sep V71 reinstall,
so the v74 names are used up. Two sets are generated from the pinned V74 (= V71-reinstall) tools:
  v74c  install V74 candidate   when the phone recovery is exactly V71 (417164b7...); restore mode = stock recovery.
  v74r  ROLLBACK: reinstall V71 when the phone recovery is exactly the V74 candidate.
Nothing here opens USB; the generated tools only run when Pierre launches them on the laptop."""
import hashlib,json,py_compile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];T=ROOT/'tools'
IMAGE=ROOT/'firmware/extracted/recovery-v74-candidate-20260923'
V71='417164b78c3bc7c43eba3039caab240194b5a69000b78737c50c3503f398bcf9'   # candidate of the pinned v74 tools
V73='ab556daee50e1ef4ccb6f92f1df0ae3ad75772536a4ae823d3b2aa677e43e0a2'   # predecessor of the pinned v74 tools
report=json.loads((IMAGE/'report.json').read_text());NEW=report['candidate_sha256']
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert digest(IMAGE/'recovery-diagnostic-unsigned.img')==NEW and report['previous_sha256']==V71
assert json.loads((IMAGE/'captured-abl-validation.json').read_text())['passed']
assert digest(ROOT/'firmware/extracted/recovery-v71-candidate-20260921/recovery-diagnostic-unsigned.img')==V71
pinned=json.loads((T/'diagnostic-user-v74-tools.json').read_text())
assert pinned['candidate_sha256']==V71 and pinned['previous_sha256']==V73
SETS={'v74c':dict(candidate=NEW,previous=V71,label='v71',image='recovery-v74-candidate-20260923'),
      'v74r':dict(candidate=V71,previous=NEW,label='v74c',image='recovery-v71-candidate-20260921')}
out={}
for tag,cfg in SETS.items():
    manifest={'candidate_sha256':cfg['candidate'],'previous_sha256':cfg['previous'],'files':{},'sources':{}}
    for old,pin in pinned['files'].items():
        p=T/old;assert digest(p)==pin,('pinned v74 tool changed',old);manifest['sources'][old]=pin
        text=p.read_text()
        text=text.replace(V73,'@@PREV@@').replace(V71,'@@CAND@@').replace('@@PREV@@',cfg['previous']).replace('@@CAND@@',cfg['candidate'])
        text=text.replace('verified-v73','@@LABEL@@').replace('verified V73 predecessor','@@PRED@@')
        text=text.replace('recovery-v71-reinstall-20260921','@@IMAGE@@')
        assert 'V73' not in text and 'v73' not in text,(old,'unexpected v73 reference')
        text=text.replace('V74',tag.upper()).replace('v74',tag)
        text=text.replace('@@LABEL@@','verified-'+cfg['label']).replace('@@PRED@@','verified '+cfg['label'].upper()+' predecessor').replace('@@IMAGE@@',cfg['image'])
        name=old.replace('V74',tag.upper()).replace('v74',tag);q=T/name
        assert not q.exists() or q.read_text()==text,('refusing to overwrite a different existing file',name)
        q.write_text(text);py_compile.compile(str(q),doraise=True);manifest['files'][name]=digest(q)
    m=T/f'diagnostic-user-{tag}-tools.json';m.write_text(json.dumps(manifest,indent=2)+'\n')
    out[tag]={'files':len(manifest['files']),'candidate':cfg['candidate'],'previous':cfg['previous']}
print(json.dumps(out,indent=1))
