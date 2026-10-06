#!/usr/bin/env python3
"""Describe a staged ROM kit for the "preserve userdata" update (agent update-keepdata, 29 Sep 2026). Offline, WSL.
usage: Make-RomUpdateCompat.py <kit images dir> [--out <json>]      (default: print)
Extracts system.erofs and vendor.erofs with the tree's fsck.erofs into a temporary directory (deleted afterwards) and
records what decides whether a new build may boot the /data of an installed one (RomUpdateLayoutV1.compat_problems):
  system_fingerprint, sdk, security_patch, build_type, build tags   <- system build.prop
  vendor_security_patch                                             <- vendor build.prop
  platform_cert_sha256    <- sha256 of the DER certificate of seinfo "platform" in plat_mac_permissions.xml
  data_fs, data_encryption <- the /data line of vendor etc/fstab.qcom (fileencryption=/metadata_encryption flags)
  images                  <- the kit pins (binds this file to exactly those images)
"""
import hashlib, json, os, re, shutil, subprocess, sys, tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

FSCK = os.environ.get('A6L_FSCK_EROFS', '/home/a6l/android/a6l-lineage24/out/host/linux-x86/bin/fsck.erofs')


def props(text):
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if line and not line.startswith('#') and '=' in line:
            k, v = line.split('=', 1)
            out[k.strip()] = v.strip()
    return out


def first(root, *rels):
    for r in rels:
        p = root / r
        if p.is_file():
            return p
    raise FileNotFoundError('none of %s in %s' % (rels, root))


def platform_cert(xml_text):
    root = ET.fromstring(xml_text)
    for signer in root.iter('signer'):
        if any(s.get('value') == 'platform' for s in signer.iter('seinfo')):
            return hashlib.sha256(bytes.fromhex(signer.get('signature'))).hexdigest()
    raise ValueError('no platform signer in plat_mac_permissions.xml')


def data_line(fstab_text):
    for line in fstab_text.splitlines():
        f = line.split()
        if len(f) >= 5 and not f[0].startswith('#') and f[1] == '/data':
            flags = f[4].split(',')
            enc = sorted(x for x in flags if x.startswith(('fileencryption', 'forceencrypt', 'encryptable', 'metadata_encryption', 'keydirectory')))
            return f[2], ','.join(enc) or 'none'
    raise ValueError('no /data entry in fstab')


def extract(img, dest):
    r = subprocess.run([FSCK, '--extract=' + str(dest), '--no-preserve', str(img)], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError('fsck.erofs failed on %s: %s' % (img, r.stderr[-500:]))


def describe(images_dir, sysroot=None, venroot=None):
    images_dir = Path(images_dir)
    pins = json.loads((images_dir / 'rom-v1-pins.json').read_text())
    tmp = None
    try:
        if sysroot is None or venroot is None:
            tmp = Path(tempfile.mkdtemp(prefix='a6l-compat-'))
            sysroot, venroot = tmp / 'system', tmp / 'vendor'
            extract(images_dir / pins['images']['system']['file'], sysroot)
            extract(images_dir / pins['images']['vendor']['file'], venroot)
        sysroot, venroot = Path(sysroot), Path(venroot)
        sp = props(first(sysroot, 'system/build.prop', 'build.prop').read_text(errors='replace'))
        vp = props(first(venroot, 'build.prop', 'vendor/build.prop').read_text(errors='replace'))
        cert = platform_cert(first(sysroot, 'system/etc/selinux/plat_mac_permissions.xml', 'etc/selinux/plat_mac_permissions.xml').read_text())
        fs, enc = data_line(first(venroot, 'etc/fstab.qcom', 'vendor/etc/fstab.qcom').read_text())
        tags = sp.get('ro.system.build.tags') or sp.get('ro.build.tags')
        return {
            'schema': 1,
            'system_fingerprint': sp.get('ro.system.build.fingerprint') or sp.get('ro.build.fingerprint'),
            'vendor_fingerprint': vp.get('ro.vendor.build.fingerprint'),
            'sdk': int(sp.get('ro.build.version.sdk') or sp.get('ro.system.build.version.sdk') or 0),
            'security_patch': sp.get('ro.build.version.security_patch'),
            'vendor_security_patch': vp.get('ro.vendor.build.security_patch') or sp.get('ro.build.version.security_patch'),
            'build_type': sp.get('ro.system.build.type') or sp.get('ro.build.type'),
            'build_tags': tags,
            'build_date_utc': sp.get('ro.system.build.date.utc') or sp.get('ro.build.date.utc'),
            'lineage_version': sp.get('ro.lineage.version'),
            'platform_cert_sha256': cert,
            'data_fs': fs,
            'data_encryption': enc,
            'boot_fstab': pins.get('boot_report', {}).get('fstab'),
            'rom': pins.get('rom'),
            'images': {n: pins['images'][n]['sha256'] for n in ('boot', 'dtbo', 'vendor', 'system')},
        }
    finally:
        if tmp is not None:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    d = describe(sys.argv[1])
    text = json.dumps(d, indent=2) + '\n'
    if '--out' in sys.argv:
        out = Path(sys.argv[sys.argv.index('--out') + 1])
        with open(out, 'x') as f:
            f.write(text)
    print(text, end='')
    missing = [k for k in ('system_fingerprint', 'sdk', 'security_patch', 'vendor_security_patch', 'platform_cert_sha256') if not d.get(k)]
    if missing:
        print('A6L_COMPAT_INCOMPLETE ' + ' '.join(missing), file=sys.stderr)
        sys.exit(2)
