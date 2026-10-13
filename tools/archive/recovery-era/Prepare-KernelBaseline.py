"""Prepare an isolated older/newer kernel with the same minimal A6L harness.

No phone access. Keep essential boot-domain/USB support and the LCD console;
exclude the experimental CMD0 holds, IRQ masks, register snapshots and barriers.
"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
ORIGINAL = Path('/home/a6l/kernel/a6l-mainline')
REVISIONS = {'6.19': 'a587e4f18b483d0a17579e6325c861b303988bda',
             '7.2': 'e47d622cb6d2440a9eacdc8bb2df32c037bec7b8'}


def run(*args, **kwargs):
    proc = subprocess.run([str(a) for a in args], capture_output=True, **kwargs)
    if proc.returncode:
        raise RuntimeError(f'{args}: {proc.stderr.decode(errors="replace")}')
    return proc.stdout


def manual_usb(source):
    """Same hold/release semantics, independent of newer connected-state check."""
    def replace(old, new):
        nonlocal source
        assert source.count(old) == 1, old
        source = source.replace(old, new, 1)
    replace('#include <linux/workqueue.h>', '#include <linux/workqueue.h>\n#include <linux/init.h>\n#include <linux/kstrtox.h>\n#include <linux/of.h>')
    replace('static DEFINE_IDA(gadget_id_numbers);', '''static bool a6l_manual_usb;
static int __init a6l_manual_usb_setup(char *value)
{
\treturn kstrtobool(value, &a6l_manual_usb);
}
early_param("a6l_manual_usb", a6l_manual_usb_setup);

static DEFINE_IDA(gadget_id_numbers);''')
    replace('bool\t\t\t\tallow_connect;', 'bool\t\t\t\tallow_connect;\n\tbool\t\t\t\ta6l_hold_connect;')
    anchor = '\tif (gadget->connected)\n\t\tgoto out;'
    if anchor not in source:
        anchor = '\tif (gadget->deactivated || !gadget->udc->allow_connect || !gadget->udc->started) {'
    replace(anchor, '''\tif (gadget->udc->a6l_hold_connect) {
\t\tdev_info(&gadget->udc->dev, "A6L_USB_CONNECT_HELD: driver bound, pullup not called\\n");
\t\tgoto out;
\t}

''' + anchor)
    anchor = '\tret = driver->bind(udc->gadget, driver);'
    replace(anchor, '''\tudc->a6l_hold_connect = a6l_manual_usb &&
\t\tof_machine_is_compatible("hisense,hlte730t");
\tif (udc->a6l_hold_connect)
\t\tdev_info(&udc->dev, "A6L_USB_MANUAL_CONNECT armed\\n");

''' + anchor)
    replace('''\t\tusb_gadget_udc_start_locked(udc);
\t\tusb_gadget_connect_locked(udc->gadget);
\t\tmutex_unlock(&udc->connect_lock);''', '''\t\tif (udc->a6l_hold_connect) {
\t\t\tdev_info(&udc->dev, "A6L_USB_CONNECT_RELEASE_BEGIN\\n");
\t\t\tudc->a6l_hold_connect = false;
\t\t\tret = usb_gadget_connect_locked(udc->gadget);
\t\t\tdev_info(&udc->dev, "A6L_USB_CONNECT_RELEASE_END result=%zd\\n", ret);
\t\t\tmutex_unlock(&udc->connect_lock);
\t\t\tif (ret)
\t\t\t\tgoto out;
\t\t} else {
\t\t\tusb_gadget_udc_start_locked(udc);
\t\t\tusb_gadget_connect_locked(udc->gadget);
\t\t\tmutex_unlock(&udc->connect_lock);
\t\t}''')
    return source


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('version', choices=REVISIONS)
    args = ap.parse_args()
    kernel = Path('/home/a6l/kernel/a6l-baseline-' + args.version)
    out = ROOT / ('firmware/extracted/baseline-' + args.version + '-source-20260917')
    assert run('git', '-C', kernel, 'rev-parse', 'HEAD').decode().strip() == REVISIONS[args.version]
    assert not run('git', '-C', kernel, 'status', '--porcelain').strip()
    assert run('git', '-C', ORIGINAL, 'rev-parse', 'HEAD').decode().strip() == REVISIONS['7.2']
    # Snapshot the existing V32/V33 fixes; apply only these reviewed paths.
    retained = ['drivers/pmdomain/core.c', 'drivers/tty/serial/Makefile']
    patch = run('git', '-C', ORIGINAL, 'diff', '--', *retained)
    run('git', '-C', kernel, 'apply', '--check', '-', input=patch)
    udc = kernel / 'drivers/usb/gadget/udc/core.c'
    usb_before = udc.read_text()
    usb_after = manual_usb(usb_before)
    out.mkdir(exist_ok=False)
    (out / 'retained-boot-usb.patch').write_bytes(patch)
    run('git', '-C', kernel, 'apply', '-', input=patch)
    udc.write_text(usb_after)
    (out / 'manual-usb.patch').write_text(''.join(difflib.unified_diff(
        usb_before.splitlines(True), usb_after.splitlines(True),
        fromfile='a/drivers/usb/gadget/udc/core.c', tofile='b/drivers/usb/gadget/udc/core.c')))
    earlycon = (ROOT / 'device/hisense/a6l/kernel/a6l_earlycon.c').read_bytes()
    # Older font_desc exposes const void *; byte indexing needs an explicit cast.
    assert earlycon.count(b'font_vga_8x16.data[c * 16 + h / 2]') == 1
    earlycon = earlycon.replace(b'font_vga_8x16.data[c * 16 + h / 2]',
                               b'((const u8 *)font_vga_8x16.data)[c * 16 + h / 2]')
    (kernel / 'drivers/tty/serial/a6l_earlycon.c').write_bytes(earlycon)
    (out / 'a6l_earlycon.c').write_bytes(earlycon)
    # Preserve the existing A6L-only optional activity LED bypass; no other
    # storage-driver changes. The CMD0 submission path is the branch original.
    rel = 'drivers/mmc/host/sdhci-msm.c'
    before = (kernel / rel).read_text()
    anchor = '\thost->sdma_boundary = 0;'
    assert before.count(anchor) == 1
    after = before.replace(anchor, '''\t/* Common A6L comparison harness: retain the V22 LED isolation. */
\tif (of_machine_is_compatible("hisense,hlte730t") &&
\t    !strcmp(dev_name(&pdev->dev), "c0c4000.mmc")) {
\t\thost->quirks |= SDHCI_QUIRK_NO_LED;
\t\tdev_info(&pdev->dev, "A6L_BASELINE optional activity LED disabled\\n");
\t}

''' + anchor)
    (kernel / rel).write_text(after)
    for name in ['sdm660-hisense-a6l-probe.dts', 'sdm660-hisense-a6l-recovery.dts',
                 'sdm660-hisense-a6l-usb-only.dts', 'sdm660-hisense-a6l-usb-load.dts',
                 'sdm660-hisense-a6l-storage.dts']:
        value = (ROOT / 'device/hisense/a6l/kernel' / name).read_bytes()
        if args.version == '6.19' and name == 'sdm660-hisense-a6l-probe.dts':
            assert value.count(b'&remoteproc_cdsp') == 1
            value = value.replace(b'&remoteproc_cdsp', b'&cdsp_pil')
        (kernel / 'arch/arm64/boot/dts/qcom' / name).write_bytes(value)
        (out / name).write_bytes(value)
    # Match the last physical A6L setup, compiled against each branch's bindings.
    wrapper = '''// Same board-specific settings for both comparison kernels.
#include "sdm660-hisense-a6l-storage.dts"
&sdhc_1 {
    no-sdio;
    /delete-property/ resets;
};
'''
    name = 'sdm660-hisense-a6l-baseline.dts'
    (kernel / 'arch/arm64/boot/dts/qcom' / name).write_text(wrapper)
    (out / name).write_text(wrapper)
    (out / 'complete-tracked.patch').write_bytes(run('git', '-C', kernel, 'diff'))
    report = {'source_revision': REVISIONS[args.version], 'kernel_dir': str(kernel),
              'retained_patch_sha256': hashlib.sha256(patch).hexdigest(),
              'earlycon_sha256': hashlib.sha256(earlycon).hexdigest(),
              'mmc_core_unchanged': not bool(run('git', '-C', kernel, 'diff', '--', 'drivers/mmc/core', 'drivers/mmc/host/sdhci.c')),
              'storage_change': 'only board-gated SDHCI_QUIRK_NO_LED; no command/IRQ/voltage experiments',
              'device_tree': 'A6L wrapper compiled with branch-specific SoC definitions; audit required',
              'ready_to_flash': False}
    assert report['mmc_core_unchanged']
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
