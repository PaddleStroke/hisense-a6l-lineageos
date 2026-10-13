"""Apply the bounded V31 first-CMD0 store-barrier experiment offline."""
from pathlib import Path
import difflib, json, hashlib
ROOT=Path(__file__).resolve().parents[1]
K=Path('/home/a6l/kernel/a6l-mainline')
OUT=ROOT/'firmware/extracted/storage-ordered-source-v31-20260916'
core=K/'drivers/mmc/host/sdhci.c'
msm=K/'drivers/mmc/host/sdhci-msm.c'
old_core=core.read_text(); old_msm=msm.read_text()
assert old_core==(ROOT/'firmware/extracted/storage-commit-source-v30-20260916/sdhci-after.c').read_text()
before=(ROOT/'tools/a6l-cmd0-commit-v30.c').read_text()
after=(ROOT/'tools/a6l-cmd0-ordered-v31.c').read_text()
assert before.replace('V30','V31')==after and old_core.count(before)==1
new_core=old_core.replace(before,after)
start=old_msm.index('/* This function may sleep*/\nstatic void sdhci_msm_writew(')
end=old_msm.index('/* This function may sleep*/\nstatic void sdhci_msm_writeb(',start)
old_wrapper=old_msm[start:end]
assert old_wrapper.count('writew_relaxed(')==1 and old_wrapper.count('__sdhci_msm_check_write(')==1
wrapper=(ROOT/'tools/a6l-msm-ordered-v31.c').read_text()
assert wrapper.count('writew_relaxed(')==1 and wrapper.count('wmb();')==1
assert all(x not in wrapper for x in ['readl(', 'readw(', 'msleep(', 'udelay(', 'mmc_request_done('])
assert wrapper.index('__sdhci_msm_check_write(')<wrapper.index('wmb();')<wrapper.index('writew_relaxed(')
new_msm=old_msm[:start]+wrapper+'\n'+old_msm[end:]
OUT.mkdir(exist_ok=False)
for name,p,old,new in [('sdhci',core,old_core,new_core),('sdhci-msm',msm,old_msm,new_msm)]:
    (OUT/f'{name}-before.c').write_text(old)
    (OUT/f'{name}-after.c').write_text(new)
    (OUT/f'{name}-v31-only.patch').write_text(''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile=str(p),tofile=str(p))))
    p.write_text(new)
(OUT/'guard-checks.json').write_text(json.dumps({'passed':True,'scope':'Same staged state machine; one first-CMD0 wmb in Qualcomm accessor, software-only early CDR trace, all original side effects preserved; no DT, voltage or timing changes.','msm_before_sha256':hashlib.sha256(old_msm.encode()).hexdigest()},indent=2))
print('V31 source guards passed')
