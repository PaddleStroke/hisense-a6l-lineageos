"""Apply V29 staged first-command experiment to the exact V28 source."""
from pathlib import Path
import difflib, json
ROOT=Path(__file__).resolve().parents[1]
K=Path('/home/a6l/kernel/a6l-mainline')
BASE=ROOT/'firmware/extracted/storage-hold-source-v28-20260916'
OUT=ROOT/'firmware/extracted/storage-stages-source-v29-20260916'
changes=[]
def replace(s,a,b):
    assert s.count(a)==1,(a,s.count(a))
    return s.replace(a,b)
p=K/'drivers/mmc/host/sdhci.c';old=p.read_text()
assert old==(BASE/'sdhci-after.c').read_text()
block=(ROOT/'tools/a6l-cmd0-stages-v29.c').read_text()
anchor='static bool sdhci_send_command(struct sdhci_host *host, struct mmc_command *cmd)\n{'
new=replace(old,anchor,block+'\n'+anchor)
start=new.index('\t/* V28 diagnostic:')
end=new.index('\n\tsdhci_writew(host, SDHCI_MAKE_CMD',start)
new=new[:start]+'''\tif (a6l_cmd0_stage(host, cmd, SDHCI_MAKE_CMD(cmd->opcode, flags)))
\t\treturn true;
\ta6l_follow_command(host, "issue", cmd);
'''+new[end:]
anchor='\tif (cmd->mrq->cap_cmd_during_tfr && cmd == cmd->mrq->cmd)'
new=replace(new,anchor,'\ta6l_follow_command(host, "command_complete", cmd);\n\n'+anchor)
changes.append((p,old,new))
p=K/'drivers/base/dd.c';old=p.read_text()
assert old==(BASE/'dd-after.c').read_text()
new=replace(old,'pause_ms=250\\n','pause_ms=0\\n')
new=replace(new,'\tmsleep(250);','\t/* V29: known-good setup no longer needs per-checkpoint delays. */')
new=replace(new,'if (index > 64)','if (index > 256)')
changes.append((p,old,new))
assert not any(x in block for x in ['msleep(', 'udelay(', 'mmc_request_done(', 'SDHCI_INT_STATUS);\n\tsdhci_write'])
assert block.count('sdhci_writew(')==1
assert block.count('sdhci_writel(')==2
OUT.mkdir(exist_ok=False)
for p,old,new in changes:
    (OUT/(p.stem+'-before'+p.suffix)).write_text(old)
    (OUT/(p.stem+'-after'+p.suffix)).write_text(new)
    (OUT/(p.stem+'-v29-only.patch')).write_text(''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile=str(p),tofile=str(p))))
    p.write_text(new)
(OUT/'scope.json').write_text(json.dumps({'passed':True,'scope':'First CMD0: baseline, mask, issue, bounded status branch, normal IRQ only after success. Four-second delayed work; no waits under spinlock. Existing setup pauses removed. No RAM init/DT changes.'},indent=2))
print('V29 staged source prepared')
