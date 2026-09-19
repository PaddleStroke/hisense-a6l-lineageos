#!/usr/bin/env python3
"""Exercise the actual built console in diskless QEMU, including board guards."""
import hashlib
import json
import os
import re
from pathlib import Path
import socket
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / 'firmware/extracted/staged-usb-probe-v15-20260916'
# Keep actively written QEMU logs on Linux ext4, not the Windows shared mount.
OUT = Path('/home/a6l/kernel/test-staged-usb-probe-v15-20260916')
BUILD = Path('/home/a6l/kernel/out-a6l-probe')
BASE = 0x9d400000
FB_BYTES = 1080 * 2340 * 4


def run(*args):
    return subprocess.run([str(v) for v in args], capture_output=True, check=True).stdout


def test(name, compatible, reserved_bytes):
    p = OUT / name
    p.mkdir(exist_ok=False)
    patch = f'''/dts-v1/;
/plugin/;
/ {{ fragment@0 {{ target-path = "/"; __overlay__ {{
compatible = "{compatible}", "linux,dummy-virt";
reserved-memory {{ #address-cells = <2>; #size-cells = <2>; ranges;
framebuffer@9d400000 {{ reg = <0 0x9d400000 0 {reserved_bytes}>; no-map; }};
}};
}}; }}; }};
'''
    (p / 'fixture.dts').write_text(patch)
    run('dtc', '-@', '-I', 'dts', '-O', 'dtb', '-o', p / 'fixture.dtbo', p / 'fixture.dts')
    run('fdtoverlay', '-i', OUT / 'virt.dtb', '-o', p / 'virt.dtb', p / 'fixture.dtbo')
    sockpath = Path(f'/tmp/a6l-fb-{os.getpid()}-{name}.sock')
    args = ['qemu-system-aarch64', '-machine', 'virt,gic-version=3', '-cpu', 'cortex-a53',
            '-smp', '2', '-m', '2048', '-nodefaults', '-nographic', '-monitor', 'none',
            '-serial', 'stdio', '-nic', 'none', '-no-reboot', '-S',
            '-qmp', f'unix:{sockpath},server=on,wait=off',
            '-dtb', str(p / 'virt.dtb'), '-kernel', str(OUT / 'Image.a6l-offset'),
            '-initrd', str(ROOT / 'firmware/extracted/staged-usb-ramdisk-v15-20260916/ramdisk.cpio.gz'),
            '-append', 'earlycon=a6lfb keep_bootcon console=ttyAMA0,115200 loglevel=8 panic=0 initcall_debug pd_ignore_unused a6l_manual_usb=1']
    result = {'command': args, 'compatible': compatible, 'reserved_bytes': reserved_bytes}
    proc = None
    try:
        with (p / 'console.log').open('wb') as log, (p / 'stderr.log').open('wb') as err:
            proc = subprocess.Popen(args, stdout=log, stderr=err)
            until = time.monotonic() + 5
            while not sockpath.exists() and time.monotonic() < until:
                assert proc.poll() is None
                time.sleep(.05)
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                connection.settimeout(5)
                connection.connect(str(sockpath))
                stream = connection.makefile('rwb', buffering=0)
                json.loads(stream.readline())

                def qmp(cmd, arguments=None):
                    request = {'execute': cmd}
                    if arguments:
                        request['arguments'] = arguments
                    stream.write(json.dumps(request).encode() + b'\n')
                    while True:
                        answer = json.loads(stream.readline())
                        assert 'error' not in answer, answer
                        if 'return' in answer:
                            return answer['return']

                qmp('qmp_capabilities')
                qmp('cont')
                # Verbose initcall text through the emulated framebuffer takes
                # ~32 seconds before PID1; allow its two 10-second heartbeats.
                until = time.monotonic() + 240
                while time.monotonic() < until:
                    data = (p / 'console.log').read_bytes()
                    if (any(int(v) >= 152 for v in re.findall(rb'A6L_RAM_PROBE_ALIVE seconds=([0-9]+) ', data))) or b'Kernel panic' in data:
                        break
                    assert proc.poll() is None, 'Unexpected QEMU exit'
                    time.sleep(.5)
                qmp('stop')
                qmp('pmemsave', {'val': BASE, 'size': FB_BYTES + 4096, 'filename': str(p / 'framebuffer.bin')})
                result['cpu_still_running_before_stop'] = proc.poll() is None
        data = (p / 'console.log').read_bytes()
        frame = (p / 'framebuffer.bin').read_bytes()
        pixels = struct.iter_unpack('<I', frame[:FB_BYTES])
        counts = {}
        for (pixel,) in pixels:
            counts[pixel] = counts.get(pixel, 0) + 1
        active = compatible == 'hisense,hlte730t' and reserved_bytes == 0x23ff000
        milestones = [b'A6L init initcalls done', b'A6L init wait_for_initramfs done', b'A6L init console_on_rootfs begin', b'A6L init console_on_rootfs done', b'A6L init rdinit access result=0', b'A6L init free_initmem done', b'A6L init mark_readonly done', b'A6L init exec /init']
        positions = [data.find(marker) for marker in milestones]
        checks = dict(init_milestones_complete=all(pos >= 0 for pos in positions),
                      init_milestones_ordered=positions == sorted(positions),
                      probe_entry_trace=b'A6L probe begin' in data,
                      pid1_ready=b'A6L_RAM_PROBE_READY' in data,
                      alive=any(int(v) >= 152 for v in re.findall(rb'A6L_RAM_PROBE_ALIVE seconds=([0-9]+) ', data)),
                      staged_schedule=b'A6L_STAGED_USB_V15 config_after=60 bind_after=90 connect_after=120 serial_after=150 heartbeat=2' in data,
                      configuration_without_binding=b'A6L_USB_CONFIG_END success=1; UDC still unbound' in data,
                      no_usb_without_controller=b'A6L_USB_FIND_UDC_END unavailable' in data and b'A6L_USB_BIND_BEGIN' not in data and b'A6L_SERIAL_OPEN_BEGIN' not in data,
                      status_via_kmsg=bool(re.search(rb'\[\s*[0-9.]+\] A6L_RAM_PROBE_START', data)),
                      read_only_debugfs=b'A6L_DEBUGFS read-only mount: success' in data,
                      dependency_snapshots=data.count(b'A6L_DEFERRED_END') >= 2,
                      no_status_feedback=data.count(b'A6L_RAM_PROBE_START') == 1 and data.count(b'A6L_RAM_PROBE_READY') == 1,
                      no_kmsg_rate_drops=b'callbacks suppressed' not in data,
                      kmsg_setting_succeeded=b'write /proc/sys/kernel/printk_devkmsg:' not in data,
                      no_panic=b'Kernel panic' not in data,
                      framebuffer_tail_untouched=not any(frame[FB_BYTES:]),
                      framebuffer_gate_correct=(counts.get(0xffffffff, 0) > 1000 and
                                                counts.get(0xff000000, 0) > 1000 and
                                                set(counts) <= {0, 0xffffffff, 0xff000000}) if active else not any(frame))
        result.update(pixel_counts={hex(k): v for k, v in counts.items()}, checks=checks,
                      passed=all(checks.values()))
        assert result['passed'], result
    except Exception as error:
        result.update(passed=False, error=repr(error))
        raise
    finally:
        if proc and proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        sockpath.unlink(missing_ok=True)
        (p / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(dict(case=name, passed=True)), flush=True)
    return result


def main():
    OUT.mkdir(exist_ok=False)
    image = (BUILD / 'arch/arm64/boot/Image').read_bytes()
    assert image[56:60] == b'ARMd' and struct.unpack_from('<Q', image, 8)[0] == 0
    (OUT / 'Image').write_bytes(image)
    config = (BUILD / '.config').read_bytes()
    (OUT / 'kernel.config').write_bytes(config)
    adapted = bytearray(image)
    struct.pack_into('<Q', adapted, 8, 0x80000)
    assert adapted[:8] == image[:8] and adapted[16:] == image[16:]
    assert 0x80000 + struct.unpack_from('<Q', image, 16)[0] < 0x3200000
    (OUT / 'Image.a6l-offset').write_bytes(adapted)
    run('qemu-system-aarch64', '-machine', f'virt,gic-version=3,dumpdtb={OUT / "virt.dtb"}',
        '-cpu', 'cortex-a53', '-smp', '2', '-m', '2048', '-nodefaults', '-nographic')
    result = {'scope': 'Diskless QEMU test of staged RAM PID1 through 152 seconds; no physical USB emulation',
              'kernel_sha256': hashlib.sha256(adapted).hexdigest(),
              'ramdisk_sha256': hashlib.sha256((ROOT / 'firmware/extracted/staged-usb-ramdisk-v15-20260916/ramdisk.cpio.gz').read_bytes()).hexdigest(), 'cases': {}}
    try:
        for name, board, size in [('enabled', 'hisense,hlte730t', 0x23ff000)]:
            result['cases'][name] = test(name, board, size)
        result['passed'] = True
    finally:
        (OUT / 'report.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    assert not ARCHIVE.exists(), 'Do not overwrite an earlier test'
    try:
        main()
    finally:
        if OUT.exists():
            # Windows shared mounts do not necessarily support Linux chmod or
            # timestamp preservation. Archive content, not Unix metadata.
            ARCHIVE.mkdir(exist_ok=False)
            for source in OUT.rglob('*'):
                target = ARCHIVE / source.relative_to(OUT)
                if source.is_dir():
                    target.mkdir(exist_ok=True)
                elif source.is_file():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    data = source.read_bytes()
                    target.write_bytes(data)
                    assert target.read_bytes() == data
