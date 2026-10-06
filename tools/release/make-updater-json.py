#!/usr/bin/env python3
"""LineageOS Updater JSON for a signed A6L OTA zip (release-prep, 27 Sep 2026).

LineageOS 24's Updater (packages/apps/Updater, data/source/network/NetworkUpdate.kt) fetches lineage.updater.uri
({device} -> ro.lineage.device = a6l) and decodes a JSON *list* of builds:
  [{"datetime": <utc s>, "type": "<releasetype>", "version": "24.0",
    "files": [{"filename", "sha256", "size", "url", "os_patch_level", "os_sdk_level"[, "ota_property_files"]}]}]
- sha256 is also the download id.  - datetime must be >= ro.build.date.utc of the installed build and os_sdk_level must be
  set (37): a missing os_sdk_level is read as 0 < 37 = "downgrade" and the update is BLOCKED.
- ota_property_files is only for A/B streaming (the A6L is A-only: omitted).
--legacy also prints the pre-v2 format ({"response":[{datetime, filename, id, romtype, size, url, version}]}) for old
Updater builds / third-party servers.

usage: make-updater-json.py <ota.zip> --url <download url> [--type unofficial] [--version 24.0] [--merge old.json] [--legacy]
"""
import argparse, hashlib, json, os, re, sys, zipfile


def ota_metadata(z):
    md = {}
    for name in ('META-INF/com/android/metadata',):
        if name in z.namelist():
            for ln in z.read(name).decode().splitlines():
                if '=' in ln:
                    k, v = ln.split('=', 1); md[k.strip()] = v.strip()
    if not md:
        sys.exit('no META-INF/com/android/metadata in the zip: not an ota_from_target_files package')
    return md


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ota'); ap.add_argument('--url', required=True)
    ap.add_argument('--type', default=None); ap.add_argument('--version', default=None)
    ap.add_argument('--merge'); ap.add_argument('--legacy', action='store_true')
    a = ap.parse_args()
    if not a.url.startswith('https://'):
        sys.exit('the Updater refuses non-HTTPS URLs')
    h = hashlib.sha256()
    with open(a.ota, 'rb') as f:
        for blk in iter(lambda: f.read(1 << 20), b''): h.update(blk)
    size = os.path.getsize(a.ota); fn = os.path.basename(a.ota)
    with zipfile.ZipFile(a.ota) as z:
        md = ota_metadata(z)
    ts = int(md['post-timestamp'])
    sdk = int(md.get('post-sdk-level', '0') or 0)
    spl = md.get('post-security-patch-level', '')
    if md.get('ota-type', 'BLOCK') == 'AB':
        print('warning: A/B package, but the A6L is A-only', file=sys.stderr)
    dev = md.get('pre-device', '')
    if dev and dev != 'a6l':
        sys.exit(f'package is for device {dev!r}, not a6l')
    m = re.match(r'lineage-(\d+\.\d+)-\d{8}-([A-Za-z]+)', fn)
    version = a.version or (m.group(1) if m else '24.0')
    rtype = (a.type or (m.group(2) if m else 'unofficial')).lower()
    if not sdk:
        sys.exit('post-sdk-level missing from the OTA metadata: the Updater would treat the build as a downgrade')
    entry = {'datetime': ts, 'type': rtype, 'version': version,
             'files': [{'filename': fn, 'sha256': h.hexdigest(), 'size': size, 'url': a.url,
                        'os_patch_level': spl, 'os_sdk_level': sdk}]}
    builds = []
    if a.merge and os.path.exists(a.merge):
        builds = [b for b in json.load(open(a.merge)) if b['files'][0]['sha256'] != entry['files'][0]['sha256']]
    builds.append(entry); builds.sort(key=lambda b: b['datetime'], reverse=True)
    if a.legacy:
        print(json.dumps({'response': [{'datetime': b['datetime'], 'filename': b['files'][0]['filename'],
                                        'id': b['files'][0]['sha256'], 'romtype': b['type'], 'size': b['files'][0]['size'],
                                        'url': b['files'][0]['url'], 'version': b['version']} for b in builds]}, indent=2))
    else:
        print(json.dumps(builds, indent=2))


if __name__ == '__main__':
    main()
