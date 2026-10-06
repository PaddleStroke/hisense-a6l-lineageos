#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""ATTENDED (phone booted into the installed ROM, userdebug, adb authorised): collect the SELinux evidence of a permissive
install and build the policy plan (selinux-release, 29 Sep 2026; docs/selinux-release-20260929.md section 3).

READ-ONLY on the phone: adb shell reads only (dmesg, logcat -d, ps -AZ, ls -lZ, getprop). `--root` runs `adb root` first
(restarts adbd; userdebug only) so dmesg and the /sys labels are readable. Nothing is written to the phone.

usage: python3 tools/release/Collect-AvcV1.py <tag> [--root] [--serial S] [--out captures/avc-<tag>]
  -> <out>/{avc-dmesg.txt, avc-logcat.txt, ps-AZ.txt, labels.txt, getenforce.txt, getprop.txt, plan.md, proposed/}
Do it after exercising the features (calls, Wi-Fi, BT, camera, e-ink, USB, charging, reboot, off-mode charging) so the
denials of every path are in the log; logcat keeps less than dmesg, so collect once per scenario if needed.
"""
import os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__)); REPO = os.path.abspath(os.path.join(HERE, '..', '..'))


def adb(args, serial, check=False):
    cmd = ['adb'] + (['-s', serial] if serial else []) + args
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
    if check and r.returncode:
        sys.exit('adb failed: %s\n%s' % (' '.join(cmd), r.stdout.decode(errors='replace')))
    return r.stdout.decode(errors='replace').replace('\r\n', '\n')


def unverified_paths():
    """sysfs paths of the policy's genfs_contexts (the UNVERIFIED ones must be confirmed with ls -lZd)"""
    out = []
    for d in ('device/hisense/a6l/rom/sepolicy/vendor', 'device/hisense/a6l/rom/r6/sepolicy/vendor',
              'device/hisense/a6l/eink/sepolicy/vendor', 'device/hisense/a6l/camera/sepolicy/vendor'):
        p = os.path.join(REPO, d, 'genfs_contexts')
        if os.path.exists(p):
            for ln in open(p, encoding='utf-8', errors='replace'):
                m = re.match(r'^\s*genfscon\s+sysfs\s+(\S+)\s+(\S+)', ln)
                if m:
                    out.append(('/sys' + m.group(1), m.group(2)))
    return out


def main(a):
    if not a or a[0].startswith('-'):
        sys.exit(__doc__)
    tag = a[0]; serial = a[a.index('--serial') + 1] if '--serial' in a else None
    out = a[a.index('--out') + 1] if '--out' in a else os.path.join(REPO, 'captures', 'avc-%s-%s' % (tag, time.strftime('%Y%m%d-%H%M%S')))
    os.makedirs(out, exist_ok=True)
    st = adb(['get-state'], serial).strip()
    if st != 'device':
        sys.exit('no adb device (state %r): boot the installed ROM, authorise adb' % st)
    if adb(['shell', 'getprop', 'ro.debuggable'], serial).strip() != '1':
        print('WARNING: not a debuggable build: dmesg and /sys labels may be unreadable')
    if '--root' in a:
        print(adb(['root'], serial).strip()); time.sleep(3); adb(['wait-for-device'], serial)
    w = lambda n, s: open(os.path.join(out, n), 'w').write(s)
    w('getenforce.txt', adb(['shell', 'getenforce'], serial))
    w('avc-dmesg.txt', adb(['shell', 'dmesg 2>&1 | grep -E "avc:|selinux|SELinux"'], serial))
    w('avc-logcat.txt', adb(['shell', 'logcat -b all -d 2>&1 | grep -E "avc:|type=1400"'], serial))
    w('ps-AZ.txt', adb(['shell', 'ps -AZ'], serial))
    w('getprop.txt', adb(['shell', 'getprop'], serial))
    lab = ['# expected-type path  ->  ls -lZd']
    for path, ctx in unverified_paths():
        lab.append('%s %s -> %s' % (ctx, path, adb(['shell', 'ls -lZd %s 2>&1' % path], serial).strip()))
    for cmd in ('ls -lZ /dev/block/by-name/', 'ls -lZ /dev/dri /dev/a6l /dev/rfkill /dev/qcom_rmtfs_mem* /dev/iio:device* 2>&1',
                'ls -lZ /vendor/bin /vendor/bin/hw /vendor/a6l/radio/bin 2>&1',
                'for f in /sys/class/power_supply/* /sys/class/leds/* /sys/class/backlight/* /sys/bus/iio/devices/* '
                '/sys/class/remoteproc/* /sys/class/drm/*; do echo "$f -> $(readlink -f $f)"; ls -lZd $(readlink -f $f); done 2>&1',
                'ls -lZd /mnt/vendor/persist /data/vendor/* 2>&1'):
        lab += ['', '$ ' + cmd, adb(['shell', cmd], serial)]
    w('labels.txt', '\n'.join(lab) + '\n')
    r = subprocess.run([sys.executable, os.path.join(HERE, 'a6l-avc-plan.py'), '--repo', REPO, '--ps', os.path.join(out, 'ps-AZ.txt'),
                        '--out', out, os.path.join(out, 'avc-dmesg.txt'), os.path.join(out, 'avc-logcat.txt')],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(r.stdout.decode(errors='replace').splitlines()[-1] if r.stdout else '')
    print('A6L_AVC_COLLECT_DONE %s' % out)


if __name__ == '__main__':
    main(sys.argv[1:])
