#!/usr/bin/env python3
"""V75-usb candidate (agent usbfix): the V74 captured-ABL regression (header, ufdt overlay + controls, DT fixup,
gzip decompress, DTB selection x3 PMIC variants, AVB) run on the V75-usb image, plus ramdisk checks:
the ramdisk section of the image decompresses to the reviewed cpio, which differs from V74's only by init.rc and the
new watchdog script."""
import gzip, hashlib, importlib.util, json, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'firmware/extracted/recovery-v75usb-candidate-20260925'
V74 = ROOT / 'firmware/extracted/recovery-v74-candidate-20260923'
sha = lambda b: hashlib.sha256(b).hexdigest()
def module(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), ROOT / f'tools/{name}.py')
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
def main():
    t = module('Test-RecoveryV74'); t.OUT = OUT
    t.main()   # raises on failure, writes OUT/captured-abl-validation.json
    rep = json.loads((OUT / 'captured-abl-validation.json').read_text()); assert rep['passed']
    pkg = json.loads((OUT / 'report.json').read_text())
    img = (OUT / 'recovery-diagnostic-unsigned.img').read_bytes()
    rs = pkg['layout']['sections']['ramdisk']
    rd = img[rs['offset']:rs['offset'] + rs['bytes']]
    assert sha(rd) == pkg['ramdisk_sha256'] == rs['sha256']
    prep = module('Prepare-RecoveryV75Usb')
    new, _ = prep.parse_newc(gzip.decompress(rd)); old, _ = prep.parse_newc(gzip.decompress((V74 / 'ramdisk.cpio.gz').read_bytes()))
    n = {e[0]: e for e in new}; o = {e[0]: e for e in old}
    assert set(n) - set(o) == {'system/bin/a6l_usb_watchdog.sh'} and not set(o) - set(n)
    diff = sorted(k for k in o if o[k][3] != n[k][3]); assert diff == ['system/etc/init/hw/init.rc'], diff
    wd = n['system/bin/a6l_usb_watchdog.sh']
    assert sha(wd[2]) == pkg['watchdog_sha256'] and wd[1][1] == 0o100755 and b'\r' not in wd[2]
    rc = n['system/etc/init/hw/init.rc'][2]; assert sha(rc) == pkg['init_rc_sha256'] and b'\r' not in rc
    assert rc.index(b'start a6lprobe') < rc.index(b'start a6lusbwd')
    k74 = (V74 / 'recovery-diagnostic-unsigned.img').read_bytes()
    ks = pkg['layout']['sections']['kernel']
    assert img[ks['offset']:ks['offset'] + ks['bytes']] == (V74 / 'Image.gz-dtb').read_bytes()
    rep['v75usb_ramdisk'] = {'passed': True, 'changed': diff, 'added': ['system/bin/a6l_usb_watchdog.sh'],
                             'kernel_dt_identical_to_v74': True}
    (OUT / 'captured-abl-validation.json').write_text(json.dumps(rep, indent=2) + '\n')
    print('V75USB_TEST_PASS', pkg['candidate_sha256'])
if __name__ == '__main__':
    main()
