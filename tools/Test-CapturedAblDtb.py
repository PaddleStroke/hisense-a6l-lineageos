#!/usr/bin/env python3
"""Execute captured A6L DT selection code against local images, without USB.

Board globals model the measured SoC 317 revision 1.0 and QRD 18.0. PMIC
fixtures enumerate the three combinations supported by the stock overlay;
they are not measurements of the PMIC's live revision.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import zlib

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('exit_helper', ROOT / 'tools/Inspect-RecoveryExit.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
abl = helper.pinned(ROOT / 'firmware/extracted/stock-abl-20260914/LinuxLoader.efi',
                    '6bbe53f345729c77c9a0ba74aa7f7aadb6cd68b0ac1511f6f160bc69de6ab523')
_, mapped, _ = helper.map_pe(abl)


def select(payload, offset, pmics, board=False, board_type=11):
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    cpu.mem_map(0, (len(mapped) + 4095) & ~4095)
    cpu.mem_write(0, bytes(mapped))
    cpu.mem_map(0x2000000, 0x20000)
    cpu.mem_map(0x3000000, 0x20000)
    cpu.mem_map(0x4000000, 0x8000000)
    cpu.mem_map(0x18000000, 0x10000)
    cpu.mem_write(0x4000000, payload)
    cpu.mem_write(0x2000000, struct.pack('<Q', 0x201fff0))
    cpu.mem_write(0x56300, struct.pack('<Q', 0x3000000))  # gBS
    cpu.mem_write(0x3000160, struct.pack('<Q', 0x18000000))  # gBS->CopyMem
    # BoardInfo: type, version, subtype, (unused), soc, chip name, revision, foundry.
    for relative, value in [(0, board_type), (4, 0x120000), (8, 0), (0x10, 317),
                            (0x24, 0x10000), (0x28, 0)]:
        cpu.mem_write(0x56358 + relative, struct.pack('<I', value))
    cpu.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
    cpu.reg_write(UC_ARM64_REG_SP, 0x301fff0)
    cpu.reg_write(UC_ARM64_REG_LR, 0x1800fff0)
    args = [0, 0x4000000] if board else [0x4000000, len(payload), offset, 0x8000000]
    regs = [UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3]
    for r, value in zip(regs, args):
        cpu.reg_write(r, value)
    trace, queries = [], []

    def reg(i):
        return cpu.reg_read(regs[i])

    def ret(value=0):
        cpu.reg_write(UC_ARM64_REG_X0, value)
        cpu.reg_write(UC_ARM64_REG_PC, cpu.reg_read(UC_ARM64_REG_LR))

    def hook(uc, address, size, context):
        trace.append(hex(address))
        if len(trace) > 30:
            trace.pop(0)
        if address == 0x1474:
            ret(0x2000000)
        elif address in (0x2008, 0x18000000):
            dst, src, count = reg(0), reg(1), reg(2)
            assert count < 0x4000000
            if count:
                cpu.mem_write(dst, bytes(cpu.mem_read(src, count)))
            ret(dst)
        elif address == 0x35b74:
            dst, count, value = reg(0), reg(1), reg(2) & 255
            assert count < 0x4000000
            if count:
                cpu.mem_write(dst, bytes([value]) * count)
            ret(dst)
        elif address in (0x11f48, 0x12160):  # BoardPmicModel / BoardPmicTarget
            index = reg(0)
            assert index < 4
            value = pmics[index] & 255 if address == 0x11f48 else pmics[index]
            queries.append({'getter': hex(address), 'index': index, 'value': value})
            ret(value)
        elif address == 0x29f8:
            ret()
        elif address in (0x10e40, 0x28cc):
            raise RuntimeError('Captured-code assertion: ' + hex(address))

    cpu.hook_add(UC_HOOK_CODE, hook)
    result = {'pmic_fixture': [hex(v) for v in pmics], 'pmic_queries': queries}
    try:
        cpu.emu_start(0x17818 if board else 0x16618, 0x1800fff0,
                      timeout=5000000, count=1000000)
        result['returned'] = cpu.reg_read(UC_ARM64_REG_PC) == 0x1800fff0
        pointer = reg(0)
        result['selected'] = bool(pointer)
        result['pointer'] = hex(pointer)
        result['overlay_not_needed'] = bool(cpu.mem_read(0x56390, 1)[0])
        if pointer:
            size = struct.unpack('>I', cpu.mem_read(pointer + 4, 4))[0]
            assert 40 <= size < 0x4000000
            result['selected_offset'] = pointer - 0x4000000
            result['selected_bytes'] = size
            result['selected_sha256'] = hashlib.sha256(cpu.mem_read(pointer, size)).hexdigest()
    except Exception as error:
        result.update(error=str(error), pc=hex(cpu.reg_read(UC_ARM64_REG_PC)), trace=trace)
    return result


def main():
    directory = ROOT / 'firmware/extracted/recovery-probe-20260914'
    results = {}
    for filename in ['restore-stock-recovery.img', 'recovery-diagnostic-unsigned.img']:
        image = (directory / filename).read_bytes()
        size = struct.unpack_from('<I', image, 8)[0]
        payload = image[4096:4096 + size]
        dec = zlib.decompressobj(31)
        dec.decompress(payload)
        assert dec.eof
        offset = len(payload) - len(dec.unused_data)
        dtbo_size, dtbo_offset = struct.unpack_from('<IQ', image, 1632)
        table = image[dtbo_offset:dtbo_offset + dtbo_size]
        rows = []
        for second_pmic in [16843034, 33620250, 16908314]:
            pmics = [65563, second_pmic, 0, 0]
            rows.append({'soc': select(payload, offset, pmics),
                         'board': select(table, 0, pmics, board=True)})
        results[filename] = {'image_sha256': hashlib.sha256(image).hexdigest(), 'fixtures': rows}
    pmics = [65563, 16843034, 0, 0]
    controls = {'zero_offset': select(payload, 0, pmics),
                'bad_dtb_header': select(payload, offset + 1, pmics),
                'wrong_board_type': select(table, 0, pmics, board=True, board_type=12)}
    # This selector's primary BOARD_MATCH requirement still admits an overlay
    # with mismatched PMIC models. Record that behavior instead of assuming rejection.
    pmic_mismatch = select(table, 0, [255, 255, 0, 0], board=True)
    report = {'scope': 'Exact captured ABL SoC/board DT selectors with stated hardware fixtures',
              'functions': {'GetSocDtb': '0x16618', 'GetBoardDtb': '0x17818'},
              'board_fixture': {'soc': 317, 'revision': '1.0', 'type': 11,
                                'version': '18.0', 'subtype': 0, 'foundry': 0},
              'images': results, 'negative_controls': controls,
              'pmic_mismatch_observation': pmic_mismatch,
              'limits': 'Does not test overlay application, DT fixups, decompression, live PMIC reads or kernel execution.'}
    report['passed'] = all(r.get('returned') and r.get('selected') and 'error' not in r
        for v in results.values() for fixture in v['fixtures'] for r in fixture.values())
    report['passed'] &= all(r.get('returned') and not r.get('selected') and 'error' not in r
                            for r in controls.values())
    output = ROOT / 'firmware/extracted/diagnostic-boot-v1-20260915/dtb-emulation.json'
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'positive_cases': 12,
                      'negative_controls': {k: v.get('selected') for k, v in controls.items()},
                      'report': str(output)}))
    assert report['passed'], 'Inspect fixture errors/selection failures in the report'


if __name__ == '__main__':
    main()
