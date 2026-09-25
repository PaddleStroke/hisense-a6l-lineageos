#!/usr/bin/env python3
"""rom-v1 boot.img + dtbo.img against the CAPTURED ABL routines (agent flash). Offline; run with /home/a6l/venv-abl/bin/python.
Same routines as Test-RecoveryV74.py (header check, ufdt overlay merge, DT fixup, gzip decompress, DTB selection for the
three PMIC variants, AVB) but for the NORMAL boot path: the board overlay comes from the dtbo PARTITION image and AVB is
asked for 'boot' + 'dtbo'. usage: Test-RomV1Abl.py <boot dir from Prepare-RomV1Boot.py>
Limits: synthetic UEFI memory/PMIC fixtures; not whole BootLinux, not the cmdline builder, not physical execution.
"""
import hashlib, importlib.util, json, struct, sys, zlib
from pathlib import Path
ROOT = Path('/mnt/c/Users/Pierre/Desktop/A6L')
sys.path.insert(0, str(ROOT / 'tools'))
from a6l_fdt import read_fdt
OUT = Path(sys.argv[1])
sha = lambda b: hashlib.sha256(b).hexdigest()


def module(name, patch=None):
    src = (ROOT / f'tools/{name}.py').read_text()
    if patch:
        for a, b in patch:
            assert src.count(a) == 1, (name, a); src = src.replace(a, b)
    spec = importlib.util.spec_from_loader(name, loader=None); m = importlib.util.module_from_spec(spec)
    m.__file__ = str(ROOT / f'tools/{name}.py'); exec(compile(src, m.__file__, 'exec'), m.__dict__); return m


def main():
    rep = {'passed': False, 'scope': 'captured ABL routines, normal boot path (boot + dtbo partitions); no phone'}
    try:
        boot = (OUT / 'boot.img').read_bytes(); dtbo_img = (OUT / 'dtbo.img').read_bytes()
        pk = json.loads((OUT / 'report.json').read_text()); assert sha(boot) == pk['boot_sha256'] and not pk['qemu_variant']
        body = boot[:pk['boot_body_bytes']]
        header = module('Test-CapturedAblHeader').check(boot[:4096]); rep['header'] = header
        assert header['status'] == '0x0' and header['image_size'] <= len(body), header
        base = (OUT / 'rom-v1.dtb').read_bytes()
        table = dtbo_img[:struct.unpack('>I', dtbo_img[4:8])[0]]          # dt_table_header.total_size
        assert table[:4] == bytes.fromhex('d7b7ab1e'), 'not a DTBO table'
        size, offset = struct.unpack_from('>II', table, 32)
        overlay = table[offset:offset + size]
        rep['dtbo_table_sha256'] = sha(table)
        result, merged = module('Test-CapturedAblOverlay').overlay(base, overlay); rep['overlay'] = result
        expected = read_fdt(base); expected['/']['qcom,board-id'] = read_fdt(overlay)['/']['qcom,board-id']
        expected['/chosen']['hisense,a6l-recovery-probe'] = b'v1\0'
        assert merged and not result.get('error') and read_fdt(merged) == expected, 'overlay merge differs'
        assert read_fdt(merged)['/chosen']['hisense,a6l-image'] == b'rom-v1\0'
        (OUT / 'merged-captured-abl.dtb').write_bytes(merged)
        fix = module('Test-CapturedAblDtbFixup').fixup(merged); rep['fixups'] = fix
        assert fix.get('returned') and fix.get('status') == '0x0' and not fix.get('error'), fix
        payload = (OUT / 'Image.gz-dtb').read_bytes()
        dec = zlib.decompressobj(31); kernel = dec.decompress(payload); off = len(payload) - len(dec.unused_data)
        assert dec.eof and dec.unused_data == base and sha(kernel) == pk['kernel_sha256']
        d = module('Test-CapturedAblDecompress').decompress(payload); rep['decompression'] = d
        assert d.get('returned') and d.get('status') == '0x0' and d['sha256'] == sha(kernel) and d['gzip_end_offset'] == off
        select = module('Test-CapturedAblDtb').select
        sel = []
        for second in [16843034, 33620250, 16908314]:
            pm = [65563, second, 0, 0]
            soc, board = select(payload, off, pm), select(table, 0, pm, board=True)
            assert soc.get('selected') and soc['selected_sha256'] == sha(base), soc
            assert board.get('selected') and board['selected_sha256'] == sha(overlay), board
            sel.append({'soc': soc, 'board': board})
        rep['selectors'] = sel
        vbmeta = (ROOT / 'captures/capture-diagnostic-restore-v5/edl/vbmeta-after-write.bin').read_bytes()
        assert sha(vbmeta) == 'e48632a03179e68b3c059905f613d321b3b37aa789ba72a02b322dffde4f7350'
        avbm = module('Test-CapturedAblAvbV26', [(
            "    cpu.mem_write(0x3005000, b'recovery\\0')\n    cpu.mem_write(requested, struct.pack('<QQ', 0x3005000, 0))",
            "    cpu.mem_write(0x3005000, b'boot\\0'); cpu.mem_write(0x3005010, b'dtbo\\0')\n"
            "    cpu.mem_write(requested, struct.pack('<QQQ', 0x3005000, 0x3005010, 0))")])
        avb = avbm.verify({'vbmeta': vbmeta, 'boot': boot, 'dtbo': dtbo_img}); rep['avb'] = avb
        assert avb.get('returned') and avb.get('result') == 5 and not avb.get('error'), avb
        names = sorted(x['name'] for x in avb.get('loaded_partitions', []))
        rep['avb_loaded'] = names
        rep['avb_interpretation'] = 'Result 5 (verification error, allowed when unlocked: captured continue mask 0x39) as for V74; vbmeta flags=2.'
        rep['passed'] = True
    except Exception as e:
        rep['error'] = repr(e); raise
    finally:
        (OUT / 'captured-abl-validation.json').write_text(json.dumps(rep, indent=2, default=str) + '\n')
        print(json.dumps({'passed': rep['passed'], 'error': rep.get('error'), 'avb_loaded': rep.get('avb_loaded')}))


if __name__ == '__main__':
    main()
