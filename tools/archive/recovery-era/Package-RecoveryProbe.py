#!/usr/bin/env python3
"""Build and audit an unsigned recovery diagnostic candidate entirely offline."""
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zlib
from a6l_fdt import read_fdt, cells, strings

root = Path(__file__).resolve().parent.parent
source = Path('/home/a6l/android/a6l-lineage24')
kernel_out = Path('/home/a6l/kernel/out-a6l-probe')
validator = Path('/home/a6l/tools/a6l-overlay-validator')
output = root / 'firmware/extracted/recovery-probe-20260914'
backup = root / 'firmware/raw-backup-20260914'
spec = importlib.util.spec_from_file_location('boot_roundtrip', root / 'tools/Verify-BootRoundtrip.py')
boot_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot_module)
layout = boot_module.layout


def sha(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def changed_properties(before, after):
    require(set(before) == set(after), 'Unexpected DT node addition/removal')
    return {(path, key) for path in before for key in set(before[path]) | set(after[path])
            if before[path].get(key) != after[path].get(key)}


def run(command):
    result = subprocess.run([str(value) for value in command], capture_output=True, check=True)
    return result.stdout


output.mkdir(exist_ok=False)
report = {'scope': 'Offline unsigned recovery candidate; no phone access or flashing',
          'checks': {}, 'commands_sent_to_phone': [], 'ready_to_flash': False}
try:
    manifest = json.loads((backup / 'firmware-verification.json').read_text())
    partition = next(p for p in manifest['partitions'] if p['name'] == 'recovery')
    with (backup / 'emmc-firmware-prefix.bin').open('rb') as stream:
        stream.seek(partition['offset'])
        stock = stream.read(partition['bytes'])
    require(sha(stock) == partition['sha256'], 'Stock recovery backup hash mismatch')
    stock_layout = layout(stock)
    require(stock_layout['header_version'] == 1 and stock_layout['page_size'] == 4096,
            'Unexpected stock recovery header')
    report['stock_recovery'] = partition
    # Preserve the complete captured partition, including its existing tail.
    (output / 'restore-stock-recovery.img').write_bytes(stock)
    section = stock_layout['sections']['recovery_dtbo']
    stock_table = stock[section['offset']:section['offset'] + section['bytes']]
    require(sha(stock_table) == '5b04607b7d71c6e0bbe87f5203cfd51c5988ad1b47b19710ec7caaec300913ea',
            'Stock embedded DTBO mismatch')
    require(struct.unpack_from('>8I', stock_table) == (0xd7b7ab1e, 821, 32, 32, 1, 32, 4096, 0),
            'Unexpected stock DTBO table format')
    overlay_size, overlay_offset = struct.unpack_from('>2I', stock_table, 32)
    stock_overlay = read_fdt(stock_table[overlay_offset:overlay_offset + overlay_size])
    base_dir = kernel_out / 'arch/arm64/boot/dts/qcom'
    dtb = (base_dir / 'sdm660-hisense-a6l-recovery.dtb').read_bytes()
    overlay = (base_dir / 'a6l-recovery-overlay.dtbo').read_bytes()
    nodes, overlay_nodes = read_fdt(dtb), read_fdt(overlay)
    original_dtb = root / 'firmware/extracted/kernel-probe-20260914/sdm660-hisense-a6l-probe.dtb'
    require(sha(original_dtb.read_bytes()) == '1228d8bc1b7ae2ee477ab0f6d25fce133f440156ce33fad8af10f53a614c4a2c',
            'Previously audited board tree differs')
    changes = changed_properties(read_fdt(original_dtb.read_bytes()), nodes)
    require(changes == {('/', 'qcom,msm-id'), ('/', 'qcom,board-id'), ('/chosen', 'bootargs')},
            f'Recovery wrapper unexpectedly changes hardware: {changes}')
    require(cells(nodes['/']['qcom,msm-id']) == [317, 0] and
            cells(nodes['/']['qcom,board-id']) == [0, 0], 'SoC selection mismatch')
    for name in ['model', 'compatible', 'qcom,board-id', 'qcom,pmic-id']:
        require(overlay_nodes['/'][name] == stock_overlay['/'][name], f'Overlay match metadata differs: {name}')
    require(set(overlay_nodes) == {'/', '/fragment@0', '/fragment@0/__overlay__'},
            'Unexpected overlay nodes or symbol fixups')
    require(overlay_nodes['/fragment@0'] == {'target-path': b'/chosen\0'} and
            overlay_nodes['/fragment@0/__overlay__'] == {'hisense,a6l-recovery-probe': b'v1\0'},
            'Overlay must only add the diagnostic marker')
    (output / 'base.dtb').write_bytes(dtb)
    (output / 'overlay.dtbo').write_bytes(overlay)
    run(['fdtoverlay', '-i', output / 'base.dtb', '-o', output / 'merged-libfdt.dtb', output / 'overlay.dtbo'])
    run([validator / 'ufdt_apply_overlay', output / 'base.dtb', output / 'overlay.dtbo', output / 'merged-libufdt.dtb'])
    merged = read_fdt((output / 'merged-libufdt.dtb').read_bytes())
    require(merged == read_fdt((output / 'merged-libfdt.dtb').read_bytes()), 'Overlay implementations disagree')
    require(changed_properties(nodes, merged) == {('/chosen', 'hisense,a6l-recovery-probe')},
            'Overlay changes hardware or command-line configuration')
    require(run(['fdtget', '-t', 's', output / 'merged-libufdt.dtb', '/memory', 'device_type']).strip() == b'memory',
            'Bootloader memory-node lookup fails')
    report['checks']['dtb_preserves_audited_hardware'] = True
    report['checks']['selection_metadata_matches_stock'] = True
    report['checks']['libfdt_and_libufdt_agree'] = True
    report['checks']['overlay_changes_only_diagnostic_marker'] = True
    report['checks']['memory_lookup_works'] = True
    report['overlay_validator_revisions'] = {name: (validator / name).read_text().strip()
                                             for name in ['libufdt-revision.txt', 'libfdt-revision.txt']}

    # Match the stock v0 table's structure, but embed our own recovery overlay.
    table = struct.pack('>8I', 0xd7b7ab1e, 64 + len(overlay), 32, 32, 1, 32, 4096, 0)
    table += struct.pack('>8I', len(overlay), 64, 0, 0, 0, 0, 0, 0) + overlay
    table_path = output / 'recovery-dtbo.img'
    table_path.write_bytes(table)

    image = (root / 'firmware/extracted/abl-offset-test-20260914/Image.a6l-offset').read_bytes()
    require(sha(image) == 'bce9e6d3b330bb4a77cbd30b5bff71f1c8b5ba94f71417b8e0a495425079fb43',
            'Load-offset-tested kernel differs')
    offset, runtime_size = struct.unpack_from('<QQ', image, 8)
    require(offset == 0x80000 and offset + runtime_size < 0x3200000, 'Kernel exceeds stock DTB destination')
    compressed = gzip.compress(image, mtime=0)
    payload = compressed + dtb
    dec = zlib.decompressobj(16 + zlib.MAX_WBITS)
    require(dec.decompress(payload) == image and dec.eof and dec.unused_data == dtb,
            'Appended DTB does not start immediately after the gzip member')
    kernel_path = output / 'Image.gz-dtb'
    kernel_path.write_bytes(payload)
    ramdisk = root / 'firmware/extracted/diagnostic-ramdisk-20260914/ramdisk.cpio.gz'
    require(sha(ramdisk.read_bytes()) == '14141431a5e57a4510159365eddddc5322aceb271272787a5282ee27f2a9d983',
            'Tested RAM diagnostic archive differs')
    shutil.copyfile(ramdisk, output / 'ramdisk.cpio.gz')
    ramdisk = output / 'ramdisk.cpio.gz'
    arguments = json.loads((root / 'firmware/extracted/boot-roundtrip-20260914-v4/recovery/mkbootimg-arguments.json').read_text())
    command_line = ('console=ttyMSM0,115200n8 earlycon=msm_serial_dm,0xc170000 '
                    'androidboot.hardware=qcom loglevel=8 clk_ignore_unused '
                    'pd_ignore_unused regulator_ignore_unused panic=15 a6l_probe=1')
    for key, value in {'--kernel': kernel_path, '--ramdisk': ramdisk,
                       '--recovery_dtbo': table_path, '--cmdline': command_line}.items():
        arguments[arguments.index(key) + 1] = str(value)
    pack = source / 'system/tools/mkbootimg/mkbootimg.py'
    unpack = source / 'system/tools/mkbootimg/unpack_bootimg.py'
    body_path = output / 'recovery-diagnostic-body.img'
    run([sys.executable, pack, *arguments, '--output', body_path])
    body = bytearray(body_path.read_bytes())
    body[28:32] = stock[28:32]  # Preserve stock's unused second_addr field.
    body_path.write_bytes(body)
    new_layout = layout(body)
    require(new_layout['body_end'] == len(body) < partition['bytes'], 'Image exceeds recovery partition')
    require(new_layout['sections']['kernel']['sha256'] == sha(payload) and
            new_layout['sections']['ramdisk']['sha256'] == sha(ramdisk.read_bytes()) and
            new_layout['sections']['recovery_dtbo']['sha256'] == sha(table), 'Packaged payload mismatch')
    rebuilt_args = run([sys.executable, unpack, '--boot_img', body_path, '--out', output / 'unpacked',
                        '--format', 'mkbootimg', '-0']).split(b'\0')
    if rebuilt_args[-1] == b'':
        rebuilt_args.pop()
    rebuilt = output / 'roundtrip.img'
    run([sys.executable, pack, *[arg.decode() for arg in rebuilt_args], '--output', rebuilt])
    roundtrip = bytearray(rebuilt.read_bytes())
    roundtrip[28:32] = body[28:32]
    require(roundtrip == body, 'Recovery candidate does not round-trip exactly')
    # A full-partition raw candidate also clears the old recovery's AVB tail.
    # It is intentionally unsigned; an unlock/verification plan is still needed.
    candidate = bytes(body) + bytes(partition['bytes'] - len(body))
    candidate_path = output / 'recovery-diagnostic-unsigned.img'
    candidate_path.write_bytes(candidate)
    require(candidate[-64:] == bytes(64), 'Unexpected trailing metadata')
    report['checks']['kernel_gzip_and_appended_dtb_valid'] = True
    report['checks']['runtime_kernel_fits_before_dtb'] = True
    report['checks']['recovery_payloads_match'] = True
    report['checks']['recovery_roundtrip_exact'] = True
    report['checks']['candidate_exact_partition_size'] = True
    report['checks']['complete_stock_restore_hash_matches'] = sha((output / 'restore-stock-recovery.img').read_bytes()) == partition['sha256']
    report['layout'] = new_layout
    report['command_line'] = command_line
    report['artifacts'] = [{'file': path.name, 'bytes': path.stat().st_size, 'sha256': sha(path.read_bytes())}
                           for path in sorted(output.iterdir()) if path.is_file()]
    report['all_checks_passed'] = all(report['checks'].values())
    report['remaining_before_flash'] = [
        'Validate physical stock-recovery entry and return',
        'Establish reliable fastboot queries and the concrete unlock/data-wipe procedure',
        'Review verification policy and the exact recovery-only install/restore procedure',
        'No A6L hardware boot is established; PMIC, USB and memory fixups remain untested']
except Exception as error:
    report['error'] = str(error)
    if isinstance(error, subprocess.CalledProcessError):
        report['stderr'] = error.stderr.decode(errors='replace')
    report['all_checks_passed'] = False
finally:
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
raise SystemExit(0 if report['all_checks_passed'] else 1)
