"""Replace the verified V29 staging block with the V30 isolated commit stage."""
from pathlib import Path
import difflib,json
ROOT=Path(__file__).resolve().parents[1]
K=Path('/home/a6l/kernel/a6l-mainline')
OUT=ROOT/'firmware/extracted/storage-commit-source-v30-20260916'
p=K/'drivers/mmc/host/sdhci.c'
old=p.read_text()
assert old==(ROOT/'firmware/extracted/storage-stages-source-v29-20260916/sdhci-after.c').read_text()
before=(ROOT/'tools/a6l-cmd0-stages-v29.c').read_text()
after=(ROOT/'tools/a6l-cmd0-commit-v30.c').read_text()
assert old.count(before)==1
assert not any(s in after for s in ['msleep(', 'udelay(', 'mmc_request_done('])
assert after.count('sdhci_writew(')==1 and after.count('sdhci_writel(')==2
commit=after.split('\tcase 2:\n',1)[1].split('\tcase 3:',1)[0]
assert 'sdhci_read' not in commit
assert commit.count('sdhci_writew(')==1
new=old.replace(before,after)
OUT.mkdir(exist_ok=False)
(OUT/'sdhci-before.c').write_text(old)
(OUT/'sdhci-after.c').write_text(new)
(OUT/'sdhci-v30-only.patch').write_text(''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile=str(p),tofile=str(p))))
p.write_text(new)
(OUT/'guard-checks.json').write_text(json.dumps({'passed':True,'scope':'One additional four-second delayed-work stage between pre-issue validation and isolated command write; software PM-state trace; no extra MMIO in commit stage, no register ordering or hardware configuration changes.'},indent=2))
print('V30 source and isolated-command guards passed')
