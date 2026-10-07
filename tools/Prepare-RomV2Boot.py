#!/usr/bin/env python3
"""Assemble the A6L rom-v2 boot.img + dtbo.img (agent merge, 25 Sep 2026; = Prepare-RomV1Boot.py with the V75 DTB).
Offline, WSL. Nothing is flashed. The kernel is the SAME proven V67 Image; only the appended DTB changes:
  DTB = tools/build-rom-v2-dt.sh output (V74 base + voice/speaker, charger, flash, hall, haptics, camera, frontlight,
        ipa, microSD overlays; /chosen hisense,a6l-image = "rom-v2"). env A6L_V75_DTB = path of rom-v2.dtb.
(rom-v1 notes follow)

boot.img (header v1, same layout rules as the V74 recovery that boots on the phone):
  kernel  = V67 phone kernel Image (sha 0d7d2eb6..., same as V71/V74) gzip + appended DTB
  DTB     = V74 base.dtb (V68 base + 9 overlays: gpu, eink-pmic-reartouch, front-als, modem-wifi, display-native,
            eink-dsi, audio-internal, bluetooth-v74, eink-v74) + /chosen hisense,a6l-image = "rom-v1".
            NOT included: audio-speaker (V75; would make the whole card wait for the TFA9894, see recovery-v74 doc).
  ramdisk = Android FIRST-STAGE init (product out ramdisk/init) + /fstab.qcom + /lib/modules (sdhci-msm + modules.dep/load)
  cmdline = V74 recovery cmdline minus the diagnostic tracing/init_rc, plus androidboot.boot_devices, firmware path.
dtbo.img = the V74 recovery DTBO table (the ABL board-id selection overlay) — in normal boot the ABL reads the dtbo
           PARTITION ("BootLinux: failed to get dtbo image" otherwise), so it must hold the overlay that the recovery path embeds.
usage: Prepare-RomV1Boot.py <outdir> [--qemu]   (--qemu: ramdisk variant that loads virtio_mmio+virtio_blk instead)
"""
import gzip, hashlib, json, os, shutil, struct, subprocess, sys
from pathlib import Path
ROOT = Path('/mnt/c/Users/Pierre/Desktop/A6L')
LT = Path('/home/a6l/android/a6l-lineage24')
PROD = LT / 'out/target/product/a6l'
PACK = LT / 'system/tools/mkbootimg'
KERNEL = ROOT / 'firmware/extracted/phone-kernel-v67-candidate-20260919'
V74 = ROOT / 'firmware/extracted/recovery-v74-candidate-20260923'
ROMSRC = ROOT / 'device/hisense/a6l/rom'
GEN_CPIO = Path('/home/a6l/kernel/out-a6l-android-init/usr/gen_init_cpio')
V67_IMAGE = '0d7d2eb6692b0439a43306ce6f8e26b07334fe21897b8774f99317921fd5acf7'
# kernel r5 (30 Sep 2026, docs/kernel-android-config-20260930.md): A6L_KERNEL=r5 = the Android-config rebuild (MODVERSIONS=y,
# FBE/fs-verity/netd options): r5 Image + r5 first-stage sdhci-msm.ko (a V67 module is refused by the r5 kernel). Unset = V67, unchanged.
A6L_KERNEL = os.environ.get('A6L_KERNEL', 'v67')
assert A6L_KERNEL in ('v67', 'r5'), A6L_KERNEL
# thermal (29 Sep 2026, docs/kernel-android-config-20260930.md): r5 Image rebuilt with configs/a6l-thermal.config
# (THERMAL_EMERGENCY_POWEROFF_DELAY_MS 0 -> 100); modules/Module.symvers unchanged. Previous r5 Image 5f92e057 in pre-thermal-20260929/.
R5_IMAGE = 'de4970b302c1dbb2efde2718f6abb79ff0811ca726d1b41e92a3669e2c465fd6'
if A6L_KERNEL == 'r5':
    KERNEL = ROOT / 'firmware/extracted/kernel-r5-20260930'
KERNEL_SHA = R5_IMAGE if A6L_KERNEL == 'r5' else V67_IMAGE
# r6b boot fix (30 Sep 2026, docs/rom-r6b-bootfix-20260930.md): A6L_KERNEL_IMAGE=r5p (only with A6L_KERNEL=r5) boots the
# debug/first-install Image kernel-r5p-20260930 = the same r5 thermal kernel + CONFIG_CFI_PERMISSIVE=y (a kCFI mismatch warns
# instead of trapping). Module ABI identical (Module.symvers byte-identical), so the r5 modules/first-stage sdhci-msm stay.
A6L_KERNEL_IMAGE = os.environ.get('A6L_KERNEL_IMAGE', '')
assert A6L_KERNEL_IMAGE in ('', 'r5p', 'r5p-fencefix') and (A6L_KERNEL_IMAGE == '' or A6L_KERNEL == 'r5'), (A6L_KERNEL, A6L_KERNEL_IMAGE)
R5P_IMAGE = '124054c510f293810ea3e6eba54d4694761a03909428556c62658cfe0bdfbd3b'
KIMAGE = KERNEL / 'Image'
if A6L_KERNEL_IMAGE == 'r5p':
    KIMAGE = ROOT / 'firmware/extracted/kernel-r5p-20260930/Image'; KERNEL_SHA = R5P_IMAGE
elif A6L_KERNEL_IMAGE == 'r5p-fencefix':
    # 2 Oct: captured BUG in drm_crtc fence naming matches upstream 57acb869.
    # Config and built-in export CRCs checked unchanged; existing r5 modules.
    KF = ROOT / 'firmware/extracted/kernel-r5p-fencefix-20261002'
    kr = json.loads((KF / 'report.json').read_text())
    assert kr['compile_pass'] and kr['config_unchanged'] and kr['built_in_export_crcs_match']
    KIMAGE = KF / 'Image'
    KERNEL_SHA = 'c5b71f963cec190a5c249d10050937903cd09831d99a0f340015a3342fa0998a'
SDHCI = (KERNEL / 'ramdisk-modules/sdhci-msm.ko' if A6L_KERNEL == 'r5'
         else ROOT / 'firmware/extracted/rom-v1-20260924/stage/ramdisk-modules/sdhci-msm.ko')
V74_IMG = '24ede49b0f34b10f216617cb007c757e495fa2df1b6cd1ddd0afd63f9819da62'
V75_DTB = Path(os.environ['A6L_V75_DTB'])
BOOT_BYTES = 64 << 20
DTBO_BYTES = 8 << 20
CMDLINE = ('console=ttyMSM0,115200n8 earlycon=a6lfb keep_bootcon androidboot.hardware=qcom loglevel=6 clk_ignore_unused '
           'pd_ignore_unused regulator_ignore_unused panic=0 a6l_probe=1 a6l_manual_usb=1 androidboot.selinux=permissive '
           'androidboot.boot_devices=soc@0/c0c4000.mmc firmware_class.path=/vendor/firmware printk.devkmsg=on log_buf_len=4M androidboot.mode=normal')
# restart-hang fix (7 Oct 2026, firmware/extracted/restart-hang-20261007): the ABL appends androidboot.mode=charger when it
# sees USB_CHG (any restart or power-on with USB attached); charger mode = no mount_all/adb/UI = 'backlit black'. Our token
# comes first and ro.boot.mode is write-once, so Android always boots normally (off-mode charging is not supported).
# r6b boot fix (30 Sep 2026): log_buf_len=4M (CONFIG_LOG_BUF_SHIFT=17 = 128 KiB): the whole boot stays in the ring, so the
# userdebug boot log (/metadata/a6l/boot-kmsg.txt, started at post-fs) and dmesg from adb start at 0 s.
# release-prep (27 Sep 2026): A6L_SELINUX=enforcing drops androidboot.selinux=permissive (a `user` build is enforcing anyway;
# on userdebug this is the switch for the enforcing trial). A6L_FSTAB=fbe puts vendor-etc/fstab.qcom.fbe (FBE /data,
# persist context=) into the first-stage ramdisk. Both unset = the r3 boot.img, byte-for-byte.
if os.environ.get('A6L_SELINUX', 'permissive') == 'enforcing':
    CMDLINE = CMDLINE.replace(' androidboot.selinux=permissive', '')
FSTAB_NAME = 'fstab.qcom.fbe' if os.environ.get('A6L_FSTAB') == 'fbe' else 'fstab.qcom'
# bug hunt round2 install-tools (29 Sep 2026): the FBE fstab (fileencryption=) with a kernel built without CONFIG_FS_ENCRYPTION
# (the V67 Image: "# CONFIG_FS_ENCRYPTION is not set") gives an image that cannot set up /data encryption on its first boot
# (vold/init fail -> no Android). Refuse that combination before anything is built; A6L_FSTAB=fbe needs A6L_KERNEL=r5.
if FSTAB_NAME == 'fstab.qcom.fbe':
    _cfg = (KERNEL / 'config').read_text().splitlines() if (KERNEL / 'config').exists() else []
    if 'CONFIG_FS_ENCRYPTION=y' not in _cfg:
        raise SystemExit('ROM_V2_BOOT_REFUSED: A6L_FSTAB=fbe needs a kernel with CONFIG_FS_ENCRYPTION=y; %s/config does not '
                         'have it (A6L_KERNEL=%s). Use A6L_KERNEL=r5 or the default fstab.' % (KERNEL, A6L_KERNEL))
QEMU_CMDLINE_EXTRA = ' androidboot.boot_devices=any'
sha = lambda b: hashlib.sha256(b).hexdigest()


def run(*a, **k):
    return subprocess.check_output([str(x) for x in a], stderr=subprocess.STDOUT, **k)


def modinfo_depends(ko):
    data = Path(ko).read_bytes()
    i = data.find(b'depends=')
    while i >= 0:
        if i == 0 or data[i - 1] == 0:
            v = data[i + 8:data.index(b'\0', i)].decode()
            return [x for x in v.split(',') if x]
        i = data.find(b'depends=', i + 1)
    return []


def main():
    out = Path(sys.argv[1]); qemu = '--qemu' in sys.argv
    out.mkdir(parents=True, exist_ok=False)
    kernel = KIMAGE.read_bytes(); assert sha(kernel) == KERNEL_SHA
    assert sha((V74 / 'recovery-diagnostic-unsigned.img').read_bytes()) == V74_IMG
    base = out / 'rom-v2.dtb'; shutil.copyfile(V75_DTB, base)
    assert run('fdtget', base, '/chosen', 'hisense,a6l-image').strip() == b'rom-v2'
    assert run('fdtget', base, '/chosen', 'hisense,a6l-controls').strip() == b'v71'
    # --- ramdisk --------------------------------------------------------------------------------------------------
    init = PROD / 'ramdisk/init'
    info = run('file', '-L', init).decode()
    assert 'statically linked' in info and 'aarch64' in info, info
    rd = out / 'ramdisk-root'; (rd / 'lib/modules').mkdir(parents=True)
    shutil.copyfile(init, rd / 'init')
    shutil.copyfile(ROMSRC / 'vendor-etc' / FSTAB_NAME, rd / 'fstab.qcom')
    if qemu:
        mods = [Path(p) for p in sys.argv[sys.argv.index('--qemu') + 1:]]   # virtio_mmio.ko virtio_blk.ko
    else:
        mods = [SDHCI]
    names = []
    for m in mods:
        shutil.copyfile(m, rd / 'lib/modules' / m.name); names.append(m.name)
    dep = []
    for n in names:
        deps = modinfo_depends(rd / 'lib/modules' / n)
        missing = [d for d in deps if d.replace('_', '-') + '.ko' not in names and d + '.ko' not in names]
        assert not missing, (n, missing)
        dep.append(n + ':' + ''.join(' ' + (d + '.ko' if d + '.ko' in names else d.replace('_', '-') + '.ko') for d in deps))
    (rd / 'lib/modules/modules.dep').write_text('\n'.join(dep) + '\n')
    (rd / 'lib/modules/modules.load').write_text('\n'.join(names) + '\n')
    for f in ('modules.alias', 'modules.softdep', 'modules.options', 'modules.blocklist'):
        (rd / 'lib/modules' / f).write_text('')
    # first-stage init mounts tmpfs on /mnt, /debug_ramdisk, /second_stage_resources and needs the usual ramdisk mount points
    # (QEMU r1 without them: "mount tmpfs /mnt failed", "Init encountered errors starting first stage, aborting" -> panic)
    dirs = ['acct', 'apex', 'data', 'debug_ramdisk', 'dev', 'linkerconfig', 'metadata', 'mnt', 'odm', 'odm_dlkm', 'oem',
            'postinstall', 'proc', 'product', 'second_stage_resources', 'sys', 'system', 'system_dlkm', 'system_ext', 'tmp',
            'vendor', 'vendor_dlkm', 'lib', 'lib/modules']
    recipe = [f'dir /{d} 0755 0 0' for d in dirs] + ['nod /dev/console 0600 0 0 c 5 1', 'nod /dev/null 0666 0 0 c 1 3',
              f'file /init {rd/"init"} 0750 0 0', f'file /fstab.qcom {rd/"fstab.qcom"} 0644 0 0']
    for f in sorted(os.listdir(rd / 'lib/modules')):
        recipe.append(f'file /lib/modules/{f} {rd/"lib/modules"/f} 0644 0 0')
    (out / 'ramdisk.list').write_text('\n'.join(recipe) + '\n')
    cpio = run(GEN_CPIO, '-t', '1790208000', out / 'ramdisk.list')
    (out / 'ramdisk.cpio.gz').write_bytes(gzip.compress(cpio, mtime=0))
    # --- boot.img -------------------------------------------------------------------------------------------------
    payload = gzip.compress(kernel, mtime=0) + base.read_bytes(); (out / 'Image.gz-dtb').write_bytes(payload)
    raw = run(sys.executable, PACK / 'unpack_bootimg.py', '--boot_img', V74 / 'recovery-diagnostic-unsigned.img',
              '--out', out / 'v74-parts', '--format', 'mkbootimg', '-0').split(b'\0'); assert raw.pop() == b''
    args = [x.decode() for x in raw]
    i = args.index('--recovery_dtbo'); del args[i:i + 2]
    args[args.index('--kernel') + 1] = str(out / 'Image.gz-dtb')
    args[args.index('--ramdisk') + 1] = str(out / 'ramdisk.cpio.gz')
    args[args.index('--cmdline') + 1] = (CMDLINE.replace('androidboot.boot_devices=soc@0/c0c4000.mmc', 'androidboot.boot_devices=any console=ttyAMA0') if qemu else CMDLINE)
    assert args[args.index('--header_version') + 1] == '1'
    body = out / 'boot-body.img'
    run(sys.executable, PACK / 'mkbootimg.py', *args, '--output', body)
    v74 = (V74 / 'recovery-diagnostic-unsigned.img').read_bytes()
    b = bytearray(body.read_bytes()); b[28:32] = v74[28:32]; b = bytes(b); body.write_bytes(b)   # second_addr as V74 (see Prepare-RecoveryV74)
    assert len(b) < BOOT_BYTES
    h1, h2 = bytearray(v74[:1648]), bytearray(b[:1648])
    for a, e in [(8, 12), (16, 20), (64, 576), (576, 608), (1632, 1648)]:
        h1[a:e] = h2[a:e] = bytes(e - a)   # kernel size, ramdisk size, cmdline, id, recovery dtbo size/offset
    assert h1 == h2, 'unexpected boot header difference vs V74'
    # round trip
    raw2 = run(sys.executable, PACK / 'unpack_bootimg.py', '--boot_img', body, '--out', out / 'rt-parts', '--format', 'mkbootimg', '-0').split(b'\0'); raw2.pop()
    run(sys.executable, PACK / 'mkbootimg.py', *[x.decode() for x in raw2], '--output', out / 'roundtrip.img')
    rt = bytearray((out / 'roundtrip.img').read_bytes()); rt[28:32] = b[28:32]
    assert bytes(rt) == b, 'mkbootimg round trip differs'
    img = b + bytes(BOOT_BYTES - len(b)); (out / 'boot.img').write_bytes(img)
    dtbo = (V74 / 'recovery-dtbo.img').read_bytes(); (out / 'dtbo.img').write_bytes(dtbo + bytes(DTBO_BYTES - len(dtbo)))
    rep = {'boot_sha256': sha(img), 'boot_body_bytes': len(b), 'dtbo_sha256': sha((out / 'dtbo.img').read_bytes()),
           'dtbo_table_sha256': sha(dtbo), 'kernel_sha256': KERNEL_SHA, 'kernel': A6L_KERNEL + A6L_KERNEL_IMAGE[2:], 'dtb_sha256': sha(base.read_bytes()),
           'ramdisk_sha256': sha((out / 'ramdisk.cpio.gz').read_bytes()), 'init_sha256': sha((rd / 'init').read_bytes()),
           'cmdline': args[args.index('--cmdline') + 1], 'fstab': FSTAB_NAME, 'modules': names, 'modules_dep': dep, 'qemu_variant': qemu,
           'mkbootimg_args': [a if not a.startswith(str(out)) else Path(a).name for a in args]}
    (out / 'report.json').write_text(json.dumps(rep, indent=2) + '\n')
    print('ROM_V2_BOOT_PASS', rep['boot_sha256'], rep['dtbo_sha256'])


if __name__ == '__main__':
    main()
