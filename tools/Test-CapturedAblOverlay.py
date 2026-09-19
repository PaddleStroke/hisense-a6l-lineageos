#!/usr/bin/env python3
"""Run the captured ABL's ufdt allocator, blob check and overlay application."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *
from a6l_fdt import read_fdt

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('dt_helper', ROOT / 'tools/Test-CapturedAblDtb.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def overlay(base, addition):
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    cpu.mem_map(0, (len(helper.mapped) + 4095) & ~4095)
    cpu.mem_write(0, bytes(helper.mapped))
    for address, size in [(0x2000000, 0x20000), (0x3000000, 0x20000),
                          (0x4000000, 0x2000000), (0x18000000, 0x10000)]:
        cpu.mem_map(address, size)
    cpu.mem_write(0x4000000, base)
    cpu.mem_write(0x4100000, addition)
    cpu.mem_write(0x2000000, struct.pack('<Q', 0x201fff0))
    cpu.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
    cpu.reg_write(UC_ARM64_REG_SP, 0x301fff0)
    cpu.mem_write(0x56300, struct.pack('<Q', 0x3000000))
    cpu.mem_write(0x3000160, struct.pack('<Q', 0x18000000))
    regs = [UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3]
    heap, logs = 0x4200000, []

    def reg(i):
        return cpu.reg_read(regs[i])

    def ret(value=0):
        cpu.reg_write(UC_ARM64_REG_X0, value)
        cpu.reg_write(UC_ARM64_REG_PC, cpu.reg_read(UC_ARM64_REG_LR))

    def hook(uc, address, size, context):
        nonlocal heap
        if address == 0x1474:
            ret(0x2000000)
        elif address == 0x1da8:
            count = reg(0)
            assert 0 < count <= 0x600000
            p = heap
            heap += (count + 15) & ~15
            assert heap < 0x6000000
            ret(p)
        elif address in (0x29f8, 0x1e9c):
            ret()
        elif address in (0x2008, 0x18000000):
            dst, src, count = reg(0), reg(1), reg(2)
            assert count < 0x1000000
            if count:
                cpu.mem_write(dst, bytes(cpu.mem_read(src, count)))
            ret(dst)
        elif address == 0x35b74:
            dst, count, value = reg(0), reg(1), reg(2) & 255
            assert count < 0x1000000
            if count:
                cpu.mem_write(dst, bytes([value]) * count)
            ret(dst)
        elif address == 0x265b4:
            chars = bytearray()
            for i in range(1024):
                b = cpu.mem_read(reg(0) + i, 1)[0]
                if not b:
                    break
                chars.append(b)
            logs.append(chars.decode(errors='replace'))
            ret()
        else:
            raise RuntimeError('Captured-code assertion: ' + hex(address))

    for a in [0x1474, 0x1da8, 0x1e9c, 0x29f8, 0x2008, 0x18000000,
              0x35b74, 0x265b4, 0x10e40, 0x28cc]:
        cpu.hook_add(UC_HOOK_CODE, hook, begin=a, end=a)

    def call(address, args):
        for r, v in zip(regs, args):
            cpu.reg_write(r, v)
        cpu.reg_write(UC_ARM64_REG_LR, 0x1800fff0)
        cpu.emu_start(address, 0x1800fff0, timeout=10000000)
        assert cpu.reg_read(UC_ARM64_REG_PC) == 0x1800fff0, 'Function did not return'
        return reg(0)

    result = {'base_sha256': hashlib.sha256(base).hexdigest(),
              'overlay_sha256': hashlib.sha256(addition).hexdigest(), 'logs': logs}
    try:
        assert call(0x26b5c, []) != 0, 'Arena allocation failed'
        installed = call(0x25904, [0x4000000, len(base)])
        assert installed == 0x4000000, 'Blob validation failed'
        p = call(0x259f0, [installed, len(base), 0x4100000, len(addition)])
        result['returned_blob'] = bool(p)
        if p:
            size = struct.unpack('>I', cpu.mem_read(p + 4, 4))[0]
            assert 40 <= size <= len(base) + len(addition)
            data = bytes(cpu.mem_read(p, size))
            read_fdt(data)
            result.update(output_bytes=size, output_sha256=hashlib.sha256(data).hexdigest())
            return result, data
    except Exception as error:
        result.update(error=str(error), pc=hex(cpu.reg_read(UC_ARM64_REG_PC)))
    return result, None


def main():
    directory = ROOT / 'firmware/extracted/recovery-probe-20260914'
    stock = (directory / 'restore-stock-recovery.img').read_bytes()
    table_size, table_offset = struct.unpack_from('<IQ', stock, 1632)
    table = stock[table_offset:table_offset + table_size]
    size, offset = struct.unpack_from('>II', table, 32)
    pairs = {'stock': (ROOT / 'firmware/extracted/device-trees/stock-00.dtb', table[offset:offset + size],
                        ROOT / 'firmware/extracted/stock-dtbo-20260914/stock-00-merged.dtb'),
             'diagnostic': (directory / 'base.dtb', (directory / 'overlay.dtbo').read_bytes(),
                            directory / 'merged-libufdt.dtb')}
    results = {}
    for name, (path, addition, expected) in pairs.items():
        result, data = overlay(path.read_bytes(), addition)
        result['properties_match_prior_reference_merge'] = bool(data) and read_fdt(data) == read_fdt(expected.read_bytes())
        if data:
            (ROOT / f'firmware/extracted/diagnostic-boot-v1-20260915/{name}-captured-abl-merged.dtb').write_bytes(data)
        results[name] = result
    report = {'scope': 'Exact captured ABL ufdt allocator and overlay application, offline',
              'images': results, 'limits': 'Does not emulate image-selection glue, UEFI hardware or kernel entry.'}
    report['passed'] = all(r.get('returned_blob') and r.get('properties_match_prior_reference_merge') and not r.get('error') for r in results.values())
    (ROOT / 'firmware/extracted/diagnostic-boot-v1-20260915/overlay-emulation.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    assert report['passed'], 'Inspect captured ufdt result'


if __name__ == '__main__':
    main()
