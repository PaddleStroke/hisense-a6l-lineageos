"""Trace the sleepable MMC host-registration and initial power-up interval."""
from pathlib import Path
import difflib
ROOT = Path(__file__).resolve().parents[1]
KERNEL = Path('/home/a6l/kernel/a6l-mainline')
OUT = ROOT / 'firmware/extracted/storage-host-source-v23-20260916'

def replace(s, old, new):
    assert s.count(old) == 1, (old, s.count(old))
    return s.replace(old, new)

def instrument(s, function, steps):
    start = s.index(function); end = s.index('\n}', start)+2
    part = s[start:end]
    for anchor, stage in steps:
        indent = anchor[:len(anchor)-len(anchor.lstrip())]
        part = replace(part, anchor, indent+'a6l_storage_checkpoint(mmc_dev(host), "'+stage+'");\n'+anchor)
    return s[:start]+part+s[end:]

changes = []
p = KERNEL / 'drivers/mmc/core/host.c'; before = p.read_text()
s = replace(before, '#include <linux/device.h>', '#include <linux/device.h>\n#include <linux/a6l_probe.h>')
s = instrument(s, 'int mmc_add_host(', [
    ('\terr = mmc_validate_host_caps(host);', 'mmc_validate_caps'),
    ('\terr = device_add(&host->class_dev);', 'mmc_device_add'),
    ('\tled_trigger_register_simple(', 'mmc_led_trigger'),
    ('\tmmc_add_host_debugfs(host);', 'mmc_debugfs'),
    ('\tmmc_start_host(host);', 'mmc_start'),
    ('\treturn 0;', 'mmc_add_return'),
])
changes.append((p,before,s))
p = KERNEL / 'drivers/mmc/core/core.c'; before=p.read_text()
s = replace(before, '#include <linux/delay.h>', '#include <linux/delay.h>\n#include <linux/a6l_probe.h>')
s = instrument(s, 'void mmc_start_host(', [
    ('\t\tmmc_claim_host(host);', 'mmc_claim_before_power'),
    ('\t\tmmc_power_up(host, host->ocr_avail);', 'mmc_initial_power_up'),
    ('\t\tmmc_release_host(host);', 'mmc_initial_power_returned'),
    ('\tmmc_gpiod_request_cd_irq(host);', 'mmc_card_detect_irq'),
    ('\t_mmc_detect_change(host, 0, false);', 'mmc_schedule_scan'),
])
s = instrument(s, 'void mmc_power_up(', [
    ('\tmmc_pwrseq_pre_power_on(host);', 'power_sequence_pre'),
    ('\tmmc_set_initial_state(host);', 'power_initial_ios'),
    ('\tmmc_set_initial_signal_voltage(host);', 'power_initial_signal_voltage'),
    ('\tmmc_pwrseq_post_power_on(host);', 'power_sequence_post'),
    ('\tmmc_set_ios(host);', 'power_clock_on_ios'),
])
changes.append((p,before,s))
OUT.mkdir(exist_ok=False)
for p,before,after in changes:
    name=p.stem
    (OUT/(name+'-before.c')).write_text(before)
    (OUT/(name+'-after.c')).write_text(after)
    (OUT/(name+'-v23-only.patch')).write_text(''.join(difflib.unified_diff(before.splitlines(True),after.splitlines(True),fromfile=name+'-before.c',tofile=name+'-after.c')))
    p.write_text(after)
print('V23:16 additional MMC checkpoints; shared64-pause cap, no power-setting changes.')
