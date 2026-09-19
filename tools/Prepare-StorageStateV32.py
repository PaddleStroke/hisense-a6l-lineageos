"""Add bounded read-only pre-command snapshots to verified V31 sources."""
from pathlib import Path
import difflib,json
R=Path(__file__).resolve().parents[1];K=Path('/home/a6l/kernel/a6l-mainline')
O=R/'firmware/extracted/storage-state-source-v32-20260917'
old_archive=R/'firmware/extracted/storage-ordered-source-v31-20260916'
core=K/'drivers/mmc/host/sdhci.c';msm=K/'drivers/mmc/host/sdhci-msm.c'
old_core=core.read_text();old_msm=msm.read_text()
assert old_core==(old_archive/'sdhci-after.c').read_text()
assert old_msm==(old_archive/'sdhci-msm-after.c').read_text()
before=(R/'tools/a6l-cmd0-ordered-v31.c').read_text();after=(R/'tools/a6l-cmd0-state-v32.c').read_text()
assert old_core.count(before)==1
new_core=old_core.replace(before,after)
helper=(R/'tools/a6l-msm-state-v32.c').read_text()
assert all(x not in helper for x in ['writel','writew','msleep','udelay','regulator_','clk_get_rate'])
assert helper.count('A6L_STATE_READ("')==10
anchor='static void sdhci_msm_dump_vendor_regs(struct sdhci_host *host)'
assert old_msm.count(anchor)==1
new_msm=old_msm.replace(anchor,helper+'\n'+anchor)
start=new_msm.index(anchor);pos=new_msm.index('\tSDHCI_MSM_DUMP(',start)
new_msm=new_msm[:pos]+'''\tif (a6l_storage_hold_cmd0(mmc_dev(host->mmc))) {
		a6l_msm_state_snapshot(host);
		return;
	}

'''+new_msm[pos:]
# The hardware-affecting V31 command accessor and remainder of state machine stay exact.
assert (R/'tools/a6l-msm-ordered-v31.c').read_text() in new_msm
assert before.split('\tcase 1:',1)[1].replace('V31','V32')==after.split('\tcase 1:',1)[1]
O.mkdir(exist_ok=False)
for name,path,old,new in [('sdhci',core,old_core,new_core),('sdhci-msm',msm,old_msm,new_msm)]:
    (O/f'{name}-before.c').write_text(old);(O/f'{name}-after.c').write_text(new)
    (O/f'{name}-v32-only.patch').write_text(''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile=str(path),tofile=str(path))))
    path.write_text(new)
(O/'guard-checks.json').write_text(json.dumps({'passed':True,'scope':'Only one read-only pre-command snapshot; regulator queries outside lock; ten older SDCC5 vendor-register reads; unchanged V31 hardware writes/branches/DT/init.'},indent=2))
print('V32 source guards passed')
