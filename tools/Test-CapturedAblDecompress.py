#!/usr/bin/env python3
"""Run the captured ABL gzip decompressor and compare exact output, offline."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import time
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


def decompress(payload, capacity=0x3200000 - 0x80000):
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    cpu.mem_map(0, (len(mapped) + 4095) & ~4095)
    cpu.mem_write(0, bytes(mapped))
    cpu.mem_map(0x2000000, 0x20000)
    cpu.mem_map(0x3000000, 0x20000)
    cpu.mem_map(0x4000000, 0x8000000)
    cpu.mem_map(0x18000000, 0x10000)
    cpu.mem_write(0x4000000, payload)
    cpu.mem_write(0x2000000, struct.pack('<Q', 0x201fff0))
    cpu.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
    cpu.reg_write(UC_ARM64_REG_SP, 0x301fff0)
    cpu.reg_write(UC_ARM64_REG_LR, 0x1800fff0)
    regs = [UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
            UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5]
    for r, value in zip(regs, [0x4000000, len(payload), 0x8000000, capacity, 0x3001000, 0x3001004]):
        cpu.reg_write(r, value)
    heap = 0x6000000

    def reg(i):
        return cpu.reg_read(regs[i])

    def ret(value=0):
        cpu.reg_write(UC_ARM64_REG_X0, value)
        cpu.reg_write(UC_ARM64_REG_PC, cpu.reg_read(UC_ARM64_REG_LR))

    def hook(uc, address, size, context):
        nonlocal heap
        if address == 0x1474:
            ret(0x2000000)
        elif address == 0x1da8:  # AllocateZeroPool
            count = reg(0)
            assert 0 < count < 0x1000000
            value = heap
            heap += (count + 15) & ~15
            assert heap < 0x7000000
            ret(value)
        elif address in (0x1e9c, 0x29f8):  # FreePool, DebugPrintEnabled
            ret()
        elif address == 0x2008:
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
        else:
            raise RuntimeError('Captured-code assertion: ' + hex(address))

    # Restrict Python callbacks to fixture entry points; zlib instructions run natively in Unicorn.
    for address in [0x1474, 0x1da8, 0x1e9c, 0x29f8, 0x2008, 0x35b74, 0x10e40, 0x28cc]:
        cpu.hook_add(UC_HOOK_CODE, hook, begin=address, end=address)
    result = {}
    try:
        started = time.monotonic()
        cpu.emu_start(0x146d0, 0x1800fff0, timeout=55000000)
        result.update(returned=cpu.reg_read(UC_ARM64_REG_PC) == 0x1800fff0,
                      status=hex(reg(0)), stopped_pc=hex(cpu.reg_read(UC_ARM64_REG_PC)),
                      elapsed_seconds=round(time.monotonic() - started, 3))
        offset, size = struct.unpack('<II', cpu.mem_read(0x3001000, 8))
        result.update(gzip_end_offset=offset, decompressed_bytes=size)
        if result['returned'] and reg(0) == 0:
            assert 0 < size <= capacity
            result['sha256'] = hashlib.sha256(cpu.mem_read(0x8000000, size)).hexdigest()
    except Exception as error:
        result.update(error=str(error), pc=hex(cpu.reg_read(UC_ARM64_REG_PC)))
    return result


def main():
    results = {}
    directory = ROOT / 'firmware/extracted/recovery-probe-20260914'
    for filename in ['restore-stock-recovery.img', 'recovery-diagnostic-unsigned.img']:
        image = (directory / filename).read_bytes()
        size = struct.unpack_from('<I', image, 8)[0]
        payload = image[4096:4096 + size]
        dec = zlib.decompressobj(31)
        expanded = dec.decompress(payload)
        assert dec.eof
        expected = {'sha256': hashlib.sha256(expanded).hexdigest(),
                    'gzip_end_offset': len(payload) - len(dec.unused_data),
                    'decompressed_bytes': len(expanded)}
        result = decompress(payload)
        result['matches_host_zlib'] = all(result.get(k) == v for k, v in expected.items())
        results[filename] = result
    controls = {'insufficient_output': decompress(payload, len(payload)),
                'truncated_deflate': decompress(payload[:128])}
    report = {'scope': 'Captured ABL decompressor and zlib execute unchanged in Unicorn, offline',
              'function_rva': '0x146d0', 'images': results, 'negative_controls': controls,
              'fixtures': 'SafeStack, zeroed bounded arena allocation, free, CopyMem, SetMem and disabled debug logging',
              'limits': 'No UEFI memory layout, hardware handoff or kernel execution emulated.'}
    report['passed'] = (all(r.get('returned') and r.get('status') == '0x0' and r['matches_host_zlib']
                            for r in results.values()) and
                        all(r.get('returned') and r.get('status') != '0x0' and 'error' not in r
                            for r in controls.values()))
    output = ROOT / 'firmware/extracted/diagnostic-boot-v1-20260915/decompress-emulation.json'
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    assert report['passed'], 'Inspect decompression fixture report'


if __name__ == '__main__':
    main()
