"""Read-only consistency check of the final report, source inventories and logs."""
from pathlib import Path
import hashlib,json,re
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
report=ROOT/'docs/hardware-review-round5-20260928.md'
text=report.read_text()
ids=re.findall(r'^### (F\d+) ',text,re.M)
assert ids==['F'+str(i) for i in range(42,51)],ids
for target in re.findall(r'\]\((C:/[^)]+)\)',text+(HERE/'README.md').read_text()):
    target=re.sub(r':\d+$','',target)
    p=Path('/mnt/c/'+target[3:])
    assert p.is_file(),target
changed=[]
for name in ('source-hashes.json','extended-source-hashes.json'):
    for p,h in json.loads((HERE/name).read_text()).items():
        if hashlib.sha256(Path(p).read_bytes()).hexdigest()!=h:changed.append(p)
(HERE/'changed-at-report-finalization.json').write_text(json.dumps(sorted(set(changed)),indent=2)+'\n')
if changed:print('SOURCE_DRIFT: '+str(len(set(changed)))+' files differ from the tested snapshots; see changed-at-report-finalization.json and concurrent-revalidation/STATUS.md')
assert json.loads((HERE/'changed-during-run.json').read_text())==[]
assert json.loads((HERE/'extended-changed-during-run.json').read_text())==[]
assert len(json.loads((HERE/'results.json').read_text()))==4
assert len(json.loads((HERE/'extended-results.json').read_text()))==2
assert 'process_returncode=-6 (SIGABRT)' in (HERE/'apdu-offset1.log').read_text()
print('REPORT_EVIDENCE_PASS: F42-F50, all local links exist, reproduction logs present; current-source drift reported separately')
