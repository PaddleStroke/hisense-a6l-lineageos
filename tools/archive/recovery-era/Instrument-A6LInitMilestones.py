#!/usr/bin/env python3
"""Log the A6L transition from kernel initcalls to RAM userspace."""
from pathlib import Path
import difflib
import hashlib
import json
import subprocess

ROOT = Path(__file__).resolve().parents[1]
KERNEL = Path('/home/a6l/kernel/a6l-mainline')
OUT = ROOT / 'firmware/extracted/init-milestones-source-20260915'
REL = 'init/main.c'


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

    replace('#include <linux/init.h>', '#include <linux/init.h>\n#include <linux/of.h>')
    macro = '''
/* A6L diagnostic: preserve operations and expose the late-init boundary. */
#define a6l_init_trace(fmt, ...) do { \\
\tif (initcall_debug && of_machine_is_compatible("hisense,hlte730t")) \\
\t\tpr_info("A6L init " fmt "\\n", ##__VA_ARGS__); \\
} while (0)

'''
    replace('static void __init do_basic_setup(void)', macro + 'static void __init do_basic_setup(void)')
    for operation, label in [
        ('do_initcalls();', 'initcalls'),
        ('kernel_init_freeable();', 'kernel_init_freeable'),
        ('async_synchronize_full();', 'async_synchronize_full'),
        ('free_initmem();', 'free_initmem'),
        ('mark_readonly();', 'mark_readonly'),
        ('do_sysctl_args();', 'sysctl_args'),
        ('kunit_run_all_tests();', 'kunit'),
        ('wait_for_initramfs();', 'wait_for_initramfs'),
        ('console_on_rootfs();', 'console_on_rootfs'),
        ('integrity_load_keys();', 'integrity_load_keys'),
    ]:
        old = '\t' + operation
        replace(old, '\ta6l_init_trace("' + label + ' begin");\n' + old +
                '\n\ta6l_init_trace("' + label + ' done");')
    old = '\tramdisk_command_access = init_eaccess(ramdisk_execute_command);'
    replace(old, '\ta6l_init_trace("rdinit access begin");\n' + old +
            '\n\ta6l_init_trace("rdinit access result=%d", ramdisk_command_access);')
    old = '\treturn kernel_execve(init_filename, argv_init, envp_init);'
    replace(old, '\ta6l_init_trace("exec %s", init_filename);\n' + old)
    recovered = source
    for old, new in reversed(edits):
        assert recovered.count(new) == 1
        recovered = recovered.replace(new, old, 1)
    assert recovered == original
    target = KERNEL / REL
    assert target.read_text() == original, 'Unexpected existing init/main.c changes'
    OUT.mkdir(exist_ok=False)
    (OUT / 'main.before.c').write_text(original)
    (OUT / 'main.after.c').write_text(source)
    (OUT / 'logging.patch').write_text(''.join(difflib.unified_diff(
        original.splitlines(True), source.splitlines(True), fromfile='a/' + REL, tofile='b/' + REL)))
    target.write_text(source)
    (OUT / 'report.json').write_text(json.dumps({
        'scope': 'Exact-board and initcall_debug gated printk only; no operation removed, reordered, or delayed intentionally',
        'before_sha256': hashlib.sha256(original.encode()).hexdigest(),
        'after_sha256': hashlib.sha256(source.encode()).hexdigest(),
        'reverses_exactly_to_pinned_source': True,
        'physical_test_pending': True,
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
