"""Narrow V20's last checkpoint inside sleepable SDHCI setup paths."""
from pathlib import Path
import difflib

ROOT = Path(__file__).resolve().parents[1]
KERNEL = Path('/home/a6l/kernel/a6l-mainline')
OUT = ROOT / 'firmware/extracted/storage-core-source-v21-20260916'
path = KERNEL / 'drivers/mmc/host/sdhci.c'
before = path.read_text()

def replace(s, old, new):
    assert s.count(old) == 1, (old, s.count(old))
    return s.replace(old, new)

s = replace(before, '#include <linux/delay.h>', '#include <linux/delay.h>\n#include <linux/a6l_probe.h>')

def instrument(s, function, steps):
    start = s.index(function)
    end = s.index('\n}', start) + 2
    part = s[start:end]
    for anchor, stage in steps:
        indent = anchor[:len(anchor)-len(anchor.lstrip())]
        part = replace(part, anchor, indent + 'a6l_storage_checkpoint(mmc_dev(host->mmc), "' + stage + '");\n' + anchor)
    return s[:start]+part+s[end:]

s = instrument(s, 'int sdhci_setup_host(', [
    ('\tif (!mmc->supply.vqmmc) {', 'core_setup_supplies'),
    ('\tsdhci_read_caps(host);', 'core_read_caps'),
    ('\toverride_timeout_clk = host->timeout_clk;', 'core_caps_read_dma_setup'),
    ('\tif (host->flags & (SDHCI_USE_SDMA | SDHCI_USE_ADMA)) {', 'core_dma_mask'),
    ('\t\tbuf = dma_alloc_coherent(', 'core_alloc_adma'),
    ('\tif (host->version >= SDHCI_SPEC_300)\n\t\thost->max_clk', 'core_clock_limits'),
    ('\tif (!IS_ERR(mmc->supply.vqmmc)) {', 'core_vqmmc_constraints'),
    ('\tmax_current_caps = sdhci_readl(', 'core_current_capabilities'),
    ('\tspin_lock_init(&host->lock);', 'core_request_limits'),
    ('\tif (mmc->max_segs == 1)\n', 'core_bounce_buffer'),
    ('\treturn 0;\n\nunreg:', 'core_setup_complete'),
])
# On this board, the first capability read occurs in sdhci_setup_host, before
# host registration. Subsequent calls return early when read_caps is true.
s = instrument(s, 'void __sdhci_read_caps(', [
    ('\tsdhci_reset_for_all(host);', 'core_caps_reset_all'),
    ('\tif (host->v4_mode)', 'core_caps_reset_returned'),
    ('\tv = ver ? *ver :', 'core_caps_read_version'),
    ('\tif (caps) {', 'core_caps_read_registers'),
])
s = instrument(s, 'int __sdhci_add_host(', [
    ('\thost->complete_wq = alloc_workqueue(', 'core_create_workqueue'),
    ('\tsdhci_init(host, 0);', 'core_initialize_controller'),
    ('\tret = request_threaded_irq(', 'core_request_controller_irq'),
    ('\tret = sdhci_led_register(host);', 'core_register_led'),
    ('\tret = mmc_add_host(mmc);', 'core_register_mmc_host'),
    ('\tsdhci_enable_card_detection(host);', 'core_enable_detection'),
])
OUT.mkdir(exist_ok=False)
(OUT / 'sdhci-before.c').write_text(before)
(OUT / 'sdhci-after.c').write_text(s)
(OUT / 'v21-only.patch').write_text(''.join(difflib.unified_diff(before.splitlines(True), s.splitlines(True), fromfile='sdhci-before.c', tofile='sdhci-after.c')))
path.write_text(s)
print('V21: 21 new checkpoint sites; same A6L/device/opt-in gate and shared64-pause limit.')
