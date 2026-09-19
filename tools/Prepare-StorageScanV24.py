"""Add bounded, sleepable checkpoints around the first MMC rescan."""
from pathlib import Path
import difflib

ROOT = Path(__file__).resolve().parents[1]
KERNEL = Path('/home/a6l/kernel/a6l-mainline')
OUT = ROOT / 'firmware/extracted/storage-scan-source-v24-20260916'

def replace(s, old, new):
    assert s.count(old) == 1, (old, s.count(old))
    return s.replace(old, new)

def instrument(s, function, steps):
    start = s.index(function); end = s.index('\n}', start)+2
    part = s[start:end]
    for anchor, stage in steps:
        indent = anchor[:len(anchor)-len(anchor.lstrip())]
        part = replace(part, '\n'+anchor, '\n'+indent+'a6l_storage_checkpoint(mmc_dev(host), "'+stage+'");\n'+anchor)
    return s[:start]+part+s[end:]

p = KERNEL / 'drivers/mmc/core/core.c'; before = p.read_text()
s = instrument(before, 'void mmc_rescan(', [
    ('\tif (host->rescan_disable)', 'scan_entry'),
    ('\tmmc_claim_host(host);', 'scan_claim_host'),
    ('\tif (mmc_card_is_removable(host) && host->ops->get_cd &&', 'scan_claimed_check_presence'),
    ('\tif (!mmc_attach_sd_uhs2(host)) {', 'scan_uhs2_check'),
    ('\t\tif (!mmc_rescan_try_freq(host, max(freq, host->f_min)))', 'scan_try_frequency'),
])
s = instrument(s, 'static int mmc_rescan_try_freq(', [
    ('\tmmc_power_up(host, host->ocr_avail);', 'scan_power_up'),
    ('\tmmc_hw_reset_for_init(host);', 'scan_hardware_reset'),
    ('\tif (!(host->caps2 & MMC_CAP2_NO_SDIO))\n\t\tsdio_reset(host);', 'scan_sdio_reset'),
    ('\tmmc_go_idle(host);', 'scan_go_idle'),
    ('\tif (!(host->caps2 & MMC_CAP2_NO_SD)) {', 'scan_interface_condition'),
    ('\tif (!(host->caps2 & MMC_CAP2_NO_SDIO))\n\t\tif (!mmc_attach_sdio(host))', 'scan_attach_sdio'),
    ('\tif (!(host->caps2 & MMC_CAP2_NO_SD))\n\t\tif (!mmc_attach_sd(host))', 'scan_attach_sd'),
    ('\tif (!(host->caps2 & MMC_CAP2_NO_MMC))', 'scan_attach_mmc'),
])
s = instrument(s, 'static void mmc_hw_reset_for_init(', [
    ('\tmmc_pwrseq_reset(host);', 'scan_reset_sequence'),
    ('\tif (!(host->caps & MMC_CAP_HW_RESET) || !host->ops->card_hw_reset)', 'scan_reset_capabilities'),
    ('\thost->ops->card_hw_reset(host);', 'scan_reset_card'),
])
changes = [(p, before, s)]
p = KERNEL / 'drivers/base/dd.c'; before = p.read_text()
s = replace(before, '\tif (index > 64)\n', '\tif (index > 96)\n')
changes.append((p, before, s))
OUT.mkdir(exist_ok=False)
for p,before,after in changes:
    (OUT/(p.stem+'-before.c')).write_text(before)
    (OUT/(p.stem+'-after.c')).write_text(after)
    (OUT/(p.stem+'-v24-only.patch')).write_text(''.join(difflib.unified_diff(before.splitlines(True),after.splitlines(True),fromfile=str(p),tofile=str(p))))
    p.write_text(after)
print('V24:16 scan checkpoints; shared bound96 x250ms, no power-setting changes.')
