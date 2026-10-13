#!/usr/bin/env python3
"""A6L diag-r5 (30 Sep 2026, docs/diag-r5-20260930.md): DIAGNOSTIC boot images for the BOOT partition.
Offline, WSL. Nothing is flashed. Purpose: find why `insmod msm.ko separate_gpu_kms=1` freezes the phone on the r5 kernel
family (r6/r6b) while the same list worked on V67 from the V74/V75 diagnostic recovery.

Each image = header v1 like the V74/V75 recovery and the ROM boot.img (tools/Prepare-RomV2Boot.py layout rules):
  kernel  = gzip(Image) + appended DTB     (variant, below)
  ramdisk = the V75-usb diagnostic recovery ramdisk (8ecb8e9e image, ramdisk 40479307) - RAM only: adbd (root, serial
            HLTE730T-PROBE), a6l_android_probe (configfs gadget, UDC bind, soft-connect, /sdhci-msm.ko at ~20 s),
            a6l_usb_watchdog.sh. Only /sdhci-msm.ko is replaced (r5 build for r5 kernels; MODVERSIONS refuses the V67 one).
            Every other cpio entry (header + data) is byte-identical.
  cmdline = the V75 recovery cmdline (initcall_debug, *_ignore_unused, panic=0, a6l_manual_usb, traces, init_rc)
            + log_buf_len=4M printk.devkmsg=on + androidboot.a6l_diag=<variant> (-> ro.boot.a6l_diag).
  No recovery_dtbo: in normal boot the ABL takes the board overlay from the dtbo PARTITION (= V74 recovery-dtbo table,
  installed by r6/r6b and unchanged). The ABL appends `skip_initramfs rootwait ro init=/init root=...`: harmless (mainline
  kernel unpacks the initramfs; /init = Android first-stage init, which sees /system/bin/recovery -> recovery mode, no
  first-stage mount, then second stage with androidboot.init_rc = the ramdisk's RAM-only init.rc).
Variants:
  r5p-dt6b  (MAIN) r5p Image 124054c5 + dt-r6b (dd279f2c, = r6b boot.img, incl. ramoops) -> Image.gz-dtb must equal r6b's
  r5p-dtv74 r5p Image + V74/V75 recovery base.dtb (aceadcb7) + the same ramoops properties   (DT vs kernel)
  v67-dt6b  V67 Image 0d7d2eb6 + dt-r6b, V75 ramdisk unchanged (V67 sdhci-msm)              (ROM DT on the proven kernel)
usage: Prepare-DiagR5Boot.py <outdir> <variant>
"""
import gzip, hashlib, json, os, shutil, struct, subprocess, sys
from pathlib import Path
ROOT = Path('/mnt/c/Users/Pierre/Desktop/A6L')
X = ROOT / 'firmware/extracted'
PACK = Path('/home/a6l/android/a6l-lineage24/system/tools/mkbootimg')
V75 = X / 'recovery-v75usb-candidate-20260925'
V74 = X / 'recovery-v74-candidate-20260923'
R6B = Path('/home/a6l/rom-v2/boot-r6b')
sha = lambda b: hashlib.sha256(b).hexdigest()
PIN = {
    'v75_img': '8ecb8e9e4c5289cfe63c23eb32b749178935a6588673cea3a1b1ebee3c94b304',
    'v75_ramdisk': '404793070039f2404659f74afac863dcba0e32092d8b0fc39246db3770c892e8',
    'v74_img': '24ede49b0f34b10f216617cb007c757e495fa2df1b6cd1ddd0afd63f9819da62',
    'v74_base_dtb': 'aceadcb790ded3f4b6da770263000eb2f6031f19fb8ff85d521a70346b98637e',
    'r6b_dtb': 'dd279f2c89625535d8dcdef4496de35775f05764b4be905e85779a533af0b27f',
    'r6b_boot': '3616b0fa63e8af3928d1d5df146f73327620270d81198968c82f8140be0a2947',
    'r5p_image': '124054c510f293810ea3e6eba54d4694761a03909428556c62658cfe0bdfbd3b',
    'v67_image': '0d7d2eb6692b0439a43306ce6f8e26b07334fe21897b8774f99317921fd5acf7',
    'r5_sdhci': None,   # filled from kernel-r5-20260930/SHA256SUMS
}
V75_CMDLINE = ('console=ttyMSM0,115200n8 earlycon=a6lfb androidboot.hardware=qcom loglevel=8 clk_ignore_unused '
               'pd_ignore_unused regulator_ignore_unused panic=0 a6l_probe=1 keep_bootcon initcall_debug a6l_manual_usb=1 '
               'a6l_usb_trace=1 a6l_storage_trace=1 androidboot.selinux=permissive androidboot.init_rc=/system/etc/init/hw/init.rc')
EXTRA = ' log_buf_len=4M printk.devkmsg=on androidboot.a6l_diag=%s'
BOOT_BYTES = 64 << 20
DTBO_BYTES = 8 << 20
# ramoops on the stock recorder_mem (same properties as device/hisense/a6l/kernel/a6l-ramoops-v75.dtso / dt-r6b)
RAMOOPS = [('compatible', '-ts', 'ramoops'), ('console-size', '-tx', '200000'), ('pmsg-size', '-tx', '40000'),
           ('record-size', '-tx', '20000'), ('max-reason', '-tx', '2')]
VARIANTS = {
    'r5p-dt6b': ('r5p', 'r6b'),
    'r5p-dtv74': ('r5p', 'v74'),
    'v67-dt6b': ('v67', 'r6b'),
    # kernel bisect variants (tools/build-diag-r5-kvariants.sh, collected in firmware/extracted/diag-r5-20260930/kernel-<k>/):
    'k1-dt6b': ('k1', 'r6b'),     # r5p - KFENCE - BUG_ON_DATA_CORRUPTION (own sdhci-msm/kit modules: SLAB flag bits shift)
    'r5m-dt6b': ('r5m', 'r6b'),   # V67 config + rom-v2 frag + MODVERSIONS/thermal/CFI-permissive (own sdhci-msm/kit modules)
}
DIAGK = X / 'diag-r5-20260930'


def run(*a, **k):
    return subprocess.check_output([str(x) for x in a], stderr=subprocess.STDOUT, **k)


def newc_entries(buf):
    """Parse a newc cpio archive -> list of (name, header_bytes(110), name_bytes_padded, data, data_pad) + trailer tail."""
    out, i = [], 0
    while True:
        h = buf[i:i + 110]; assert h[:6] == b'070701', (i, h[:6])
        f = [int(h[6 + 8 * k:14 + 8 * k], 16) for k in range(13)]
        filesize, namesize = f[6], f[11]
        name = buf[i + 110:i + 110 + namesize - 1].decode()
        nend = i + 110 + namesize; npad = (-nend) % 4
        dstart = nend + npad; dend = dstart + filesize; dpad = (-dend) % 4
        out.append({'name': name, 'hdr': bytearray(h), 'namefield': buf[i + 110:nend + npad], 'data': buf[dstart:dend],
                    'dpad': buf[dend:dend + dpad], 'fields': f})
        i = dend + dpad
        if name == 'TRAILER!!!':
            return out, buf[i:]


def newc_build(entries, tail):
    b = bytearray()
    for e in entries:
        h = bytearray(e['hdr']); h[6 + 8 * 6:14 + 8 * 6] = b'%08X' % len(e['data'])
        b += h + e['namefield'] + e['data'] + bytes((-(len(b) + len(h) + len(e['namefield']) + len(e['data']))) % 4)
    return bytes(b) + tail


def main():
    out = Path(sys.argv[1]); variant = sys.argv[2]
    kern, dtsel = VARIANTS[variant]
    out.mkdir(parents=True, exist_ok=False)
    rep = {'variant': variant, 'kernel': kern, 'dt': dtsel}
    v75 = (V75 / 'recovery-diagnostic-unsigned.img').read_bytes(); assert sha(v75) == PIN['v75_img']
    v74 = (V74 / 'recovery-diagnostic-unsigned.img').read_bytes(); assert sha(v74) == PIN['v74_img']
    # --- kernel ---------------------------------------------------------------------------------------------------
    if kern == 'r5p':
        kimg = (X / 'kernel-r5p-20260930/Image').read_bytes(); assert sha(kimg) == PIN['r5p_image']
    elif kern == 'v67':
        kimg = (X / 'phone-kernel-v67-candidate-20260919/Image').read_bytes(); assert sha(kimg) == PIN['v67_image']
    else:
        kd = DIAGK / ('kernel-' + kern)
        ks = dict(l.split()[::-1] for l in (kd / 'SHA256SUMS').read_text().splitlines() if l.strip())
        kimg = (kd / 'Image').read_bytes(); assert sha(kimg) == ks['Image'], 'kernel-%s Image sha' % kern
    rep['kernel_sha256'] = sha(kimg)
    # --- DT -------------------------------------------------------------------------------------------------------
    dtb = out / 'diag.dtb'
    if dtsel == 'r6b':
        shutil.copyfile('/home/a6l/rom-v2/dt-r6b/rom-v2.dtb', dtb); assert sha(dtb.read_bytes()) == PIN['r6b_dtb']
    else:
        shutil.copyfile(V75 / 'base.dtb', dtb); assert sha(dtb.read_bytes()) == PIN['v74_base_dtb']
        node = '/reserved-memory/memory@b0180000'
        assert run('fdtget', '-t', 'x', dtb, node, 'reg').split() == [b'0', b'b0180000', b'0', b'400000']
        run('fdtget', dtb, node, 'no-map')
        for p, t, v in RAMOOPS:
            run('fdtput', t, dtb, node, p, v)
        assert run('fdtget', dtb, node, 'compatible').strip() == b'ramoops'
    assert run('fdtget', dtb, '/chosen', 'hisense,a6l-controls').strip() == b'v71'
    rep['dtb_sha256'] = sha(dtb.read_bytes())
    rep['dt_image_marker'] = run('fdtget', dtb, '/chosen', 'hisense,a6l-image').strip().decode()
    payload = gzip.compress(kimg, mtime=0) + dtb.read_bytes(); (out / 'Image.gz-dtb').write_bytes(payload)
    if variant == 'r5p-dt6b':
        r6b = (R6B / 'boot.img').read_bytes(); assert sha(r6b) == PIN['r6b_boot']
        assert payload == (R6B / 'Image.gz-dtb').read_bytes(), 'kernel+DT payload differs from the r6b boot.img one'
        ks = struct.unpack_from('<I', r6b, 8)[0]; assert r6b[4096:4096 + ks] == payload
        rep['payload_equals_r6b_boot_kernel_section'] = True
    # --- ramdisk --------------------------------------------------------------------------------------------------
    rdgz = (V75 / 'ramdisk.cpio.gz').read_bytes(); assert sha(rdgz) == PIN['v75_ramdisk']
    cpio = gzip.decompress(rdgz)
    ents, tail = newc_entries(cpio)
    assert newc_build(ents, tail) == cpio, 'cpio round trip'
    names = [e['name'] for e in ents]
    sd = [e for e in ents if e['name'] == 'sdhci-msm.ko']; assert len(sd) == 1
    rep['v75_sdhci_sha256'] = sha(sd[0]['data'])
    if kern == 'r5p':
        sums = dict(l.split()[::-1] for l in (X / 'kernel-r5-20260930/SHA256SUMS').read_text().splitlines() if l.strip())
        new = (X / 'kernel-r5-20260930/ramdisk-modules/sdhci-msm.ko').read_bytes()
        key = [k for k in sums if k.endswith('ramdisk-modules/sdhci-msm.ko')]; assert len(key) == 1 and sums[key[0]] == sha(new)
        assert b'vermagic=7.2.3-a6l-probe+ SMP preempt mod_unload modversions aarch64' in new
        sd[0]['data'] = new
    elif kern in ('r5m', 'k1'):
        new = (DIAGK / ('kernel-' + kern) / 'sdhci-msm.ko').read_bytes(); assert sha(new) == ks['sdhci-msm.ko']
        assert b'vermagic=7.2.3-a6l-probe+ SMP preempt mod_unload modversions aarch64' in new
        sd[0]['data'] = new
    rep['sdhci_sha256'] = sha(sd[0]['data'])
    newcpio = newc_build(ents, tail)
    e2, _ = newc_entries(newcpio)
    diffs = [a['name'] for a, b in zip(ents, e2) if a['data'] != b['data']]
    same_hdr = all(bytes(a['hdr'][:54]) + bytes(a['hdr'][62:]) == bytes(b['hdr'][:54]) + bytes(b['hdr'][62:]) for a, b in zip(newc_entries(cpio)[0], e2))
    assert [e['name'] for e in e2] == names and same_hdr
    orig = {e['name']: e['data'] for e in newc_entries(cpio)[0]}
    rep['ramdisk_changed_entries'] = [e['name'] for e in e2 if orig[e['name']] != e['data']]
    assert rep['ramdisk_changed_entries'] in ([], ['sdhci-msm.ko'])
    (out / 'ramdisk.cpio').write_bytes(newcpio)
    rd = gzip.compress(newcpio, compresslevel=9, mtime=0) if rep['ramdisk_changed_entries'] else rdgz
    (out / 'ramdisk.cpio.gz').write_bytes(rd); rep['ramdisk_sha256'] = sha(rd); rep['ramdisk_entries'] = len(names)
    # --- boot.img (V75 header as template, recovery_dtbo dropped like Prepare-RomV2Boot) ---------------------------
    cmdline = V75_CMDLINE + EXTRA % variant
    raw = run(sys.executable, PACK / 'unpack_bootimg.py', '--boot_img', V75 / 'recovery-diagnostic-unsigned.img',
              '--out', out / 'v75-parts', '--format', 'mkbootimg', '-0').split(b'\0'); assert raw.pop() == b''
    args = [x.decode() for x in raw]
    assert args[args.index('--cmdline') + 1] == V75_CMDLINE, 'V75 cmdline changed?'
    i = args.index('--recovery_dtbo'); del args[i:i + 2]
    args[args.index('--kernel') + 1] = str(out / 'Image.gz-dtb')
    args[args.index('--ramdisk') + 1] = str(out / 'ramdisk.cpio.gz')
    args[args.index('--cmdline') + 1] = cmdline
    assert args[args.index('--header_version') + 1] == '1' and len(cmdline) < 512
    body = out / 'boot-body.img'
    run(sys.executable, PACK / 'mkbootimg.py', *args, '--output', body)
    b = bytearray(body.read_bytes()); b[28:32] = v74[28:32]; b = bytes(b); body.write_bytes(b)
    assert len(b) < BOOT_BYTES
    r6b = (R6B / 'boot.img').read_bytes()
    h1, h2 = bytearray(r6b[:1648]), bytearray(b[:1648])
    for a, e in [(8, 12), (16, 20), (64, 576), (576, 608), (1632, 1648)]:
        h1[a:e] = h2[a:e] = bytes(e - a)
    assert h1 == h2, 'unexpected boot header difference vs the r6b boot.img'
    raw2 = run(sys.executable, PACK / 'unpack_bootimg.py', '--boot_img', body, '--out', out / 'rt-parts', '--format', 'mkbootimg', '-0').split(b'\0'); raw2.pop()
    run(sys.executable, PACK / 'mkbootimg.py', *[x.decode() for x in raw2], '--output', out / 'roundtrip.img')
    rt = bytearray((out / 'roundtrip.img').read_bytes()); rt[28:32] = b[28:32]
    assert bytes(rt) == b, 'mkbootimg round trip differs'
    img = b + bytes(BOOT_BYTES - len(b)); (out / 'boot.img').write_bytes(img)
    dtbo = (V74 / 'recovery-dtbo.img').read_bytes(); (out / 'dtbo.img').write_bytes(dtbo + bytes(DTBO_BYTES - len(dtbo)))
    shutil.copyfile(dtb, out / 'rom-v2.dtb')   # name expected by the ABL emulation
    rep.update({'boot_sha256': sha(img), 'boot_body_bytes': len(b), 'boot_bytes': len(img), 'dtbo_sha256': sha((out / 'dtbo.img').read_bytes()),
                'dtbo_table_sha256': sha(dtbo), 'cmdline': cmdline, 'qemu_variant': False,
                'mkbootimg_args': [a if not a.startswith(str(out)) else Path(a).name for a in args]})
    (out / 'report.json').write_text(json.dumps(rep, indent=2) + '\n')
    print('DIAG_R5_BOOT_PASS', variant, rep['boot_sha256'], 'body', len(b))


if __name__ == '__main__':
    main()
