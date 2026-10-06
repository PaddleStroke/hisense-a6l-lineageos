#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Static checks of A6LDisplaySwitcher (e-ink settings + tiles), android-eink-settings 29 Sep 2026.
# usage: check-app.py [DEVICE_DIR]   (default: ../../../.. = device/hisense/a6l). Prints PASS/FAIL lines, exit 1 on failure.
import os, re, sys, xml.etree.ElementTree as ET

here = os.path.dirname(os.path.abspath(__file__))
D = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.abspath(os.path.join(here, '../../../..'))
SW = os.path.join(D, 'eink/switcher'); APP = os.path.join(SW, 'app')
fails = 0; n = 0
def check(ok, what):
    global fails, n
    n += 1
    print(('PASS ' if ok else 'FAIL ') + what)
    if not ok: fails += 1
def rd(p): return open(p, encoding='utf-8').read()

A = 'http://schemas.android.com/apk/res/android'
def strings(p):
    r = {}
    for e in ET.parse(p).getroot().findall('string'):
        r[e.get('name')] = ''.join(e.itertext())
    return r
en = strings(os.path.join(APP, 'res/values/strings.xml'))
fr_p = os.path.join(APP, 'res/values-fr/strings.xml')
check(os.path.exists(fr_p), 'values-fr/strings.xml exists')
fr = strings(fr_p) if os.path.exists(fr_p) else {}
check(set(en) == set(fr), 'en/fr string keys identical (missing fr: %s, extra fr: %s)' % (sorted(set(en) - set(fr)), sorted(set(fr) - set(en))))
fmt = re.compile(r'%(\d+\$)?[-#+ 0,(]*\d*(\.\d+)?[sdf%]')
for k in sorted(set(en) & set(fr)):
    a = sorted(m.group(0) for m in fmt.finditer(en[k])); b = sorted(m.group(0) for m in fmt.finditer(fr[k]))
    if a != b: check(False, 'format args of %s: en %s fr %s' % (k, a, b))
    # aapt2 rejects a bare apostrophe in a string (the XML text here has it unescaped only if the raw file does)
raw = rd(fr_p) if fr else ''
check(not re.search(r"(?<!\\)'", re.sub(r'<!--.*?-->', '', raw, flags=re.S).split('<resources>', 1)[-1]), 'fr: every apostrophe escaped')
check(all(v.strip() for v in fr.values()), 'fr: no empty strings')

java = {}
for root, _, fs in os.walk(os.path.join(APP, 'src')):
    for f in fs:
        if f.endswith('.java'): java[f[:-5]] = rd(os.path.join(root, f))
alljava = '\n'.join(java.values())
refs = set(re.findall(r'R\.string\.(\w+)', alljava))
man = rd(os.path.join(APP, 'AndroidManifest.xml'))
refs |= set(re.findall(r'@string/(\w+)', man))
check(refs <= set(en), 'every referenced string exists (missing: %s)' % sorted(refs - set(en)))
unused = set(en) - refs - {'app_name'}
inline_patch = os.path.join(D, 'rom/android/patches/packages/apps/Settings/0001-a6l-display-tabs.patch')
# Settings obtains these same translated resources from the controller package.
external_refs = set(re.findall(r'"([a-z][a-z0-9_]*)"', rd(inline_patch))) if os.path.exists(inline_patch) else set()
check(not (unused - external_refs), 'no unused strings (%s)' % sorted(unused - external_refs))
draw = set(re.findall(r'@drawable/(\w+)', man)) | set(re.findall(r'R\.drawable\.(\w+)', alljava))
check(all(os.path.exists(os.path.join(APP, 'res/drawable', d + '.xml')) for d in draw), 'drawables exist')

mroot = ET.fromstring(man)
comps = [c.get('{%s}name' % A) for c in mroot.find('application') if c.tag in ('activity', 'service', 'receiver', 'provider')]
check(all(c.startswith('.') and c[1:] in java for c in comps), 'manifest components have sources: %s' % comps)
provider = mroot.find('application').find('provider')
check(provider is not None and provider.get('{%s}permission' % A) == 'android.permission.WRITE_SECURE_SETTINGS'
      and 'enforceCallingOrSelfPermission' in java.get('SettingsProvider', ''), 'inline provider enforces privileged access including call()')
tiles = [c for c in mroot.find('application') if c.tag == 'service']
check(len(tiles) >= 4 and all(t.get('{%s}permission' % A) == 'android.permission.BIND_QUICK_SETTINGS_TILE' for t in tiles), 'QS tiles bound with BIND_QUICK_SETTINGS_TILE')
check('DisplayTile' in [t.get('{%s}name' % A)[1:] for t in tiles], 'screen-switch tile present')
check('com.android.settings.action.EXTRA_SETTINGS' not in man and 'A6lEinkPreferences' in rd(inline_patch)
      and 'QS_TILE_PREFERENCES' in man, 'controls inline in Display, no duplicate navigation tile; long-press retained')

# no hard-coded user-visible English in tiles/settings (setText/setLabel/setSubtitle with a literal)
check(not re.search(r'set(Text|Label|Subtitle)\(\s*"', alljava), 'no literal UI text in Java')
# properties: the app writes only system_ext a6l_dualux_ctl_prop keys and reads no vendor_internal key
dj = java['Dualux']
props = dict(re.findall(r'static final String (P_\w+) = "([^"]+)"', dj))
pc = rd(os.path.join(SW, 'sepolicy/system_ext/private/property_contexts'))
ctl = [l.split()[0] for l in pc.splitlines() if 'a6l_dualux_ctl_prop' in l and not l.startswith('#')]
for k, v in props.items():
    if k == 'P_STATE': continue
    check(any(v == c or (c.endswith('.') and v.startswith(c)) for c in ctl), '%s=%s labelled a6l_dualux_ctl_prop' % (k, v))
check('persist.vendor.' not in re.sub(r'//.*', '', alljava), 'app reads no persist.vendor.* (vendor_internal, denied to system_app)')
te = rd(os.path.join(SW, 'sepolicy/system_ext/private/system_app.te'))
check('set_prop(system_app, a6l_dualux_ctl_prop)' in te, 'system_app may set a6l_dualux_ctl_prop')
vpc = rd(os.path.join(SW, 'sepolicy/vendor/property_contexts'))
check('vendor.dualux.state' in vpc and 'vendor_restricted_prop(vendor_dualux_prop)' in rd(os.path.join(SW, 'sepolicy/vendor/property.te')), 'vendor.dualux.state readable (vendor_restricted)')
# every setting the app writes is consumed by a daemon
src = rd(os.path.join(SW, 'native/a6l_dualux.c')) + rd(os.path.join(D, 'eink/src/a6l_eink_mirror.c'))
for k, v in props.items():
    if k in ('P_STATE', 'P_PER_SCREEN'): continue   # per_screen is app-internal (PerScreenMemory)
    check(v in src, '%s (%s) consumed by a6l_dualux / a6l_eink_mirror' % (k, v))
check('P_PER_SCREEN' in java.get('PerScreenMemory', ''), 'per_screen consumed by PerScreenMemory')

# product wiring: rom.mk -> dualux.mk -> PRODUCT_PACKAGES; Android.bp module; sepolicy dirs
rom = rd(os.path.join(D, 'rom/rom.mk'))
check('device/hisense/a6l/eink/switcher/dualux.mk' in rom, 'rom/rom.mk inherits dualux.mk')
mk = rd(os.path.join(SW, 'dualux.mk'))
check(all(p in mk for p in ('A6LDisplaySwitcher', 'a6l_dualux', 'privapp_whitelist_org.lineageos.a6l.dualux')), 'dualux.mk PRODUCT_PACKAGES')
bp = rd(os.path.join(SW, 'Android.bp'))
check('name: "A6LDisplaySwitcher"' in bp and 'resource_dirs: ["app/res"]' in bp and 'certificate: "platform"' in bp, 'Android.bp app module (res dir picks up values-fr)')
bc = rd(os.path.join(D, 'rom/BoardConfig-rom.mk'))
check('eink/switcher/sepolicy/vendor' in bc and 'switcher/sepolicy/system_ext/private' in bc, 'BoardConfig-rom.mk sepolicy dirs')
priv = rd(os.path.join(APP, 'privapp-permissions-a6l-dualux.xml'))
need = set(re.findall(r'uses-permission android:name="([^"]+)"', man)) - {'android.permission.WRITE_SETTINGS', 'android.permission.RECEIVE_BOOT_COMPLETED'}
check(all(p in priv for p in need), 'privileged permissions allowlisted: %s' % sorted(need))
check('filename: "privapp-permissions-a6l-dualux.xml"' in bp and 'system_ext_specific: true' in bp.split('prebuilt_etc {', 1)[1].split('}', 1)[0], 'allowlist installs as XML on the app partition (SystemConfig ignores extensionless files)')
print('%s %d/%d' % ('CHECK_APP PASS' if not fails else 'CHECK_APP FAIL', n - fails, n))
sys.exit(1 if fails else 0)
