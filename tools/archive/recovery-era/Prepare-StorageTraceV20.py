"""Add bounded, opt-in, sleepable A6L storage probe checkpoints in WSL."""
from pathlib import Path
import hashlib
import subprocess

ROOT = Path(__file__).resolve().parents[1]
KERNEL = Path('/home/a6l/kernel/a6l-mainline')
OUT = ROOT / 'firmware/extracted/storage-trace-source-v20-r4-20260916'
OUT.mkdir(exist_ok=False)

def replace(s, old, new):
    assert s.count(old) == 1, (old, s.count(old))
    return s.replace(old, new)

dd = KERNEL / 'drivers/base/dd.c'
msm = KERNEL / 'drivers/mmc/host/sdhci-msm.c'
init = ROOT / 'device/hisense/a6l/diagnostic/init.c'
for name, p in [('dd-before.c', dd), ('sdhci-msm-before.c', msm), ('init-before.c', init)]:
    (OUT / name).write_bytes(p.read_bytes())

s = dd.read_text()
s = replace(s, '#include <linux/debugfs.h>', '#include <linux/debugfs.h>\n#include <linux/of.h>\n#include <linux/a6l_probe.h>')
helper = '''/* Temporary diagnostic only: all call sites are sleepable probe paths.
 * No register access or power-policy change; bound total added delay to 16s.
 */
static bool a6l_storage_trace;
static atomic_t a6l_storage_checkpoints = ATOMIC_INIT(0);

static int __init a6l_storage_trace_setup(char *value)
{
	return kstrtobool(value, &a6l_storage_trace);
}
early_param("a6l_storage_trace", a6l_storage_trace_setup);

void a6l_storage_checkpoint(struct device *dev, const char *stage)
{
	int index;

	if (!a6l_storage_trace || !of_machine_is_compatible("hisense,hlte730t") ||
	    strcmp(dev_name(dev), "c0c4000.mmc"))
		return;
	index = atomic_inc_return(&a6l_storage_checkpoints);
	if (index > 64)
		return;
	dev_info(dev, "A6L_STORAGE_CHECKPOINT %d %s pause_ms=250\\n", index, stage);
	msleep(250);
}
EXPORT_SYMBOL_GPL(a6l_storage_checkpoint);

'''
s = replace(s, 'static int call_driver_probe(', helper + 'static int call_driver_probe(')
for anchor, label in [
    ('\tlink_ret = device_links_check_suppliers(dev);', 'check_suppliers'),
    ('\tret = pinctrl_bind_pins(dev);', 'pinctrl_bind'),
    ('\t\tret = dev->bus->dma_configure(dev);', 'dma_configure'),
    ('\t\tret = dev->pm_domain->activate(dev);', 'power_domain_activate'),
    ('\tret = call_driver_probe(dev, drv);', 'call_driver_probe'),
    ('\tpm_runtime_get_suppliers(dev);', 'runtime_get_suppliers'),
    ('\tpm_runtime_barrier(dev);', 'runtime_barrier'),
]:
    indent = anchor[:len(anchor)-len(anchor.lstrip())]
    s = replace(s, anchor, indent + f'a6l_storage_checkpoint(dev, "{label}");\n' + anchor)
s = replace(s, '\tpm_runtime_get_suppliers(dev);\n\tif (dev->parent)',
    '\tpm_runtime_get_suppliers(dev);\n\ta6l_storage_checkpoint(dev, "runtime_get_parent");\n\tif (dev->parent)')
dd.write_text(s)
header = KERNEL / 'include/linux/a6l_probe.h'
assert not header.exists()
header.write_text('''/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_A6L_PROBE_H
#define _LINUX_A6L_PROBE_H
struct device;
void a6l_storage_checkpoint(struct device *dev, const char *stage);
#endif
''')
s = msm.read_text()
s = replace(s, '#include <linux/module.h>', '#include <linux/module.h>\n#include <linux/a6l_probe.h>')
start, end = s.index('static int sdhci_msm_probe('), s.index('static void sdhci_msm_remove(')
probe = s[start:end]
for anchor, label in [
    ('\thost = sdhci_pltfm_init(', 'platform_init_map'),
    ('\tret = mmc_of_parse(', 'parse_mmc_dt'),
    ('\tret = sdhci_msm_gcc_reset(', 'gcc_reset'),
    ('\tmsm_host->bus_clk = devm_clk_get(', 'bus_clock_get_enable'),
    ('\tclk = devm_clk_get(&pdev->dev, "iface");', 'get_iface_core_clocks'),
    ('\tret = dev_pm_opp_of_find_icc_paths(', 'opp_interconnect_paths'),
    ('\tret = devm_pm_opp_set_clkname(', 'opp_set_clock'),
    ('\tret = devm_pm_opp_of_add_table(', 'opp_add_table'),
    ('\tret = dev_pm_opp_set_rate(', 'opp_boost_clock'),
    ('\tret = clk_bulk_prepare_enable(', 'enable_bulk_clocks'),
    ('\tmsm_host->xo_clk = devm_clk_get(', 'get_xo_map_core'),
    ('\twritel_relaxed(CORE_VENDOR_SPEC_POR_VAL,', 'write_vendor_power_on_reset'),
    ('\thost_version = readw_relaxed(', 'read_host_core_versions'),
    ('\tret = sdhci_msm_register_vreg(', 'register_regulators'),
    ('\tsdhci_msm_handle_pwr_irq(host, 0);', 'handle_pending_power_irq'),
    ('\tmsm_host->pwr_irq = platform_get_irq_byname(', 'get_power_irq'),
    ('\tmsm_host_writel(msm_host, INT_MASK, host,', 'enable_power_irq_mask'),
    ('\tret = devm_request_threaded_irq(', 'request_power_irq'),
    ('\tpm_runtime_get_noresume(&pdev->dev);', 'runtime_pm_setup'),
    ('\tif (of_property_read_bool(node, "supports-cqe"))', 'add_mmc_host'),
    ('\tpm_runtime_put_autosuspend(&pdev->dev);', 'host_added_autosuspend'),
]:
    probe = replace(probe, anchor, f'\ta6l_storage_checkpoint(&pdev->dev, "{label}");\n' + anchor)
msm.write_text(s[:start] + probe + s[end:])

s = init.read_text()
assert hashlib.sha256(init.read_bytes()).hexdigest() == '77676a614242a54f0767b104828ccbff0fd387736403757ef4762313139ab4f3'
s = replace(s, 'int storage_attempted = 0, storage_snapshot = 0;',
    'int storage_attempted = 0, storage_snapshot = 0, storage_armed = 0;\n    size_t storage_notice_end = 0;')
s = s.replace('A6L_STAGED_STORAGE_V19', 'A6L_STAGED_STORAGE_V20')
anchor = '        if (elapsed >= 20 && serial >= 0 && sent > 0 && !storage_attempted) {'
s = replace(s, anchor, '''        if (elapsed >= 18 && serial >= 0 && sent > 0 && !storage_armed) {
            storage_armed = 1;
            message("A6L_STORAGE_MODULE_FORK_ARMED earliest=20 child_pause_ms=1000\\n");
            storage_notice_end = journal_used;
        }
        if (elapsed >= 20 && serial >= 0 && storage_armed &&
            sent >= storage_notice_end && !storage_attempted) {''')
s = replace(s, 'message("A6L_STORAGE_MODULE_LOAD_BEGIN\\n");',
    'message("A6L_STORAGE_MODULE_LOAD_BEGIN pause_ms=1000\\n");\n                usleep(1000000);')
init.write_bytes(s.encode())
for name, p in [('dd-after.c', dd), ('sdhci-msm-after.c', msm), ('init-after.c', init), ('a6l_probe.h', header)]:
    (OUT / name).write_bytes(p.read_bytes())
(OUT / 'kernel-diff.patch').write_bytes(subprocess.check_output(['git', '-C', str(KERNEL), 'diff', '--', 'drivers/base/dd.c', 'drivers/mmc/host/sdhci-msm.c']))
print('V20 source prepared: 29 checkpoints, each 250ms; global cap64; ARM marker before fork; child pause1s')
