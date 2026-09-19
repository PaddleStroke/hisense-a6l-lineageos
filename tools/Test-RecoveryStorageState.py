#!/usr/bin/env python3
"""Regression-check the corrected image against captured ABL instructions."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import zlib
from a6l_fdt import read_fdt

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'firmware/extracted/recovery-probe-storage-state-v32-20260917'
OLD = ROOT / 'firmware/extracted/recovery-probe-20260914'


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f'tools/{name}.py')
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    report = {'passed': False, 'scope': 'Offline exact captured ABL routines; no phone access',
              'limits': 'Synthetic UEFI memory/PMIC/rollback fixtures; not whole BootLinux or physical kernel execution.'}
    try:
        candidate = (OUT / 'recovery-diagnostic-unsigned.img').read_bytes()
        package = json.loads((OUT / 'report.json').read_text())
        assert package['packaging_passed'] and sha(candidate) == package['candidate_sha256']
        report['candidate_sha256'] = sha(candidate)
        header = module('Test-CapturedAblHeader').check(candidate[:4096])
        report['header'] = header
        assert header['status'] == '0x0' and header['image_size'] == package['layout']['body_end']
        base, addition = (OUT / 'base.dtb').read_bytes(), (OUT / 'overlay.dtbo').read_bytes()
        assert base == (ROOT / 'firmware/extracted/recovery-probe-storage-nosdio-v27-20260916/base.dtb').read_bytes()
        merge = module('Test-CapturedAblOverlay').overlay
        result, data = merge(base, addition)
        report['overlay'] = result
        expected = read_fdt(base)
        expected['/']['qcom,board-id'] = read_fdt(addition)['/']['qcom,board-id']
        expected['/chosen']['hisense,a6l-recovery-probe'] = b'v1\0'
        assert data and not result.get('error') and read_fdt(data) == expected
        (OUT / 'merged-captured-abl.dtb').write_bytes(data)
        report['overlay']['only_changes'] = ['root qcom,board-id selection metadata', 'chosen diagnostic marker']
        # Original regression and incomplete correction must still fail. This
        # protects against accidentally replacing the old ufdt with a newer one.
        controls = {}
        for name, b in [('missing_main_symbols', (OLD / 'base.dtb').read_bytes()),
                        ('missing_overlay_fixups', base)]:
            r, d = merge(b, (OLD / 'overlay.dtbo').read_bytes())
            assert d is None and not r.get('error') and r.get('returned_blob') is False
            expected_log = 'Bad main_symbols' if name == 'missing_main_symbols' else 'Bad overlay_fixups'
            assert any(expected_log in line for line in r['logs'])
            controls[name] = r
        report['overlay_negative_controls'] = controls
        stock = (OLD / 'restore-stock-recovery.img').read_bytes()
        size, offset = struct.unpack_from('<IQ', stock, 1632)
        table = stock[offset:offset + size]
        size, offset = struct.unpack_from('>II', table, 32)
        stock_overlay = table[offset:offset + size]
        stock_base = (ROOT / 'firmware/extracted/device-trees/stock-00.dtb').read_bytes()
        r, d = merge(stock_base, stock_overlay)
        stock_expected = read_fdt((ROOT / 'firmware/extracted/stock-dtbo-20260914/stock-00-merged.dtb').read_bytes())
        for key in ['qcom,board-id', 'model', 'compatible']:
            stock_expected['/'][key] = read_fdt(stock_overlay)['/'][key]
        assert d and not r.get('error') and read_fdt(d) == stock_expected
        report['stock_overlay_positive_control'] = r
        report['stock_overlay_reference_difference'] = 'Old captured ufdt additionally applies three root properties: qcom,board-id, model, compatible.'
        fix = module('Test-CapturedAblDtbFixup').fixup(data)
        report['fixups'] = fix
        assert fix.get('returned') and fix.get('status') == '0x0' and not fix.get('error')
        assert fix['chosen_bootargs'] == 'rdinit=/init panic=15 loglevel=8\0'
        assert fix['initrd_start'] == [0, 0x84000000] and fix['initrd_end'] == [0, 0x84020000]
        assert list(fix['memory_reg'].values()) == [[0, 0x80000000, 0, 0x80000000, 1, 0, 1, 0]]
        payload = (OUT / 'Image.gz-dtb').read_bytes()
        dec = zlib.decompressobj(31)
        kernel = dec.decompress(payload)
        offset = len(payload) - len(dec.unused_data)
        assert dec.eof and dec.unused_data == base
        assert sha(kernel) == '01e882200d5c99b1199d15697721de50dc0114e3465f62945ead2b68a5ee2c12'
        decompressed = module('Test-CapturedAblDecompress').decompress(payload)
        report['decompression'] = decompressed
        assert decompressed.get('returned') and decompressed.get('status') == '0x0' and not decompressed.get('error')
        assert decompressed['sha256'] == sha(kernel) and decompressed['gzip_end_offset'] == offset
        assert decompressed['decompressed_bytes'] == len(kernel)
        select = module('Test-CapturedAblDtb').select
        table = (OUT / 'recovery-dtbo.img').read_bytes()
        selectors = []
        for second in [16843034, 33620250, 16908314]:
            pmics = [65563, second, 0, 0]
            soc, board = select(payload, offset, pmics), select(table, 0, pmics, board=True)
            for entry, digest in [(soc, sha(base)), (board, sha(addition))]:
                assert entry.get('returned') and entry.get('selected') and not entry.get('error')
                assert entry['selected_sha256'] == digest
            selectors.append(dict(soc=soc, board=board))
        report['selectors'] = selectors
        vbmeta = (ROOT / 'captures/capture-diagnostic-restore-v5/edl/vbmeta-after-write.bin').read_bytes()
        assert sha(vbmeta) == 'e48632a03179e68b3c059905f613d321b3b37aa789ba72a02b322dffde4f7350'
        avb = module('Test-CapturedAblAvbV26').verify({'vbmeta': vbmeta, 'recovery': candidate})
        report['avb'] = avb
        assert avb.get('returned') and avb.get('result') == 5 and not avb.get('error')
        assert avb['loaded_partitions'] == [{'name': 'recovery', 'bytes': len(candidate), 'sha256': sha(candidate)}]
        report['avb_interpretation'] = 'Result 5 is allowed by captured unlocked continue mask 0x39; fresh live vbmeta fixture matches backup.'
        report['passed'] = True
    except Exception as error:
        report['error'] = repr(error)
        raise
    finally:
        (OUT / 'captured-abl-validation.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(dict(passed=report['passed'], error=report.get('error'), report=str(OUT / 'captured-abl-validation.json'))))


if __name__ == '__main__':
    main()
