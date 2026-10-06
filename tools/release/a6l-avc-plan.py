#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""A6L audit-driven SELinux policy plan (selinux-release, 29 Sep 2026; docs/selinux-release-20260929.md).

OFFLINE: reads the avc log collected from the installed (permissive) ROM by tools/release/Collect-AvcV1.py (or any dmesg /
logcat / bugreport text) and PROPOSES policy changes. It never edits a policy dir: the plan is reviewed, rules are copied by
hand, then `bash tools/release/check-a6l-sepolicy.sh user rom/sepolicy/vendor rom/selinux/sepolicy/vendor` must PASS.

usage: a6l-avc-plan.py [--repo <A6L repo>] [--ps ps-AZ.txt] [--out <dir>] <log> [<log>...]
  -> markdown plan on stdout (+ <out>/plan.md and <out>/proposed/<policy dir>/<domain>.te when --out is given)
Each (scontext, tcontext, tclass) group is classified:
  LABEL      the object has a generic type (vendor_file, sysfs, device, unlabeled, default props...): label it
             (file_contexts / genfs_contexts / property_contexts) instead of allowing the generic type
  NEVERALLOW the rule would break a Treble/CTS neverallow (vendor exec of system binaries, dac_override, core props...):
             change the code or config, never paste the rule
  PLATFORM   denial of a platform/app domain against platform types: not ours to fix in vendor policy (report upstream / ignore)
  DEBUG      su / shell / adbd / dumpstate noise of a userdebug session
  ALLOW      plain missing permission: proposed allow rule in the dir that declares the domain
permissive=0 lines (the domain was already enforcing) are marked BLOCKING.
"""
import collections, os, re, sys

AVC = re.compile(r'avc:\s+denied\s+\{\s*([^}]*)\}\s+for\s+(.*?)scontext=u:r:([\w]+):s0(?::[\w,.]+)?\s+'
                 r'tcontext=u:(?:object_r|r):([\w]+):s0(?::[\w,.]+)?\s+tclass=([\w]+)(?:\s+permissive=([01]))?')
KV = re.compile(r'\b(comm|name|path|dev|ino|pid|capability|scontext)="?([^" ]+)"?')
GENERIC_OBJ = {'vendor_file': 'file_contexts (/vendor/...)', 'unlabeled': 'file_contexts / genfs (no label at all)',
               'sysfs': 'genfs_contexts (sysfs path)', 'device': 'file_contexts (/dev node)', 'proc': 'genfs_contexts (proc)',
               'default_prop': 'property_contexts', 'vendor_default_prop': 'property_contexts',
               'block_device': 'file_contexts (/dev/block/by-name)', 'tmpfs': 'file_contexts or type_transition',
               'vendor_data_file': 'file_contexts (/data/vendor/<dir>)', 'debugfs': 'genfs_contexts (debugfs; release: avoid)',
               'sysfs_devices_system_cpu': 'genfs (if a6l-specific)', 'vendor_configs_file': 'file_contexts (/vendor/etc/...)'}
DEBUG_DOMAINS = {'su', 'shell', 'adbd', 'dumpstate', 'magisk', 'runas', 'simpleperf', 'perfetto', 'traced_probes'}
PLATFORM_HINT = re.compile(r'^(untrusted_app\w*|platform_app|priv_app|system_app|isolated_app\w*|gmscore_app|system_server|'
                           r'surfaceflinger|zygote|webview_zygote|app_zygote|init|vendor_init|ueventd|logd|lmkd|installd|vold|'
                           r'netd|audioserver|cameraserver|mediaserver|mediaextractor|mediaprovider\w*|keystore|gatekeeperd|'
                           r'statsd|storaged|traced|update_engine|apexd|servicemanager|hwservicemanager|vndservicemanager|'
                           r'bootanim|healthd|charger|kernel|bluetooth|nfc|radio|secure_element|wificond|hal_\w+_server)$')
CORE_PROP_WRITE = re.compile(r'(graphics_config_writable_prop|system_prop|exported\w*_prop|persist_\w+|default_prop)$')


def parse(paths):
    groups = collections.OrderedDict()
    for p in paths:
        with open(p, encoding='utf-8', errors='replace') as f:
            for ln in f:
                m = AVC.search(ln)
                if not m:
                    continue
                perms = m.group(1).split(); rest = m.group(2); s, t, c, perm = m.group(3), m.group(4), m.group(5), m.group(6)
                kv = dict(KV.findall(rest))
                g = groups.setdefault((s, t, c), {'perms': set(), 'n': 0, 'ex': [], 'blocking': False})
                g['perms'].update(perms); g['n'] += 1; g['blocking'] |= perm == '0'
                ex = ' '.join('%s=%s' % (k, kv[k]) for k in ('comm', 'name', 'path', 'capability') if k in kv)
                if ex and ex not in g['ex'] and len(g['ex']) < 4:
                    g['ex'].append(ex)
    return groups


def domain_dirs(repo):
    """domain/type -> policy dir (relative to device/hisense/a6l) that declares it"""
    base = os.path.join(repo, 'device/hisense/a6l'); owner = {}
    for root, _, files in os.walk(base):
        if 'sepolicy' not in root or '/superseded' in root or root.startswith(os.path.join(base, 'hals')):
            continue
        for fn in files:
            if fn.endswith('.te'):
                txt = open(os.path.join(root, fn), encoding='utf-8', errors='replace').read()
                for t in re.findall(r'^\s*type\s+(\w+)\s*[,;]', txt, re.M):
                    owner.setdefault(t, os.path.relpath(root, base))
    return owner


AOSP_VENDOR_DOMAINS = {'charger_vendor', 'vendor_modprobe', 'rild', 'tee', 'vndservicemanager'}


def is_vendor_domain(s, owner):
    return s in owner or s in AOSP_VENDOR_DOMAINS or s.startswith(('hal_', 'vendor_', 'a6l_')) and not s.endswith('_server')


def classify(s, t, c, perms, owner):
    if s in DEBUG_DOMAINS:
        return 'DEBUG', 'userdebug session noise'
    vend = is_vendor_domain(s, owner)
    if vend and c == 'file' and t in ('system_file', 'shell_exec', 'toolbox_exec', 'logcat_exec', 'system_linker_exec') and \
            perms & {'execute', 'execute_no_trans', 'entrypoint'}:
        return 'NEVERALLOW', 'vendor domain executing a system binary (%s): use /vendor/bin/sh + toybox, or move the step into a system/vendor daemon' % t
    if vend and c in ('capability', 'cap_userns') and perms & {'dac_override', 'dac_read_search'}:
        return 'NEVERALLOW', 'dac_override/dac_read_search are neverallowed for vendor domains: fix file owner/mode (ueventd/init chown)'
    if vend and c == 'property_service' and CORE_PROP_WRITE.search(t):
        return 'NEVERALLOW', 'vendor setting a platform property (%s): use a vendor.* / persist.vendor.* property or vendor_init' % t
    if vend and c in ('tcp_socket', 'udp_socket') and s.startswith('hal_') and 'create' in perms:
        return 'NEVERALLOW', 'HALs may not open network sockets (hal_neverallows.te)'
    if s == 'vendor_modprobe' and not (c == 'system' and 'module_load' in perms):
        return 'LABEL', 'service still runs with seclabel vendor_modprobe: build with A6L_SELINUX_PREP=1 (rc transform) so its script gets its own domain'
    if s == 'kernel' and t in ('vendor_file', 'unlabeled') and c == 'file':
        return 'LABEL', 'kernel firmware load: /vendor/firmware(/.*)? must be vendor_firmware_file (AOSP lets the kernel read it)'
    if not vend and s.endswith('app') or re.match(r'^(untrusted_app|isolated_app)', s):
        return 'PLATFORM', 'app domain: if the object is an a6l property/file, give it a proper type; never allow in vendor policy'
    if t in GENERIC_OBJ:
        return 'LABEL', 'generic type %s -> %s' % (t, GENERIC_OBJ[t])
    if not vend and (PLATFORM_HINT.match(s) or not s.startswith(('a6l_', 'hal_', 'vendor_'))):
        if s == 'kernel' and t in ('vendor_file', 'vendor_firmware_file'):
            return 'LABEL', 'kernel firmware load: /vendor/firmware must be vendor_firmware_file (AOSP allows kernel to read it)'
        if s == 'init' and c == 'process' and 'transition' not in perms:
            return 'PLATFORM', 'init: check for a service with no domain transition (file label missing)'
        return 'PLATFORM', 'platform/app domain: not fixed in vendor policy (check whether an a6l object needs a better label)'
    return 'ALLOW', ''


def rule(s, t, c, perms):
    return 'allow %s %s:%s %s;' % (s, 'self' if s == t and c in ('capability', 'capability2', 'netlink_kobject_uevent_socket',
                                                                     'udp_socket', 'tcp_socket', 'unix_dgram_socket') else t,
                                  c, perms[0] if len(perms) == 1 else '{ %s }' % ' '.join(perms))


def ps_check(path, owner):
    """processes of our binaries that run in init/kernel/vendor_modprobe (= no transition) or su"""
    bad = []
    for ln in open(path, encoding='utf-8', errors='replace'):
        f = ln.split()
        if len(f) < 9 or not f[0].startswith('u:r:'):
            continue
        dom = f[0].split(':')[2]; cmd = f[-1]
        if ('/vendor/' in cmd or 'a6l' in cmd) and dom in ('init', 'vendor_init', 'kernel', 'vendor_modprobe', 'su'):
            bad.append('%s runs as %s' % (cmd, dom))
    return bad


def main(a):
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')); out = None; ps = None; logs = []
    i = 0
    while i < len(a):
        if a[i] == '--repo': repo = a[i + 1]; i += 2
        elif a[i] == '--out': out = a[i + 1]; i += 2
        elif a[i] == '--ps': ps = a[i + 1]; i += 2
        else: logs.append(a[i]); i += 1
    if not logs:
        sys.exit(__doc__)
    owner = domain_dirs(repo); groups = parse(logs)
    cls = collections.defaultdict(list)
    for (s, t, c), g in groups.items():
        perms = sorted(g['perms']); k, why = classify(s, t, c, set(perms), owner)
        cls[k].append((s, t, c, perms, g, why))
    L = ['# A6L avc policy plan', '', 'logs: %s' % ', '.join(os.path.basename(x) for x in logs),
         'groups: %d (%s)' % (len(groups), ', '.join('%s %d' % (k, len(v)) for k, v in sorted(cls.items()))), '',
         'Nothing here is applied. Review, copy rules into the owning dir, then run check-a6l-sepolicy.sh user (must PASS).', '']
    proposed = collections.defaultdict(list)
    for k in ('NEVERALLOW', 'LABEL', 'ALLOW', 'PLATFORM', 'DEBUG'):
        if not cls.get(k):
            continue
        L += ['## %s (%d)' % (k, len(cls[k])), '']
        for s, t, c, perms, g, why in sorted(cls[k]):
            r = rule(s, t, c, perms)
            tgt = owner.get(s, 'rom/selinux/sepolicy/vendor')
            L.append('- %s`%s` x%d%s%s' % ('**BLOCKING** ' if g['blocking'] else '', r, g['n'],
                                          ' -> %s/%s.te' % (tgt, s) if k == 'ALLOW' else '', ('  (%s)' % why) if why else ''))
            for ex in g['ex']:
                L.append('    - %s' % ex)
            if k == 'ALLOW':
                proposed[(tgt, s)].append('%s   # x%d %s' % (r, g['n'], '; '.join(g['ex'][:2])))
        L.append('')
    if ps:
        bad = ps_check(ps, owner)
        L += ['## processes without their own domain (ps -AZ)', ''] + ['- ' + b for b in bad] + ([] if bad else ['- none']) + ['']
    text = '\n'.join(L)
    print(text)
    if out:
        os.makedirs(out, exist_ok=True)
        open(os.path.join(out, 'plan.md'), 'w').write(text + '\n')
        for (tgt, s), rules in proposed.items():
            d = os.path.join(out, 'proposed', tgt); os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, s + '.te'), 'w') as f:
                f.write('# PROPOSED by tools/release/a6l-avc-plan.py - review before copying into %s\n' % tgt + '\n'.join(rules) + '\n')
    print('A6L_AVC_PLAN groups=%d allow=%d label=%d neverallow=%d platform=%d debug=%d blocking=%d' % (
        len(groups), len(cls['ALLOW']), len(cls['LABEL']), len(cls['NEVERALLOW']), len(cls['PLATFORM']), len(cls['DEBUG']),
        sum(1 for g in groups.values() if g['blocking'])))


if __name__ == '__main__':
    main(sys.argv[1:])
