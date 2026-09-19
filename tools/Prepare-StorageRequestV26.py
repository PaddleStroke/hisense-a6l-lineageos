"""Trace first SDHCI command dispatch without sleeping in atomic contexts."""
from pathlib import Path
import difflib
ROOT=Path(__file__).resolve().parents[1]
KERNEL=Path('/home/a6l/kernel/a6l-mainline')
OUT=ROOT/'firmware/extracted/storage-request-source-v26-20260916'
changes=[]
def replace(s,a,b):
    assert s.count(a)==1,(a,s.count(a))
    return s.replace(a,b)
def function(s,signature,edit):
    a=s.index(signature);b=s.index('\n}',a)+2
    return s[:a]+edit(s[a:b])+s[b:]
def before(s,line,code):
    return replace(s,'\n'+line,'\n'+code+'\n'+line)
def cmd_event(stage,value='cmd->opcode'):
    return '\tif (cmd->opcode == MMC_GO_IDLE_STATE)\n\t\ta6l_storage_event(mmc_dev(host->mmc), "'+stage+'", '+value+');'

p=KERNEL/'include/linux/a6l_probe.h';old=p.read_text()
new=replace(old,'#endif','void a6l_storage_event(struct device *dev, const char *stage, unsigned int value);\n#endif')
changes.append((p,old,new))
p=KERNEL/'drivers/base/dd.c';old=p.read_text()
event='''
/* Diagnostic printk only: no sleeps, allocations or hardware accesses. */
static atomic_t a6l_storage_events = ATOMIC_INIT(0);

void a6l_storage_event(struct device *dev, const char *stage, unsigned int value)
{
	int index;

	if (!a6l_storage_trace || !of_machine_is_compatible("hisense,hlte730t") ||
	    strcmp(dev_name(dev), "c0c4000.mmc"))
		return;
	index = atomic_inc_return(&a6l_storage_events);
	if (index > 64)
		return;
	dev_info(dev, "A6L_STORAGE_EVENT %d %s value=%08x\\n", index, stage, value);
}
EXPORT_SYMBOL_GPL(a6l_storage_event);
'''
new=replace(old,'EXPORT_SYMBOL_GPL(a6l_storage_checkpoint);','EXPORT_SYMBOL_GPL(a6l_storage_checkpoint);\n'+event)
changes.append((p,old,new))
p=KERNEL/'drivers/mmc/core/core.c';old=p.read_text()
def core_edit(s):
    for line,stage in [('\terr = mmc_retune(host);','cmd0_retune'),('\ttrace_mmc_request_start(host, mrq);','cmd0_trace_request'),('\thost->ops->request(host, mrq);','cmd0_host_request')]:
        s=before(s,line,'\tif (mrq->cmd->opcode == MMC_GO_IDLE_STATE)\n\t\ta6l_storage_checkpoint(mmc_dev(host), "'+stage+'");')
    return s
new=function(old,'static void __mmc_start_request(struct mmc_host *host, struct mmc_request *mrq)\n{',core_edit)
changes.append((p,old,new))
p=KERNEL/'drivers/mmc/host/sdhci.c';old=p.read_text()
assert '#include <linux/a6l_probe.h>' in old
def request_edit(s):
    for line,stage in [('\tpresent = mmc->ops->get_cd(mmc);','cmd0_sdhci_get_cd'),('\tspin_lock_irqsave(&host->lock, flags);','cmd0_sdhci_lock')]:
        s=before(s,line,'\tif (mrq->cmd->opcode == MMC_GO_IDLE_STATE)\n\t\ta6l_storage_checkpoint(mmc_dev(mmc), "'+stage+'");')
    for line,stage,value in [('\tsdhci_led_activate(host);','request_locked','host->runtime_suspended'),('\tif (sdhci_present_error(host, mrq->cmd, present))','request_led_done','present'),('\tif (!sdhci_send_command_retry(host, cmd, flags))','request_send','cmd->opcode'),('\tspin_unlock_irqrestore(&host->lock, flags);\n\n\treturn;','request_send_returned','cmd->error')]:
        s=before(s,line,'\tif (mrq->cmd->opcode == MMC_GO_IDLE_STATE)\n\t\ta6l_storage_event(mmc_dev(mmc), "'+stage+'", '+value+');')
    return s
new=function(old,'void sdhci_request(struct mmc_host *mmc, struct mmc_request *mrq)\n{',request_edit)
def send_edit(s):
    s=replace(s,'\tu32 mask;','\tu32 mask, a6l_present;')
    s=before(s,'\tWARN_ON(host->cmd);',cmd_event('send_entry'))
    s=replace(s,'\tif (sdhci_readl(host, SDHCI_PRESENT_STATE) & mask)',cmd_event('present_read_begin', 'mask')+'\n\ta6l_present = sdhci_readl(host, SDHCI_PRESENT_STATE);\n'+cmd_event('present_read_end','a6l_present')+'\n\tif (a6l_present & mask)')
    for line,stage,value in [('\tsdhci_writel(host, cmd->arg, SDHCI_ARGUMENT);','argument_write','cmd->arg'),('\tsdhci_set_transfer_mode(host, cmd);','transfer_mode','cmd->flags'),('\tif ((cmd->flags & MMC_RSP_136) && (cmd->flags & MMC_RSP_BUSY)) {','transfer_mode_done','cmd->flags'),('\tsdhci_mod_timer(host, cmd->mrq, timeout);','timer_arm','0'),('\tsdhci_writew(host, SDHCI_MAKE_CMD(cmd->opcode, flags), SDHCI_COMMAND);','command_write','SDHCI_MAKE_CMD(cmd->opcode, flags)'),('\treturn true;','command_write_returned','0')]:
        s=before(s,line,cmd_event(stage,value))
    return s
new=function(new,'static bool sdhci_send_command(struct sdhci_host *host, struct mmc_command *cmd)\n{',send_edit)
def irq_edit(s):
    s=before(s,'\tspin_lock(&host->lock);','\ta6l_storage_event(mmc_dev(host->mmc), "irq_entry", irq);')
    s=before(s,'\tif (host->runtime_suspended) {','\ta6l_storage_event(mmc_dev(host->mmc), "irq_locked", host->runtime_suspended);')
    s=before(s,'\tif (!intmask || intmask == 0xffffffff) {','\ta6l_storage_event(mmc_dev(host->mmc), "irq_status", intmask);')
    s=before(s,'\t\tsdhci_writel(host, mask, SDHCI_INT_STATUS);','\t\ta6l_storage_event(mmc_dev(host->mmc), "irq_ack", mask);')
    s=before(s,'\t\tif (intmask & SDHCI_INT_CMD_MASK)','\t\ta6l_storage_event(mmc_dev(host->mmc), "irq_command_begin", intmask);')
    s=before(s,'\t\tif (intmask & SDHCI_INT_DATA_MASK)','\t\ta6l_storage_event(mmc_dev(host->mmc), "irq_command_end", intmask);')
    s=before(s,'\tspin_unlock(&host->lock);\n\n\t/* Process mrqs','\ta6l_storage_event(mmc_dev(host->mmc), "irq_unlock", result);')
    return s
new=function(new,'static irqreturn_t sdhci_irq(int irq, void *dev_id)\n{',irq_edit)
changes.append((p,old,new))
OUT.mkdir(exist_ok=False)
for p,old,new in changes:
    (OUT/(p.stem+'-before'+p.suffix)).write_text(old)
    (OUT/(p.stem+'-after'+p.suffix)).write_text(new)
    (OUT/(p.stem+'-v26-only.patch')).write_text(''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile=str(p),tofile=str(p))))
    p.write_text(new)
print('V26:5 process-context pauses; bounded64 non-sleeping events; no extra MMIO reads or hardware-setting changes.')
