"""Archive and fix missing vendor variants exposed by the graphics build graph."""
from pathlib import Path
import subprocess,json
a=Path('/home/a6l/android/a6l-lineage24')
out=Path('/mnt/c/Users/Pierre/Desktop/A6L/research/android-surface-v44-20260917/build-fixes')
out.mkdir(parents=True,exist_ok=False)
targets=[(a/'external/skia/Android.bp','libskia_skcms')]
for p in (a/'external').glob('**/Android.bp'):
    if 'name: "libhwy"' in p.read_text():targets.append((p,'libhwy'));break
assert len(targets)==2
records=[]
for p,module in targets:
    s=p.read_text();needle=f'    name: "{module}",'
    assert s.count(needle)==1
    start=s.index(needle);assert 'vendor_available' not in s[start:s.index('\n}',start)]
    (out/(module+'.before.bp')).write_text(s)
    s=s.replace(needle,needle+'\n    vendor_available: true,')
    p.write_text(s);(out/(module+'.after.bp')).write_text(s)
    records.append({'file':str(p),'module':module})
(out/'changes.json').write_text(json.dumps(records,indent=2)+'\n')
print(records)
