#!/usr/bin/env python3
"""Preserve boot domains at genpd sync only on A6L with pd_ignore_unused."""
from pathlib import Path
import difflib
import hashlib
import json
import subprocess

ROOT = Path(__file__).resolve().parents[1]
KERNEL = Path('/home/a6l/kernel/a6l-mainline')
OUT = ROOT / 'firmware/extracted/keep-boot-domains-source-20260915'
REL = 'drivers/pmdomain/core.c'


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

    replace('#include <linux/platform_device.h>', '#include <linux/platform_device.h>\n#include <linux/of.h>')
    helper = '''/* Local bring-up only: retain firmware power state until consumers exist. */
static bool a6l_keep_boot_domains(void)
{
\treturn pd_ignore_unused && of_machine_is_compatible("hisense,hlte730t");
}

'''
    replace('void of_genpd_sync_state(struct device_node *np)', helper + 'void of_genpd_sync_state(struct device_node *np)')
    start = source.index('void of_genpd_sync_state(')
    end = source.index('\nEXPORT_SYMBOL_GPL(of_genpd_sync_state);', start)
    old = source[start:end]
    needle = '\tif (!np)\n\t\treturn;'
    assert old.count(needle) == 1
    new = old.replace(needle, needle + '''

\tif (a6l_keep_boot_domains()) {
\t\tpr_info("A6L genpd preserving boot domains for %pOF\\n", np);
\t\treturn;
\t}
''', 1)
    replace(old, new)
    start = source.index('static void genpd_provider_sync_state(')
    end = source.index('\nstatic struct device_driver genpd_provider_drv', start)
    old = source[start:end]
    needle = '\tswitch (genpd->sync_state) {'
    assert old.count(needle) == 1
    new = old.replace(needle, '''\tif (a6l_keep_boot_domains()) {
\t\tpr_info("A6L genpd preserving boot provider %s\\n", dev_name(dev));
\t\treturn;
\t}

''' + needle, 1)
    replace(old, new)
    recovered = source
    for old, new in reversed(edits):
        assert recovered.count(new) == 1
        recovered = recovered.replace(new, old, 1)
    assert recovered == original
    target = KERNEL / REL
    assert target.read_text() == original, 'Unexpected existing power-domain changes'
    OUT.mkdir(exist_ok=False)
    (OUT / 'core.before.c').write_text(original)
    (OUT / 'core.after.c').write_text(source)
    (OUT / 'preserve-boot-domains.patch').write_text(''.join(difflib.unified_diff(
        original.splitlines(True), source.splitlines(True), fromfile='a/' + REL, tofile='b/' + REL)))
    target.write_text(source)
    (OUT / 'report.json').write_text(json.dumps({
        'scope': 'Skip genpd sync-state poweroff only on hisense,hlte730t with pd_ignore_unused; preserve stay_on and existing firmware domain state',
        'before_sha256': hashlib.sha256(original.encode()).hexdigest(),
        'after_sha256': hashlib.sha256(source.encode()).hexdigest(),
        'reverses_exactly_to_pinned_source': True,
        'runtime_power_management_paths_unchanged': True,
        'physical_fix_unproven': True,
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
