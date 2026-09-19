#!/usr/bin/env python3
"""Exercise captured ABL device-tree fixups with explicit offline UEFI fixtures."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *
from a6l_fdt import read_fdt, cells

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('dt_helper', ROOT / 'tools/Test-CapturedAblDtb.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def fixup(dtb):
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    cpu.mem_map(0, (len(helper.mapped) + 4095) & ~4095)
    cpu.mem_write(0, bytes(helper.mapped))
    cpu.mem_map(0x2000000, 0x20000)
    cpu.mem_map(0x3000000, 0x20000)
    cpu.mem_map(0x4000000, 0x200000)
    cpu.mem_map(0x18000000, 0x10000)
    cpu.mem_write(0x4000000, dtb)
    cpu.mem_write(0x2000000, struct.pack('<Q', 0x201fff0))
    cpu.mem_write(0x56300, struct.pack('<QQ', 0x3000000, 0x3002000))  # gBS, gRT
    cpu.mem_write(0x3000160, struct.pack('<Q', 0x18000000))  # CopyMem
    cpu.mem_write(0x3000140, struct.pack('<Q', 0x18000010))  # LocateProtocol
    cpu.mem_write(0x3002048, struct.pack('<Q', 0x18000020))  # GetVariable
    cpu.mem_write(0x3008000, b'rdinit=/init panic=15 loglevel=8\0')
    cpu.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
    cpu.reg_write(UC_ARM64_REG_SP, 0x301fff0)
    cpu.reg_write(UC_ARM64_REG_LR, 0x1800fff0)
    regs = [UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
            UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5]
    for r, v in zip(regs, [0x4000000, 0x3008000, 0x84000000, 0x20000]):
        cpu.reg_write(r, v)
    trace, events = [], []

    def reg(i):
        return cpu.reg_read(regs[i])

    def ret(value=0):
        cpu.reg_write(UC_ARM64_REG_X0, value)
        cpu.reg_write(UC_ARM64_REG_PC, cpu.reg_read(UC_ARM64_REG_LR))

    def hook(uc, address, size, context):
        trace.append(hex(address))
        if len(trace) > 40:
            trace.pop(0)
        if address == 0x1474:
            ret(0x2000000)
        elif address in (0x2008, 0x18000000):
            dst, src, count = reg(0), reg(1), reg(2)
            assert count < 0x200000
            if count:
                cpu.mem_write(dst, bytes(cpu.mem_read(src, count)))
            ret(dst)
        elif address == 0x35b74:
            dst, count, value = reg(0), reg(1), reg(2) & 255
            assert count < 0x200000
            if count:
                cpu.mem_write(dst, bytes([value]) * count)
            ret(dst)
        elif address == 0x1da8:
            count = reg(0)
            assert 0 < count <= 1024
            events.append('zeroed temporary boot-device buffer')
            cpu.mem_write(0x300a000, bytes(count))
            ret(0x300a000)
        elif address in (0x29f8, 0x1e9c):
            ret()
        elif address == 0x113e8:  # GetRamPartitions(pointer-to-pointer, count)
            cpu.mem_write(reg(0), struct.pack('<Q', 0x3009000))
            cpu.mem_write(reg(1), struct.pack('<I', 2))
            cpu.mem_write(0x3009000, struct.pack('<4Q', 0x80000000, 0x80000000,
                                               0x100000000, 0x100000000))
            events.append('synthetic two-bank RAM fixture')
            ret()
        elif address == 0x115a0:
            events.append('optional memory granule unavailable')
            ret(0x800000000000000e)
        elif address == 0x18000010:
            events.append('optional LocateProtocol unavailable: ' + bytes(cpu.mem_read(reg(0), 16)).hex())
            ret(0x800000000000000e)
        elif address == 0x18000020:
            chars = bytearray()
            for offset in range(0, 512, 2):
                b = bytes(cpu.mem_read(reg(0) + offset, 2))
                if b == b'\0\0':
                    break
                chars.extend(b)
            name = chars.decode('utf-16le')
            events.append('GetVariable: ' + name)
            if name == 'BootDeviceBaseAddr':
                ret(0x800000000000000e)
                return
            assert name == 'DisplaySplashBufferInfo', name
            assert struct.unpack('<Q', cpu.mem_read(reg(3), 8))[0] >= 12
            cpu.mem_write(reg(4), struct.pack('<III', 1, 0x9d400000, 0x2400000))
            cpu.mem_write(reg(3), struct.pack('<Q', 12))
            ret()
        elif address in (0x10e40, 0x28cc):
            raise RuntimeError('Captured-code assertion: ' + hex(address))

    for address in [0x1474, 0x2008, 0x18000000, 0x35b74, 0x29f8, 0x1e9c,
                    0x113e8, 0x115a0, 0x18000010, 0x18000020, 0x10e40, 0x28cc,
                    0x1da8]:
        cpu.hook_add(UC_HOOK_CODE, hook, begin=address, end=address)
    result = {'input_sha256': hashlib.sha256(dtb).hexdigest(), 'fixture_events': events}
    try:
        cpu.emu_start(0x187a0, 0x1800fff0, timeout=10000000)
        result.update(returned=cpu.reg_read(UC_ARM64_REG_PC) == 0x1800fff0,
                      status=hex(reg(0)), pc=hex(cpu.reg_read(UC_ARM64_REG_PC)))
        if result['returned'] and reg(0) == 0:
            size = struct.unpack('>I', cpu.mem_read(0x4000004, 4))[0]
            nodes = read_fdt(bytes(cpu.mem_read(0x4000000, size)))
            result['output_bytes'] = size
            result['memory_reg'] = {k: cells(v['reg']) for k, v in nodes.items()
                                    if v.get('device_type') == b'memory\0' and 'reg' in v}
            result['chosen_bootargs'] = nodes['/chosen']['bootargs'].decode(errors='replace')
            result['initrd_start'] = cells(nodes['/chosen']['linux,initrd-start'])
            result['initrd_end'] = cells(nodes['/chosen']['linux,initrd-end'])
    except Exception as error:
        result.update(error=str(error), pc=hex(cpu.reg_read(UC_ARM64_REG_PC)), trace=trace)
    return result


def main():
    images = {'stock': ROOT / 'firmware/extracted/stock-dtbo-20260914/stock-00-merged.dtb',
              'diagnostic': ROOT / 'firmware/extracted/recovery-probe-20260914/merged-libufdt.dtb'}
    results = {k: fixup(p.read_bytes()) for k, p in images.items()}
    report = {'scope': 'Captured UpdateDeviceTree (0x187a0), memory/chosen edits and libfdt, offline',
              'results': results,
              'limits': 'Synthetic RAM banks and splash values; optional protocols and BootDeviceBaseAddr unavailable. Not live hardware or whole BootLinux emulation.'}
    report['passed'] = all(r.get('returned') and r.get('status') == '0x0' and 'error' not in r
                            for r in results.values())
    output = ROOT / 'firmware/extracted/diagnostic-boot-v1-20260915/dtb-fixup-emulation.json'
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    assert report['passed'], 'Inspect fixup fixture report'


if __name__ == '__main__':
    main()
