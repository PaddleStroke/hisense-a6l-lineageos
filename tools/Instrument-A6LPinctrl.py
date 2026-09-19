#!/usr/bin/env python3
"""Add board-gated log lines; preserve all pin-controller operations verbatim."""
from pathlib import Path
import hashlib
import json
import subprocess

ROOT = Path(__file__).resolve().parents[1]
KERNEL = Path('/home/a6l/kernel/a6l-mainline')
REL = 'drivers/pinctrl/qcom/pinctrl-msm.c'
OUT = ROOT / 'firmware/extracted/pinctrl-trace-source-20260915'


def main():
    assert subprocess.check_output(['git', '-C', str(KERNEL), 'rev-parse', 'HEAD']).decode().strip() == 'e47d622cb6d2440a9eacdc8bb2df32c037bec7b8'
    original = subprocess.check_output(['git', '-C', str(KERNEL), 'show', 'HEAD:' + REL]).decode()
    source = original
    edits = []

    def replace(old, new):
        nonlocal source
        assert source.count(old) == 1, old
        source = source.replace(old, new, 1)
        edits.append((old, new))

    replace('#include <linux/interrupt.h>', '#include <linux/init.h>\n#include <linux/interrupt.h>')
    macro = '''
/* Local diagnostic: logging only on the exact A6L board. */
#define a6l_pinctrl_trace(dev, fmt, ...) do { \\
\tif (initcall_debug && of_machine_is_compatible("hisense,hlte730t")) \\
\t\tdev_info(dev, "A6L pinctrl " fmt "\\n", ##__VA_ARGS__); \\
} while (0)
'''
    replace('#define PS_HOLD_OFFSET 0x820', '#define PS_HOLD_OFFSET 0x820\n' + macro)
    start = source.index('static int msm_gpio_get_direction(')
    stop = source.index('\nstatic int msm_gpio_get(', start)
    old = source[start:stop]
    new = old.replace('\tval = msm_readl_ctl(pctrl, g);',
        '\ta6l_pinctrl_trace(pctrl->dev, "gpio %u ctl begin", offset);\n'
        '\tval = msm_readl_ctl(pctrl, g);\n'
        '\ta6l_pinctrl_trace(pctrl->dev, "gpio %u ctl done %08x", offset, val);')
    assert old != new
    replace(old, new)
    start = source.index('\nint msm_pinctrl_probe(')
    stop = source.index('\nEXPORT_SYMBOL(msm_pinctrl_probe);', start)
    old = source[start:stop]
    new = old

    def trace_before(needle, message):
        nonlocal new
        assert new.count(needle) == 1, needle
        new = new.replace(needle, '\ta6l_pinctrl_trace(&pdev->dev, "' + message + '");\n' + needle, 1)

    trace_before('\tpctrl = devm_kzalloc', 'probe entry')
    trace_before('\tif (soc_data->tiles)', 'map registers')
    trace_before('\tmsm_pinctrl_setup_pm_reset(pctrl);', 'maps done; setup reset handlers')
    trace_before('\tpctrl->irq = platform_get_irq(pdev, 0);', 'reset handlers done; get IRQ')
    trace_before('\tpctrl->desc.owner = THIS_MODULE;', 'IRQ done; register pinctrl')
    trace_before('\tfor (i = 0; i < soc_data->nfunctions; i++)', 'pinctrl registered; add functions')
    trace_before('\tret = pinctrl_enable(pctrl->pctrl);', 'functions added; enable pinctrl')
    trace_before('\tret = msm_gpio_init(pctrl);', 'pinctrl enabled; register GPIOs')
    trace_before('\tplatform_set_drvdata(pdev, pctrl);', 'GPIOs registered; probe done')
    replace(old, new)
    # Reverse every insertion and prove the stock source is recovered exactly.
    recovered = source
    for old, new in reversed(edits):
        assert recovered.count(new) == 1
        recovered = recovered.replace(new, old, 1)
    assert recovered == original
    target = KERNEL / REL
    assert target.read_text() in (original, source), 'Unexpected existing pinctrl edits'
    OUT.mkdir(exist_ok=False)
    (OUT / 'pinctrl-msm.before.c').write_text(original)
    (OUT / 'pinctrl-msm.after.c').write_text(source)
    target.write_text(source)
    diff = subprocess.check_output(['git', '-C', str(KERNEL), 'diff', '--', REL])
    (OUT / 'logging.patch').write_bytes(diff)
    (OUT / 'report.json').write_text(json.dumps({
        'scope': 'Board-gated printk instrumentation only; no new MMIO reads or writes',
        'before_sha256': hashlib.sha256(original.encode()).hexdigest(),
        'after_sha256': hashlib.sha256(source.encode()).hexdigest(),
        'reverses_exactly_to_pinned_source': True,
        'physical_test_pending': True,
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
