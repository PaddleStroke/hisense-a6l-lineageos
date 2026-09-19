"""Trace CMD0 setup and its sleepable request path without changing hardware settings."""
from pathlib import Path
import difflib
ROOT=Path(__file__).resolve().parents[1]
KERNEL=Path('/home/a6l/kernel/a6l-mainline')
OUT=ROOT/'firmware/extracted/storage-cmd0-source-v25-20260916'
def replace(s,a,b):
    assert s.count(a)==1,(a,s.count(a))
    return s.replace(a,b)
def instrument(s,function,steps,condition=None):
    a=s.index(function);b=s.index('\n}',a)+2
    part=s[a:b]
    for anchor,stage in steps:
        indent=anchor[:len(anchor)-len(anchor.lstrip())]
        trace=indent+'a6l_storage_checkpoint(mmc_dev(host), "'+stage+'");\n'
        if condition:
            trace=indent+'if ('+condition+')\n\t'+trace
        part=replace(part,'\n'+anchor,'\n'+trace+anchor)
    return s[:a]+part+s[b:]
p=KERNEL/'drivers/mmc/core/mmc_ops.c';before=p.read_text()
s=replace(before,'#include <linux/slab.h>','#include <linux/slab.h>\n#include <linux/a6l_probe.h>')
s=instrument(s,'int mmc_go_idle(', [
    ('\t\tmmc_set_chip_select(host, MMC_CS_HIGH);','cmd0_chipselect_high'),
    ('\terr = __mmc_go_idle(host);','cmd0_submit'),
    ('\t\tmmc_set_chip_select(host, MMC_CS_DONTCARE);','cmd0_chipselect_restore'),
    ('\treturn err;','cmd0_go_idle_return'),
])
s=instrument(s,'int __mmc_go_idle(', [
    ('\terr = mmc_wait_for_cmd(host, &cmd, 0);','cmd0_wait_command'),
    ('\tmmc_delay(1);','cmd0_wait_command_returned'),
])
changes=[(p,before,s)]
p=KERNEL/'drivers/mmc/core/core.c';before=p.read_text();s=before
s=instrument(s,'int mmc_wait_for_cmd(', [
    ('\tmmc_wait_for_req(host, &mrq);','cmd0_wait_request'),
    ('\treturn cmd->error;','cmd0_wait_request_returned'),
], 'cmd->opcode == MMC_GO_IDLE_STATE')
s=instrument(s,'void mmc_wait_for_req(', [
    ('\t__mmc_start_req(host, mrq);','cmd0_start_request'),
    ('\tif (!mrq->cap_cmd_during_tfr)','cmd0_wait_completion'),
], 'mrq->cmd->opcode == MMC_GO_IDLE_STATE')
a=s.index('void mmc_wait_for_req(');b=s.index('\n}',a)+2
part=s[a:b]
part=replace(part,'\n}', '\n\tif (mrq->cmd->opcode == MMC_GO_IDLE_STATE)\n\t\ta6l_storage_checkpoint(mmc_dev(host), "cmd0_completion_returned");\n}')
s=s[:a]+part+s[b:]
s=instrument(s,'static int __mmc_start_req(', [
    ('\tmmc_wait_ongoing_tfr_cmd(host);','cmd0_wait_ongoing_transfer'),
    ('\terr = mmc_start_request(host, mrq);','cmd0_mmc_start_request'),
], 'mrq->cmd->opcode == MMC_GO_IDLE_STATE')
s=instrument(s,'int mmc_start_request(', [
    ('\terr = mmc_mrq_prep(host, mrq);','cmd0_prepare_request'),
    ('\tled_trigger_event(host->led, LED_FULL);','cmd0_led_trigger'),
    ('\t__mmc_start_request(host, mrq);','cmd0_dispatch_request'),
], 'mrq->cmd->opcode == MMC_GO_IDLE_STATE')
changes.append((p,before,s))
OUT.mkdir(exist_ok=False)
for p,before,after in changes:
    (OUT/(p.stem+'-before.c')).write_text(before)
    (OUT/(p.stem+'-after.c')).write_text(after)
    (OUT/(p.stem+'-v25-only.patch')).write_text(''.join(difflib.unified_diff(before.splitlines(True),after.splitlines(True),fromfile=str(p),tofile=str(p))))
    p.write_text(after)
print('V25:16 CMD0 checkpoints, unchanged96-pause cap and power settings.')
