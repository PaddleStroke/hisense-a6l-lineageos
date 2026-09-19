"""V28 RAM diagnostic: hold first CMD0 immediately before command register write."""
from pathlib import Path
import difflib,json
ROOT=Path(__file__).resolve().parents[1]
K=Path('/home/a6l/kernel/a6l-mainline')
OUT=ROOT/'firmware/extracted/storage-hold-source-v28-20260916'
changes=[]
def replace(s,a,b):
    assert s.count(a)==1,(a,s.count(a))
    return s.replace(a,b)
p=K/'include/linux/a6l_probe.h';old=p.read_text()
new=replace(old,'#endif','bool a6l_storage_hold_cmd0(struct device *dev);\n#endif')
changes.append((p,old,new))
p=K/'drivers/base/dd.c';old=p.read_text()
new=replace(old,'EXPORT_SYMBOL_GPL(a6l_storage_event);','EXPORT_SYMBOL_GPL(a6l_storage_event);\n\n/* Temporary RAM-only diagnostic; the opted-in request remains pending. */\nbool a6l_storage_hold_cmd0(struct device *dev)\n{\n\treturn a6l_storage_trace &&\n\t       of_machine_is_compatible("hisense,hlte730t") &&\n\t       !strcmp(dev_name(dev), "c0c4000.mmc");\n}\nEXPORT_SYMBOL_GPL(a6l_storage_hold_cmd0);')
changes.append((p,old,new))
p=K/'drivers/mmc/host/sdhci.c';old=p.read_text()
a='\tsdhci_writew(host, SDHCI_MAKE_CMD(cmd->opcode, flags), SDHCI_COMMAND);'
b='\t/* V28 diagnostic: finish setup but do not launch CMD0. The RAM\n\t * logger keeps running; this request is deliberately never completed.\n\t * Cancel the just-armed timer so it cannot reset the controller later.\n\t * No sleeps, lock dropping, simulated success or extra MMIO here.\n\t */\n\tif (cmd->opcode == MMC_GO_IDLE_STATE &&\n\t    a6l_storage_hold_cmd0(mmc_dev(host->mmc))) {\n\t\tsdhci_del_timer(host, cmd->mrq);\n\t\ta6l_storage_event(mmc_dev(host->mmc), "cmd0_command_held", 0);\n\t\treturn true;\n\t}\n\n\tsdhci_writew(host, SDHCI_MAKE_CMD(cmd->opcode, flags), SDHCI_COMMAND);'
new=replace(old,a,b)
changes.append((p,old,new))
OUT.mkdir(exist_ok=False)
for p,old,new in changes:
    (OUT/(p.stem+'-before'+p.suffix)).write_text(old)
    (OUT/(p.stem+'-after'+p.suffix)).write_text(new)
    (OUT/(p.stem+'-v28-only.patch')).write_text(''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile=str(p),tofile=str(p))))
    p.write_text(new)
check={'passed':True,'scope':'CMD0-only board/device/diagnostic-flag gated stop before SDHCI_COMMAND; timer canceled; request intentionally pending, no storage success claimed','no_new_mmio':new.count('sdhci_read')==old.count('sdhci_read') and new.count('sdhci_write')==old.count('sdhci_write'),'no_sleep_added':all(x not in b for x in ['msleep(', 'udelay(', 'spin_unlock'])}
assert check['no_new_mmio'] and check['no_sleep_added']
(OUT/'hold-guard-checks.json').write_text(json.dumps(check,indent=2))
print(json.dumps(check))
