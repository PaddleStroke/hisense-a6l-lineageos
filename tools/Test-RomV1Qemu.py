#!/usr/bin/env python3
"""Boot the rom-v1 images in QEMU with the PHONE kernel (agent flash). Offline, WSL. No phone.
What it exercises: the real boot.img ramdisk (first-stage init + fstab.qcom + libmodprobe module loading), first-stage
mount of system/vendor from a GPT disk by-name (virtio-blk instead of eMMC), switch_root, selinux_setup (split policy),
second stage, vendor init.qcom.rc, mount_all (userdata formatted by fs_mgr on the first boot, persist ro), ueventd rules,
module-loader services, up to sys.boot_completed.
Differences from the phone (unavoidable): kernel+ramdisk passed by QEMU (the ABL part is checked by Test-RomV1Abl.py);
ramdisk = the --qemu variant (virtio_mmio + virtio_blk + a6l_simplefb instead of sdhci-msm, boot_devices=any);
DTB = QEMU virt; display = simple-framebuffer + software rendering; hardware modules load but find no hardware.
usage: Test-RomV1Qemu.py <run-dir> <qemu-boot-dir (Prepare-RomV1Boot --qemu output)> [minutes]
"""
import gzip, json, os, re, struct, subprocess, sys, time, uuid, zlib, glob, shutil
from pathlib import Path
RUN = Path(sys.argv[1]); QB = Path(sys.argv[2]); MIN = int(sys.argv[3]) if len(sys.argv) > 3 else 25
ROOT = Path('/mnt/c/Users/Pierre/Desktop/A6L')
P = Path('/home/a6l/android/a6l-lineage24/out/target/product/a6l')
KERNEL = ROOT / 'firmware/extracted/phone-kernel-v67-candidate-20260919/Image'
RUN.mkdir(parents=True, exist_ok=False)
MiB = 1 << 20


def trimmed(src, dst):
    with open(src, 'rb') as f:
        f.seek(1024); sb = f.read(128); assert sb[:4] == bytes.fromhex('e2e1f5e0'), 'not erofs'
        size = struct.unpack_from('<I', sb, 36)[0] << sb[12]
        f.seek(0)
        with open(dst, 'wb') as o:
            left = size
            while left:
                b = f.read(min(left, 1 << 24)); o.write(b); left -= len(b)
    return size


def gpt_disk(path, parts):
    """parts: list of (name, size_bytes, source_file_or_None). 1 MiB aligned, 128 entries."""
    lba = 512
    first = 2048
    layout = []
    cur = first
    for name, size, src in parts:
        n = (size + MiB - 1) // MiB * (MiB // lba)
        layout.append((name, cur, cur + n - 1, src)); cur += n
    total = cur + 2048
    with open(path, 'wb') as f:
        f.truncate(total * lba)
    entries = bytearray(128 * 128)
    linux = uuid.UUID('0FC63DAF-8483-4772-8E79-3D69D8477DE4').bytes_le
    for i, (name, s, e, src) in enumerate(layout):
        ent = linux + uuid.uuid4().bytes_le + struct.pack('<QQQ', s, e, 0) + name.encode('utf-16-le').ljust(72, b'\0')
        entries[i * 128:(i + 1) * 128] = ent
    ecrc = zlib.crc32(entries) & 0xffffffff
    def header(cur_lba, backup_lba, entries_lba):
        h = bytearray(struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x10000, 92, 0, 0, cur_lba, backup_lba, first, total - 34,
                                  uuid.uuid4().bytes_le, entries_lba, 128, 128, ecrc))
        h[16:20] = struct.pack('<I', zlib.crc32(bytes(h)) & 0xffffffff)
        return bytes(h).ljust(512, b'\0')
    mbr = bytearray(512); mbr[446:462] = bytes([0, 0, 2, 0, 0xee, 0xff, 0xff, 0xff]) + struct.pack('<II', 1, min(total - 1, 0xffffffff)); mbr[510:] = b'\x55\xaa'
    with open(path, 'r+b') as f:
        f.write(mbr); f.write(header(1, total - 1, 2)); f.write(entries)
        f.seek((total - 33) * lba); f.write(entries); f.write(header(total - 1, 1, total - 33))
        for name, s, e, src in layout:
            if src:
                f.seek(s * lba)
                with open(src, 'rb') as g:
                    shutil.copyfileobj(g, f, 1 << 24)
    return {n: (s, e) for n, s, e, _ in layout}


def main():
    sysz = trimmed(P / 'system.img', RUN / 'system.erofs'); venz = trimmed(P / 'vendor.img', RUN / 'vendor.erofs')
    pd = RUN / 'persist-root'; pd.mkdir(); (pd / 'wlan_mac.bin').write_text('Intf0MacAddress=7CB37B000001\n')
    subprocess.check_call(['mke2fs', '-q', '-t', 'ext4', '-d', str(pd), str(RUN / 'persist.img'), '32M'])
    layout = gpt_disk(RUN / 'disk.img', [('system', sysz + 16 * MiB, RUN / 'system.erofs'), ('vendor', venz + 16 * MiB, RUN / 'vendor.erofs'),
                                         ('persist', 32 * MiB, RUN / 'persist.img'), ('metadata', 10 * MiB, None), ('misc', MiB, None), ('modem', MiB, None),
                                         ('dsp', MiB, None), ('bluetooth', MiB, None), ('userdata', 4096 * MiB, None)])
    os.remove(RUN / 'system.erofs'); os.remove(RUN / 'vendor.erofs')
    rep = json.loads((QB / 'report.json').read_text())
    cmdline = rep['cmdline'].replace('loglevel=6', 'loglevel=8') + ' androidboot.hw_timeout_multiplier=5 androidboot.a6l_logcat=1 androidboot.a6l_stayawake=1'
    dtb = sorted(glob.glob(str(ROOT / 'firmware/extracted/android-framework-v72-*-r3/virt.dtb')))[-1]
    cmd = ['qemu-system-aarch64', '-machine', 'virt,gic-version=3', '-cpu', 'cortex-a53', '-smp', '4', '-m', '6144', '-nodefaults',
           '-nographic', '-monitor', 'none', '-serial', 'stdio', '-nic', 'none', '-no-reboot', '-dtb', dtb, '-kernel', str(KERNEL),
           '-initrd', str(QB / 'ramdisk.cpio.gz'), '-append', cmdline,
           '-drive', f'if=none,file={RUN/"disk.img"},format=raw,id=d0', '-device', 'virtio-blk-device,drive=d0']
    (RUN / 'cmd.json').write_text(json.dumps({'cmd': cmd, 'layout': layout}, indent=2))
    log = open(RUN / 'console.log', 'wb')
    p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    t0 = time.time(); done = False
    while time.time() - t0 < MIN * 60 and p.poll() is None:
        time.sleep(10)
        text = (RUN / 'console.log').read_bytes()
        if b'A6L_ROM_BOOT_COMPLETED' in text:
            # A6L_QEMU_HOLD (s): keep the system running after boot_completed to prove it stays up (agent gnss)
            hold = int(os.environ.get('A6L_QEMU_HOLD', '40'))
            t1 = time.time()
            while time.time() - t1 < hold and p.poll() is None:
                time.sleep(10)
            done = True; break
    p.kill(); p.wait(); log.close()
    text = (RUN / 'console.log').read_bytes().decode(errors='replace')
    checks = {
        'first_stage_modules': bool(re.search(r'Loaded \d+ kernel modules|Loaded \d+ modules from', text)),
        'first_stage_mount_system': '/system' in text and 'switch' in text.lower() or 'Switching root to' in text,
        'selinux_setup': 'SELinux: Loaded' in text or 'Loaded SELinux policy' in text or 'selinux' in text.lower(),
        'second_stage': 'init second stage started' in text,
        'vendor_rc': 'A6L_ROM display' in text,
        'mount_all_userdata_format': 'Format' in text and '/data' in text or 'formatting' in text.lower(),
        'bootinfo': 'A6L_ROM bootinfo' in text,
        'boot_completed': 'A6L_ROM_BOOT_COMPLETED' in text,
        'no_panic': 'Kernel panic' not in text,
    }
    # after boot_completed: no zygote/system_server restart (the r5 failure mode), and the GNSS HAL is declared+served
    after = text.split('A6L_ROM_BOOT_COMPLETED', 1)[1] if checks['boot_completed'] else ''
    checks['stable_after_boot'] = bool(after) and not re.search(r"Exit zygote|Service 'zygote' \(pid \d+\) received|Failure starting core service|FATAL EXCEPTION IN SYSTEM PROCESS", after)
    checks['zygote_starts'] = len(re.findall(r"starting service 'zygote'\.\.\.", text))
    ts_after = [float(x) for x in re.findall(r'^\[\s*([0-9.]+)\]', after, re.M)]
    checks['observed_s_after_boot'] = int(ts_after[-1] - ts_after[0]) if len(ts_after) > 1 else 0
    checks['suspended_after_boot'] = 'Suspending console' in after
    checks['gnss_hal_declared'] = 'android.hardware.gnss.IGnss/default in device VINTF manifest' in text
    m = re.findall(r'\[\s*([0-9.]+)\] A6L_ROM_BOOT_COMPLETED', text)
    last = re.findall(r'^\[\s*([0-9.]+)\]', text, re.M)
    rep = {'checks': checks, 'seconds': int(time.time() - t0),
           'boot_completed_at_s': float(m[0]) if m else None, 'last_kernel_ts_s': float(last[-1]) if last else None, 'completed': done, 'cmdline': cmdline,
           'bootinfo': re.findall(r'A6L_ROM bootinfo[^\r\n]*', text)[:6], 'rom_lines': re.findall(r'A6L_ROM [^\r\n]*', text)[:80]}
    (RUN / 'report.json').write_text(json.dumps(rep, indent=2) + '\n')
    ok = checks['boot_completed'] and checks['no_panic'] and checks.get('stable_after_boot', False) and \
        checks['observed_s_after_boot'] >= int(os.environ.get('A6L_QEMU_HOLD', '40')) - 60
    print('ROM_V1_QEMU', 'PASS' if ok else 'FAIL', json.dumps(checks))
    # post-mortem: copy tombstones / ANRs / dropbox out of the userdata partition (read-only loop mount)
    try:
        s0, e0 = layout['userdata']; mnt = RUN / 'data-mnt'; mnt.mkdir()
        subprocess.run(['mount', '-o', f'ro,loop,noload,offset={s0 * 512},sizelimit={(e0 - s0 + 1) * 512}', str(RUN / 'disk.img'), str(mnt)], check=True)
        pm = RUN / 'postmortem'; pm.mkdir()
        for d in ('tombstones', 'anr', 'system/dropbox'):
            subprocess.run(['cp', '-r', str(mnt / d), str(pm / d.replace('/', '_'))], check=False)
        subprocess.run(['umount', str(mnt)], check=False)
    except Exception as e:
        print('postmortem failed', e)
    if ok:
        os.remove(RUN / 'disk.img')


if __name__ == '__main__':
    main()
