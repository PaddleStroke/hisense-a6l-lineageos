"""Generate the hash-pinned install/rollback tools for the V75-usb recovery candidate (agent usbrec, 26 Sep 2026). Offline only.

Model: exactly the V74C tools (tools/Prepare-V74CTrial.py), re-derived from the pinned v74c set (diagnostic-user-v74c-tools.json)
with only the hashes / labels / tags changed:
  v75u  install V75-usb (8ecb8e9e...) when the phone recovery is exactly V74 (24ede49b...); restore mode = stock recovery.
  v75r  ROLLBACK: reinstall V74 when the phone recovery is exactly V75-usb.
Same safety chain as V74C: stock Android fingerprint + boot_completed, port 3-2, battery >= 40 %, desktop inhibitor, host pause
(fwupd/ModemManager) + resume, Sahara/GPT/devinfo/BCB/vbmeta checks, exact predecessor sha, only the recovery partition programmed,
full readback, poweroff. Nothing here opens USB; the generated tools only run when Pierre launches them on the laptop."""
import hashlib,json,py_compile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];T=ROOT/'tools'
IMAGE=ROOT/'firmware/extracted/recovery-v75usb-candidate-20260925'
V71='417164b78c3bc7c43eba3039caab240194b5a69000b78737c50c3503f398bcf9'   # predecessor of the pinned v74c tools
V74='24ede49b0f34b10f216617cb007c757e495fa2df1b6cd1ddd0afd63f9819da62'   # candidate of the pinned v74c tools
V75U='8ecb8e9e4c5289cfe63c23eb32b749178935a6588673cea3a1b1ebee3c94b304'
report=json.loads((IMAGE/'report.json').read_text())
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert report['candidate_sha256']==V75U and report['previous_sha256']==V74
assert digest(IMAGE/'recovery-diagnostic-unsigned.img')==V75U and (IMAGE/'recovery-diagnostic-unsigned.img').stat().st_size==67108864
assert json.loads((IMAGE/'captured-abl-validation.json').read_text())['passed']
assert digest(ROOT/'firmware/extracted/recovery-v74-candidate-20260923/recovery-diagnostic-unsigned.img')==V74
pinned=json.loads((T/'diagnostic-user-v74c-tools.json').read_text())
assert pinned['candidate_sha256']==V74 and pinned['previous_sha256']==V71
SETS={'v75u':dict(candidate=V75U,previous=V74,label='v74',image='recovery-v75usb-candidate-20260925'),
      'v75r':dict(candidate=V74,previous=V75U,label='v75u',image='recovery-v74-candidate-20260923')}
OLD_V74C_IMG="('restore-stock', 'ram-staging/recovery-diagnostic-staged-usb-v74c.img')]"
out={}
for tag,cfg in SETS.items():
    manifest={'candidate_sha256':cfg['candidate'],'previous_sha256':cfg['previous'],'files':{},'sources':{}}
    for old,pin in pinned['files'].items():
        p=T/old;assert digest(p)==pin,('pinned v74c tool changed',old);manifest['sources'][old]=pin
        text=p.read_text()
        if tag=='v75u' and OLD_V74C_IMG in text:
            # the v75u install mode must also refuse the V74 image (the v74c staged file stays on the laptop)
            text=text.replace(OLD_V74C_IMG,"('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-@@OLDTAG@@.img'), "+OLD_V74C_IMG)
        text=text.replace(V71,'@@PREV@@').replace(V74,'@@CAND@@').replace('@@PREV@@',cfg['previous']).replace('@@CAND@@',cfg['candidate'])
        text=text.replace('verified-v71','@@LABEL@@').replace('verified V71 predecessor','@@PRED@@')
        text=text.replace('recovery-v74-candidate-20260923','@@IMAGE@@')
        text=text.replace('V74C',tag.upper()).replace('v74c',tag)
        rest=[w for w in ('V71','v71','V74','v74') if w in text]
        assert not rest,(old,'unexpected leftover reference',rest)
        text=text.replace('@@LABEL@@','verified-'+cfg['label']).replace('@@PRED@@','verified '+cfg['label'].upper()+' predecessor')
        text=text.replace('@@IMAGE@@',cfg['image']).replace('@@OLDTAG@@','v74c')
        name=old.replace('V74C',tag.upper()).replace('v74c',tag);q=T/name
        assert not q.exists() or q.read_text()==text,('refusing to overwrite a different existing file',name)
        q.write_text(text);py_compile.compile(str(q),doraise=True);manifest['files'][name]=digest(q)
    m=T/f'diagnostic-user-{tag}-tools.json';m.write_text(json.dumps(manifest,indent=2)+'\n')
    out[tag]={'files':len(manifest['files']),'candidate':cfg['candidate'][:16],'previous':cfg['previous'][:16]}
print(json.dumps(out,indent=1));print('V75U_TOOLS_GENERATED')
