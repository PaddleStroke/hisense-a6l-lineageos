#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""A6L SELinux enforcing-readiness transform + init service domain check (selinux-release, 29 Sep 2026).

docs/selinux-release-20260929.md. Replaces the static rom/sepolicy/rc-enforcing.patch as the source of truth: the rc/ueventd
changes are computed from the CURRENT files, so they cannot go stale when other workers edit init.qcom.rc.

  apply <device_dir>        rewrite the files IN PLACE (pipeline: the Lineage TREE copy only, A6L_SELINUX_PREP=1) and
                            write rom/selinux/.prep-applied (BoardConfig-selinux.mk refuses A6L_SELINUX_PREP=1 without it)
  diff <device_dir>         print a unified diff (git-apply-able from the repo root) of what apply would change
  services <device_dir> --aosp <tree>/system/sepolicy [--prep] [--lineage <dir>...]
                            every service of every INSTALLED vendor rc: seclabel or file-label domain transition.
                            A service with no seclabel whose binary has no transition is refused by init EVEN IN PERMISSIVE
                            mode ("has incorrect label or no domain transition"); `su` does not exist in user builds.
Transform (idempotent; fails loudly if an anchor is missing = the tree drifted):
  T1 drop `seclabel u:r:vendor_modprobe:s0` (vendor services get a6l_modules / a6l_radio_ctl / a6l_chg_guard / a6l_macs
     from rom/sepolicy/vendor file labels)
  T2 remove the debug a6l_logcat service + trigger from init.qcom.rc (it runs /system/bin/logcat as su: a vendor domain may
     not exec system binaries and su is absent in user builds). userdebug/eng get rom/selinux/init.a6l.logcat-debug.rc.
  T3 ueventd: /dev/dri/* 0666 -> card* 0660 system graphics (composer, epdd lease, mirror, charger = uid system),
     renderD* 0666 (Mesa in app processes).
"""
import difflib, os, re, sys

INSTALLED_RC = [  # vendor rc files that reach the image (rom.mk / *.mk PRODUCT_COPY_FILES + soong init_rc), 29 Sep 2026
    'rom/init/init.qcom.rc', 'rom/init/init.a6l.usb.rc', 'power/rom/init.a6l-power.rc', 'rom/v2/init.a6l.wifibt.rc',
    'audio/bluetooth/btcall/init.a6l.btcall.rc', 'hals/sensors/stk3338/a6l-sensors-hal.rc', 'watchdog/init.a6l-watchdog.rc',
    'audio/route/a6l-audio-route.rc', 'camera/provider/provider/android.hardware.camera.provider-service.a6l.rc',
    'eink/a6l_eink.rc', 'eink/switcher/a6l_dualux.rc', 'gnss/android.hardware.gnss-service.a6l.rc',
    'gnss/timekeep/a6l-timekeep.rc', 'kvoice/q6voiced/a6l-q6voiced.rc', 'radio/init/android.hardware.radio-service.a6l.rc',
    'radio/init/a6l-imsdcm.rc', 'rom/r6/thermal/android.hardware.thermal-service.a6l.rc',
    'usb/gadget/android.hardware.usb.gadget-service.a6l.rc',
]
PREP_ONLY_RC = ['rom/selinux/init.a6l.logcat-debug.rc']   # installed by rom/selinux/selinux.mk (non-user variants)
# r6b boot fix (30 Sep 2026): installed in every userdebug/eng build (rom/debug/bootlog.mk), never in user; su is expected there
DEBUG_RC = ['rom/debug/init.a6l.bootlog-debug.rc']
UEVENTD = 'rom/vendor-etc/ueventd.rc'
MARKER = 'rom/selinux/.prep-applied'
# default policy dirs = rom/BoardConfig-rom.mk A6L_SEPOLICY_DIRS (29 Sep 2026); --prep adds the two enforcing-prep dirs
DEFAULT_POLICY = ['radio/sepolicy', 'audio/sepolicy', 'kvoice/q6voiced/sepolicy', 'gnss/sepolicy/vendor',
                  'eink/sepolicy/vendor', 'eink/switcher/sepolicy/vendor', 'usb/sepolicy/vendor', 'rom/r6/sepolicy/vendor',
                  'camera/sepolicy/vendor']
PREP_POLICY = ['rom/sepolicy/vendor', 'rom/selinux/sepolicy/vendor']
MODPROBE = re.compile(r'^\s*seclabel\s+u:r:vendor_modprobe:s0\s*$')
DRI_OLD = re.compile(r'^/dev/dri/\*\s+0666\s+system\s+graphics\s*$')
DRI_NEW = ['# selinux-release (A6L_SELINUX_PREP): KMS nodes only for uid system / group graphics (composer, epdd lease, e-ink mirror,',
           '# dualux, charger UI); render nodes stay world-usable for Mesa in app processes.',
           '/dev/dri/card*            0660   system     graphics',
           '/dev/dri/renderD*         0666   system     graphics']


def rd(p):
    with open(p, encoding='utf-8', newline='') as f:
        return f.read().replace('\r\n', '\n')


def sections(text):
    """split an init rc into [(header, [lines])]; header None = leading comments"""
    out, cur = [], [None, []]
    for ln in text.split('\n'):
        if re.match(r'^(on|service|import)\s', ln):
            out.append(cur); cur = [ln, []]
        else:
            cur[1].append(ln)
    out.append(cur)
    return out


def transform(dev):
    """returns {relpath: (old, new)} for changed files; raises on drift"""
    ch = {}
    for rel in INSTALLED_RC:
        p = os.path.join(dev, rel)
        if not os.path.exists(p):
            continue
        old = rd(p); lines = old.split('\n')
        new = '\n'.join(l for l in lines if not MODPROBE.match(l))
        if rel == 'rom/init/init.qcom.rc':
            secs = sections(new); keep = []; drop = 0
            for h, body in secs:
                if h and (re.match(r'^service a6l_logcat\s', h) or re.match(r'^on .*ro\.boot\.a6l_logcat=1', h)):
                    drop += 1
                    # keep the comment lines that introduce the NEXT section (they sit at the end of this body)
                    last = max([i for i, l in enumerate(body) if l.strip() and not l.lstrip().startswith('#')] + [-1])
                    tail = [l for l in body[last + 1:] if l.strip()]
                    if tail:
                        keep.append((None, tail))
                    continue
                keep.append((h, body))
            if drop not in (0, 2):
                raise SystemExit('DRIFT: init.qcom.rc a6l_logcat: expected trigger + service, found %d' % drop)
            if drop == 2:
                note = ['# selinux-release (A6L_SELINUX_PREP): the debug a6l_logcat service (androidboot.a6l_logcat=1) moved to',
                        '# /vendor/etc/init/init.a6l.logcat-debug.rc, installed in userdebug/eng only (it runs as su).', '']
                keep.insert(1, (None, note))
            # strip the comment block that introduced the logcat trigger (it would dangle above the next section)
            out = []
            for h, body in keep:
                if h is not None:
                    out.append(h)
                out.extend(body)
            new = '\n'.join(out)
            new = re.sub(r'# Debug \(QEMU test / attended first boot\): androidboot\.a6l_logcat=1[^\n]*\n#[^\n]*\n(?=\n?)', '', new)
            if 'a6l_logcat' in re.sub(r'#[^\n]*', '', new):
                raise SystemExit('DRIFT: a6l_logcat still referenced in init.qcom.rc after T2')
        if new != old:
            ch[rel] = (old, new)
    p = os.path.join(dev, UEVENTD)
    old = rd(p); lines = old.split('\n'); out = []; hit = 0
    for l in lines:
        if DRI_OLD.match(l):
            out.extend(DRI_NEW); hit += 1
        else:
            out.append(l)
    new = '\n'.join(out)
    if hit == 0 and '/dev/dri/card*' not in old:
        raise SystemExit('DRIFT: ueventd.rc has neither the /dev/dri/* 0666 rule nor the prep rule')
    if new != old:
        ch[UEVENTD] = (old, new)
    return ch


def cmd_apply(dev):
    ch = transform(dev)
    for rel, (_, new) in ch.items():
        with open(os.path.join(dev, rel), 'w', encoding='utf-8', newline='\n') as f:
            f.write(new)
        print('A6L_SELINUX_PREP changed %s' % rel)
    with open(os.path.join(dev, MARKER), 'w') as f:
        f.write('applied by tools/release/a6l_selinux_prep.py\n')
    left = [r for r in INSTALLED_RC if os.path.exists(os.path.join(dev, r)) and
            any(MODPROBE.match(l) for l in rd(os.path.join(dev, r)).split('\n'))]
    if left:
        raise SystemExit('A6L_SELINUX_PREP FAIL vendor_modprobe left in %s' % left)
    print('A6L_SELINUX_PREP_APPLIED %d file(s)' % len(ch))


def cmd_diff(dev):
    for rel, (old, new) in sorted(transform(dev).items()):
        a = 'a/device/hisense/a6l/' + rel; b = 'b/device/hisense/a6l/' + rel
        sys.stdout.writelines(difflib.unified_diff(old.splitlines(True), new.splitlines(True), a, b))
        if not new.endswith('\n'):
            pass


# ---------------------------------------------------------------- service/domain check
def fc_entries(paths):
    ents = []
    for p in paths:
        if not os.path.exists(p):
            continue
        for ln in rd(p).split('\n'):
            ln = ln.split('#', 1)[0].strip()
            if not ln:
                continue
            f = ln.split()
            ctx = f[-1]; rx = f[0]
            m = re.match(r'u:object_r:([A-Za-z0-9_]+):s0', ctx)
            if m:
                ents.append((rx, m.group(1), p))
    return ents


def stem_len(rx):
    m = re.search(r'[.^$?*+|\[({\\]', rx)
    return len(rx) if m is None else m.start()


def label_of(path, ents):
    best = None
    for i, (rx, t, src) in enumerate(ents):
        try:
            if not re.fullmatch(rx, path):
                continue
        except re.error:
            continue
        meta = re.search(r'[.^$?*+|\[({]', rx.replace('\\.', '')) is not None
        key = (0 if meta else 1, stem_len(rx), i)   # exact spec > longer stem > later entry
        if best is None or key > best[0]:
            best = (key, t, src)
    return best


def te_text(dirs):
    txt = []
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for root, _, files in os.walk(d):
            if '/compat' in root or '/prebuilts' in root:
                continue
            for fn in files:
                if fn.endswith('.te'):
                    txt.append(rd(os.path.join(root, fn)))
    return '\n'.join(txt)


def transitions(te):
    tr = {}
    for m in re.finditer(r'init_daemon_domain\(\s*(\w+)\s*\)', te):
        tr[m.group(1) + '_exec'] = m.group(1)
    for m in re.finditer(r'domain_auto_trans\(\s*init\s*,\s*(\w+)\s*,\s*(\w+)\s*\)', te):
        tr[m.group(1)] = m.group(2)
    for m in re.finditer(r'type_transition\s+init\s+(\w+)\s*:\s*process\s+(\w+)', te):
        tr[m.group(1)] = m.group(2)
    return tr


def services(dev, rels, over=None):
    out = []
    for rel in rels:
        p = os.path.join(dev, rel)
        if not os.path.exists(p):
            continue
        text = over[rel][1] if over and rel in over else rd(p)
        for h, body in sections(text):
            if not h or not h.startswith('service '):
                continue
            f = h.split(); name, exe = f[1], f[2]
            sl = [l.split()[1] for l in body if re.match(r'^\s*seclabel\s', l)]
            out.append((rel, name, exe, sl[0] if sl else None))
    return out


def cmd_services(dev, aosp, prep, extra):
    pol = [os.path.join(dev, d) for d in DEFAULT_POLICY + (PREP_POLICY if prep else [])]
    fcs = [os.path.join(aosp, 'private/file_contexts'), os.path.join(aosp, 'vendor/file_contexts')]
    fcs += [os.path.join(d, 'file_contexts') for d in extra + pol]
    ents = fc_entries(fcs)
    te = te_text([os.path.join(aosp, 'public'), os.path.join(aosp, 'private'), os.path.join(aosp, 'vendor')] + extra + pol)
    tr = transitions(te)
    types = set(re.findall(r'^\s*type\s+(\w+)\s*[,;]', te, re.M))
    rels = INSTALLED_RC + DEBUG_RC + (PREP_ONLY_RC if prep else [])
    fails = warns = 0
    over = transform(dev) if prep else None   # --prep checks the rc files AS apply would leave them
    for rel, name, exe, sl in services(dev, rels, over):
        if sl:
            dom = re.match(r'u:r:(\w+):s0', sl).group(1)
            if dom == 'su':
                st = 'DEBUG' if rel in PREP_ONLY_RC + DEBUG_RC else 'WARN'
                msg = 'seclabel su (exists only in userdebug/eng)'
                warns += st == 'WARN'
            elif dom == 'vendor_modprobe':
                st = 'WARN'; msg = 'seclabel vendor_modprobe (AOSP modprobe domain: no exec of scripts/daemons in enforcing)'; warns += 1
            elif dom in types:
                st = 'OK'; msg = 'seclabel ' + dom
            else:
                st = 'FAIL'; msg = 'seclabel %s: type not declared' % dom; fails += 1
        else:
            lb = label_of(exe, ents)
            if lb is None:
                st, msg = 'FAIL', 'no file label'; fails += 1
            elif lb[1] in tr:
                st, msg = 'OK', '%s -> %s' % (lb[1], tr[lb[1]])
            else:
                st, msg = 'FAIL', '%s (%s): no init domain transition -> init refuses to start it, even permissive' % (
                    lb[1], os.path.relpath(lb[2], dev) if lb[2].startswith(dev) else lb[2]); fails += 1
        print('%-5s %-32s %-58s %s' % (st, name, exe[:58], msg))
    print('A6L_SELINUX_SERVICES %s policy=%s fail=%d warn=%d' % ('PASS' if fails == 0 else 'FAIL', 'prep' if prep else 'default', fails, warns))
    return fails


def main(a):
    if len(a) >= 2 and a[0] == 'apply':
        cmd_apply(a[1])
    elif len(a) >= 2 and a[0] == 'diff':
        cmd_diff(a[1])
    elif len(a) >= 2 and a[0] == 'services':
        aosp = a[a.index('--aosp') + 1]
        extra = [a[i + 1] for i, x in enumerate(a) if x == '--lineage']
        sys.exit(1 if cmd_services(a[1], aosp, '--prep' in a, extra) else 0)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
